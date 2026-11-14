#define _GNU_SOURCE
#include "transaction_executor.h"
#include "resolve_allocator_boundary.h"
#include "common/target_identity.h"
#include "sha256.h"

#include <errno.h>
#include <limits.h>
#include <linux/membarrier.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#error "MAP_FIXED_NOREPLACE is required for safe relay placement"
#endif

#define RESOLVE_AV1_FOURCC UINT32_C(0x61763031)
#define RESOLVE_AV1_CODEC_ID UINT32_C(226)
#define RESOLVE_MAP_EXPECTED_SIZE 71U
#define RESOLVE_MAP_NEW_SIZE 72U
#define RESOLVE_NODE_BLACK 1U
#define RESOLVE_NODE_RED 0U

typedef struct {
    uintptr_t left;
    uintptr_t right;
    uintptr_t parent;
    uint8_t color;
    uint8_t padding[3];
    uint32_t key;
    uint32_t value;
    uint8_t tail_padding[4];
} ResolveTransactionNode;

typedef struct {
    uintptr_t begin;
    uintptr_t root;
    uint64_t size;
} ResolveTransactionMap;

typedef void (*ResolveRebalanceFunction)(ResolveTransactionNode *,
                                         ResolveTransactionNode *);

_Static_assert(sizeof(ResolveTransactionNode) == DAVINCI_RESOLVE_TREE_NODE_SIZE,
               "Resolve transaction node layout changed");
_Static_assert(offsetof(ResolveTransactionNode, parent) == 0x10U,
               "Resolve transaction node parent offset changed");
_Static_assert(offsetof(ResolveTransactionNode, color) == 0x18U,
               "Resolve transaction node color offset changed");
_Static_assert(offsetof(ResolveTransactionNode, key) == 0x1cU,
               "Resolve transaction node key offset changed");
_Static_assert(sizeof(ResolveTransactionMap) == 0x18U,
               "Resolve transaction map layout changed");
_Static_assert(sizeof(ResolveRebalanceFunction) == sizeof(uintptr_t),
               "Resolve rebalance function pointer width changed");

static const uint32_t g_expected_allocator_owners[8] = {
    DAVINCI_RESOLVE_OWNER_TBB_PROXY,
    DAVINCI_RESOLVE_OWNER_CODEX,
    DAVINCI_RESOLVE_OWNER_TBB_PROXY,
    DAVINCI_RESOLVE_OWNER_TBB_PROXY,
    DAVINCI_RESOLVE_OWNER_TBB_PROXY,
    DAVINCI_RESOLVE_OWNER_TBB_MALLOC,
    DAVINCI_RESOLVE_OWNER_TBB_MALLOC,
    DAVINCI_RESOLVE_OWNER_GLIBC
};

#ifdef DAVINCI_AV1_EXECUTOR_TESTING
static unsigned g_test_fail_after_stage;
void davinci_av1_executor_test_fail_after(unsigned stage) {
    g_test_fail_after_stage = stage;
}
#define EXECUTOR_TEST_FAIL_STAGE(stage) \
    (g_test_fail_after_stage != 0U && g_test_fail_after_stage == (stage))
#else
#define EXECUTOR_TEST_FAIL_STAGE(stage) false
#endif

static bool checked_range(uintptr_t address, size_t length) {
    return address != 0U && length != 0U && length <= UINTPTR_MAX - address;
}

static bool rel32_value(uintptr_t instruction_end, uintptr_t target,
                        int32_t *relative) {
    if (relative == NULL) return false;
    if (target >= instruction_end) {
        const uintptr_t delta = target - instruction_end;
        if (delta > (uintptr_t)INT32_MAX) return false;
        *relative = (int32_t)delta;
        return true;
    }

    const uintptr_t delta = instruction_end - target;
    if (delta > (uintptr_t)INT32_MAX + 1U) return false;
    *relative = delta == (uintptr_t)INT32_MAX + 1U
                    ? INT32_MIN : -(int32_t)delta;
    return true;
}

static bool decode_rel32_target(const DavinciResolveCodeSiteSnapshotV1 *site,
                                uintptr_t *target) {
    if (site == NULL || target == NULL || site->bytes[0] != 0xe8 ||
        site->address > UINTPTR_MAX - 5U)
        return false;

    int32_t relative = 0;
    memcpy(&relative, site->bytes + 1U, sizeof(relative));
    const uintptr_t end = site->address + 5U;
    if (relative >= 0) {
        if ((uintptr_t)relative > UINTPTR_MAX - end) return false;
        *target = end + (uintptr_t)relative;
    } else {
        const uintptr_t magnitude = (uintptr_t)(-(int64_t)relative);
        if (magnitude > end) return false;
        *target = end - magnitude;
    }
    return true;
}

static bool mapping_matches(const DavinciMappingIdentity *actual,
                            uintptr_t address, uintptr_t mapping_start,
                            uintptr_t mapping_end, uint64_t inode,
                            uint32_t device_major, uint32_t device_minor,
                            uint32_t protection) {
    return actual != NULL && actual->start == mapping_start &&
           actual->end == mapping_end && actual->inode == inode &&
           actual->device_major == device_major &&
           actual->device_minor == device_minor &&
           actual->protection == protection &&
           address >= actual->start && address < actual->end;
}

static bool mapping_record_matches(uintptr_t address,
                                   uintptr_t mapping_start,
                                   uintptr_t mapping_end, uint64_t inode,
                                   uint32_t device_major,
                                   uint32_t device_minor,
                                   uint32_t protection) {
    DavinciMappingIdentity actual;
    return davinci_find_mapping_identity(address, &actual) == 0 &&
           mapping_matches(&actual, address, mapping_start, mapping_end,
                           inode, device_major, device_minor, protection);
}

/* mprotect may split one original executable VMA into smaller adjacent VMAs.
 * After the exact original RX protection is restored, Linux is not required to
 * coalesce those VMAs back to their former boundaries. For patchable code pages
 * validate the captured backing-file identity and protection, while requiring
 * the original snapshot range to have covered the complete page. */
static bool mapping_record_covers(uintptr_t address, size_t length,
                                  uintptr_t original_mapping_start,
                                  uintptr_t original_mapping_end,
                                  uint64_t inode, uint32_t device_major,
                                  uint32_t device_minor,
                                  uint32_t protection) {
    if (!checked_range(address, length) ||
        address < original_mapping_start || address > original_mapping_end ||
        length > original_mapping_end - address)
        return false;

    DavinciMappingIdentity actual;
    return davinci_find_mapping_identity(address, &actual) == 0 &&
           address >= actual.start && length <= actual.end - address &&
           actual.inode == inode &&
           actual.device_major == device_major &&
           actual.device_minor == device_minor &&
           actual.protection == protection;
}

static bool snapshot_digest_valid(
    const DavinciResolveTransactionSnapshotV1 *snapshot) {
    if (snapshot == NULL ||
        snapshot->magic != DAVINCI_RESOLVE_SNAPSHOT_MAGIC ||
        snapshot->version != DAVINCI_RESOLVE_SNAPSHOT_VERSION ||
        snapshot->struct_size != sizeof(*snapshot) ||
        snapshot->node_count != DAVINCI_RESOLVE_TREE_NODE_COUNT ||
        snapshot->call_site_count != DAVINCI_RESOLVE_CALL_SITE_COUNT ||
        snapshot->transfer_site_count != DAVINCI_RESOLVE_TRANSFER_SITE_COUNT ||
        snapshot->page_count != DAVINCI_RESOLVE_CODE_PAGE_COUNT ||
        snapshot->map_page_count == 0U ||
        snapshot->map_page_count > DAVINCI_RESOLVE_MAP_PAGE_MAX ||
        snapshot->allocator_owner_count !=
            DAVINCI_RESOLVE_ALLOCATOR_OWNER_COUNT ||
        snapshot->system_page_size != DAVINCI_RESOLVE_PAGE_SIZE ||
        snapshot->relay_length != DAVINCI_RESOLVE_PAGE_SIZE ||
        snapshot->relay_candidate < UINT64_C(0x40000000) ||
        snapshot->relay_candidate >= UINT64_C(0x85e3deb5) ||
        (snapshot->relay_candidate & (DAVINCI_RESOLVE_PAGE_SIZE - 1U)) != 0U ||
        snapshot->relay_length > UINT64_C(0x85e3deb5) -
                                     snapshot->relay_candidate ||
        snapshot->callback_pointer_cell !=
            snapshot->relay_candidate + DAVINCI_AV1_RELAY_CALLBACK_CELL_OFFSET ||
        !checked_range(snapshot->relay_candidate, DAVINCI_RESOLVE_PAGE_SIZE))
        return false;

    uint8_t digest[32];
    DavinciSha256 context;
    davinci_sha256_init(&context);
    davinci_sha256_update(
        &context, snapshot,
        offsetof(DavinciResolveTransactionSnapshotV1, snapshot_sha256));
    davinci_sha256_final(&context, digest);
    return memcmp(digest, snapshot->snapshot_sha256, sizeof(digest)) == 0;
}

static bool validate_snapshot_mappings(
    const DavinciResolveTransactionSnapshotV1 *snapshot) {
    for (uint32_t index = 0U; index < snapshot->page_count; ++index) {
        const DavinciResolveCodePageSnapshotV1 *page =
            &snapshot->pages[index];
        if (page->length != DAVINCI_RESOLVE_PAGE_SIZE ||
            (page->address & (DAVINCI_RESOLVE_PAGE_SIZE - 1U)) != 0U ||
            page->protection != (PROT_READ | PROT_EXEC) ||
            page->reserved != 0U ||
            !mapping_record_covers(page->address, page->length,
                                   page->mapping_start, page->mapping_end,
                                   page->mapping_inode, page->device_major,
                                   page->device_minor, page->protection))
            return false;
        for (uint32_t previous = 0U; previous < index; ++previous) {
            if (snapshot->pages[previous].address == page->address)
                return false;
        }
    }

    for (uint32_t index = 0U; index < snapshot->map_page_count; ++index) {
        const DavinciResolveMapPageSnapshotV1 *page =
            &snapshot->map_pages[index];
        if (page->length != DAVINCI_RESOLVE_PAGE_SIZE ||
            (page->address & (DAVINCI_RESOLVE_PAGE_SIZE - 1U)) != 0U ||
            page->protection != (PROT_READ | PROT_WRITE) ||
            page->reserved != 0U ||
            !mapping_record_matches(page->address, page->mapping_start,
                                    page->mapping_end, page->mapping_inode,
                                    page->device_major, page->device_minor,
                                    page->protection))
            return false;
        for (uint32_t previous = 0U; previous < index; ++previous) {
            if (snapshot->map_pages[previous].address == page->address)
                return false;
        }
    }
    return true;
}

static bool validate_allocator_snapshot(
    const DavinciResolveTransactionSnapshotV1 *snapshot) {
    if (snapshot->allocator_values[0] == 0U ||
        snapshot->allocator_values[1] == 0U ||
        snapshot->allocator_values[7] != 1U ||
        snapshot->allocator_values[8] == 0U)
        return false;

    for (uint32_t index = 0U; index < 9U; ++index) {
        uint64_t actual = 0U;
        int protection = 0;
        if (!checked_range(snapshot->allocator_slot_addresses[index],
                           sizeof(actual)) ||
            davinci_find_mapping_range_protection(
                snapshot->allocator_slot_addresses[index], sizeof(actual),
                &protection) != 0 ||
            (protection & PROT_READ) == 0)
            return false;
        memcpy(&actual, (const void *)snapshot->allocator_slot_addresses[index],
               sizeof(actual));
        if (actual != snapshot->allocator_values[index]) return false;
    }

    const uintptr_t owner_targets[8] = {
        (uintptr_t)snapshot->allocator_values[0],
        (uintptr_t)snapshot->allocator_values[1],
        (uintptr_t)snapshot->allocator_values[2],
        (uintptr_t)snapshot->allocator_values[3],
        (uintptr_t)snapshot->allocator_values[4],
        (uintptr_t)snapshot->allocator_values[5],
        (uintptr_t)snapshot->allocator_values[6],
        (uintptr_t)snapshot->allocator_values[8]
    };
    for (uint32_t index = 0U; index < DAVINCI_RESOLVE_ALLOCATOR_OWNER_COUNT;
         ++index) {
        const DavinciResolveAllocatorOwnerSnapshotV1 *owner =
            &snapshot->allocator_owners[index];
        if (owner->target != owner_targets[index] ||
            owner->owner != g_expected_allocator_owners[index] ||
            owner->protection != (PROT_READ | PROT_EXEC) ||
            !mapping_record_matches(owner->target, owner->mapping_start,
                                    owner->mapping_end, owner->mapping_inode,
                                    owner->device_major, owner->device_minor,
                                    owner->protection))
            return false;
    }
    return true;
}

static bool map_page_covers(const DavinciResolveTransactionSnapshotV1 *snapshot,
                            uintptr_t address, size_t length) {
    if (!checked_range(address, length)) return false;
    const uintptr_t last = address + length - 1U;
    const uintptr_t first_page = address & ~(uintptr_t)(DAVINCI_RESOLVE_PAGE_SIZE - 1U);
    const uintptr_t last_page = last & ~(uintptr_t)(DAVINCI_RESOLVE_PAGE_SIZE - 1U);
    for (uintptr_t page = first_page;; page += DAVINCI_RESOLVE_PAGE_SIZE) {
        bool found = false;
        for (uint32_t index = 0U; index < snapshot->map_page_count; ++index) {
            if (snapshot->map_pages[index].address == page) {
                found = true;
                break;
            }
        }
        if (!found) return false;
        if (page == last_page) return true;
        if (page > UINTPTR_MAX - DAVINCI_RESOLVE_PAGE_SIZE) return false;
    }
}

static bool code_page_covers(const DavinciResolveTransactionSnapshotV1 *snapshot,
                             uintptr_t address, size_t length) {
    if (!checked_range(address, length)) return false;
    const uintptr_t last = address + length - 1U;
    const uintptr_t first_page = address & ~(uintptr_t)(DAVINCI_RESOLVE_PAGE_SIZE - 1U);
    const uintptr_t last_page = last & ~(uintptr_t)(DAVINCI_RESOLVE_PAGE_SIZE - 1U);
    for (uintptr_t page = first_page;; page += DAVINCI_RESOLVE_PAGE_SIZE) {
        bool found = false;
        for (uint32_t index = 0U; index < snapshot->page_count; ++index) {
            if (snapshot->pages[index].address == page) {
                found = true;
                break;
            }
        }
        if (!found) return false;
        if (page == last_page) return true;
        if (page > UINTPTR_MAX - DAVINCI_RESOLVE_PAGE_SIZE) return false;
    }
}

static bool tree_validate_node(
    uintptr_t address, uintptr_t expected_parent,
    uint64_t minimum_key, uint64_t maximum_key,
    uintptr_t seen[DAVINCI_RESOLVE_TREE_NODE_COUNT + 1U],
    uint32_t *seen_count, uint32_t *black_height,
    uintptr_t *minimum_node, uint32_t *minimum_key_value,
    bool *av01_found) {
    if (address == 0U) {
        *black_height = 1U;
        return true;
    }
    if (*seen_count >= DAVINCI_RESOLVE_TREE_NODE_COUNT + 1U ||
        !checked_range(address, sizeof(ResolveTransactionNode)))
        return false;
    for (uint32_t index = 0U; index < *seen_count; ++index) {
        if (seen[index] == address) return false;
    }
    seen[(*seen_count)++] = address;

    const ResolveTransactionNode *node =
        (const ResolveTransactionNode *)address;
    if (node->parent != expected_parent ||
        (uint64_t)node->key <= minimum_key ||
        (uint64_t)node->key >= maximum_key ||
        (node->color != RESOLVE_NODE_BLACK &&
         node->color != RESOLVE_NODE_RED))
        return false;

    if (*minimum_node == 0U || node->key < *minimum_key_value) {
        *minimum_node = address;
        *minimum_key_value = node->key;
    }
    if (node->key == RESOLVE_AV1_FOURCC) *av01_found = true;

    uint32_t left_black_height = 0U;
    uint32_t right_black_height = 0U;
    if (!tree_validate_node(node->left, address, minimum_key, node->key,
                            seen, seen_count, &left_black_height,
                            minimum_node, minimum_key_value, av01_found) ||
        !tree_validate_node(node->right, address, node->key, maximum_key,
                            seen, seen_count, &right_black_height,
                            minimum_node, minimum_key_value, av01_found))
        return false;
    if (left_black_height != right_black_height ||
        (node->color == RESOLVE_NODE_RED &&
         ((node->left != 0U &&
           ((ResolveTransactionNode *)node->left)->color == RESOLVE_NODE_RED) ||
          (node->right != 0U &&
           ((ResolveTransactionNode *)node->right)->color == RESOLVE_NODE_RED))))
        return false;
    *black_height = left_black_height +
                    (node->color == RESOLVE_NODE_BLACK ? 1U : 0U);
    return true;
}

static bool map_tree_valid(const ResolveTransactionMap *map,
                           uintptr_t map_address, uint64_t expected_size,
                           bool require_av01) {
    if (map == NULL) return false;
    if (map->root == 0U)
        return map->begin == 0U && map->size == 0U && expected_size == 0U;

    const uintptr_t sentinel = map_address + offsetof(ResolveTransactionMap, root);
    const ResolveTransactionNode *root =
        (const ResolveTransactionNode *)map->root;
    if (root->parent != sentinel || root->color != RESOLVE_NODE_BLACK)
        return false;

    uintptr_t seen[DAVINCI_RESOLVE_TREE_NODE_COUNT + 1U];
    uint32_t seen_count = 0U;
    uint32_t black_height = 0U;
    uintptr_t minimum_node = 0U;
    uint32_t minimum_key = 0U;
    bool av01_found = false;
    if (!tree_validate_node(map->root, sentinel, 0U, UINT64_MAX,
                            seen, &seen_count, &black_height,
                            &minimum_node, &minimum_key, &av01_found))
        return false;
    return map->size == expected_size && seen_count == expected_size &&
           map->begin == minimum_node && av01_found == require_av01;
}

static bool resolve_map_snapshot_matches(
    const DavinciResolveTransactionSnapshotV1 *snapshot) {
    const uintptr_t map_address = snapshot->generic_map;
    if (!checked_range(map_address, sizeof(ResolveTransactionMap)) ||
        !map_page_covers(snapshot, map_address, sizeof(ResolveTransactionMap)) ||
        memcmp((const void *)map_address, snapshot->original_map_header,
               sizeof(snapshot->original_map_header)) != 0)
        return false;

    const ResolveTransactionMap *map = (const ResolveTransactionMap *)map_address;
    if (map->size != RESOLVE_MAP_EXPECTED_SIZE || map->root == 0U ||
        map->begin == 0U ||
        !map_tree_valid(map, map_address, RESOLVE_MAP_EXPECTED_SIZE, false))
        return false;

    for (uint32_t index = 0U; index < snapshot->node_count; ++index) {
        const DavinciResolveTreeNodeSnapshotV1 *node =
            &snapshot->nodes[index];
        if (!checked_range(node->address, DAVINCI_RESOLVE_TREE_NODE_SIZE) ||
            !map_page_covers(snapshot, node->address,
                             DAVINCI_RESOLVE_TREE_NODE_SIZE) ||
            memcmp((const void *)node->address, node->bytes,
                   DAVINCI_RESOLVE_TREE_NODE_SIZE) != 0)
            return false;
        for (uint32_t previous = 0U; previous < index; ++previous) {
            if (snapshot->nodes[previous].address == node->address)
                return false;
        }
    }

    return true;
}

static bool live_code_snapshot_matches(
    const DavinciResolveTransactionSnapshotV1 *snapshot,
    const DavinciAv1TransactionBindingsV1 *bindings) {
    if (!resolve_map_snapshot_matches(snapshot) ||
        !code_page_covers(snapshot, bindings->callback_entry, 8U) ||
        ((bindings->callback_entry & 7U) != 0U) ||
        memcmp((const void *)bindings->callback_entry,
               snapshot->original_callback, 8U) != 0)
        return false;

    for (uint32_t index = 0U; index < snapshot->call_site_count; ++index) {
        const DavinciResolveCodeSiteSnapshotV1 *site =
            &snapshot->call_sites[index];
        if (site->dispatch_index >= DAVINCI_AV1_WRAPPER_COUNT ||
            site->reserved[0] != 0U || site->reserved[1] != 0U ||
            site->bytes[0] != 0xe8 || !code_page_covers(snapshot, site->address, 5U) ||
            memcmp((const void *)site->address, site->bytes, 5U) != 0)
            return false;
    }
    for (uint32_t index = 0U; index < snapshot->transfer_site_count; ++index) {
        const DavinciResolveCodeSiteSnapshotV1 *site =
            &snapshot->transfer_sites[index];
        DavinciMappingIdentity transfer_mapping;
        if (site->dispatch_index != UINT8_MAX || site->reserved[0] != 0U ||
            site->reserved[1] != 0U || !checked_range(site->address, 5U) ||
            davinci_find_mapping_identity(site->address, &transfer_mapping) != 0 ||
            transfer_mapping.protection != (PROT_READ | PROT_EXEC) ||
            transfer_mapping.inode != snapshot->pages[0].mapping_inode ||
            transfer_mapping.device_major != snapshot->pages[0].device_major ||
            transfer_mapping.device_minor != snapshot->pages[0].device_minor ||
            memcmp((const void *)site->address, site->bytes, 5U) != 0)
            return false;
    }
    return true;
}

static bool validate_bindings(
    const DavinciResolveTransactionSnapshotV1 *snapshot,
    const DavinciAv1TransactionBindingsV1 *bindings) {
    if (bindings == NULL || bindings->callback_entry == 0U ||
        bindings->callback_target == 0U ||
        bindings->allocator_target != snapshot->allocator_values[0] ||
        bindings->sized_delete_target != snapshot->allocator_values[1])
        return false;

    uintptr_t rebalancer_target = 0U;
    if (!decode_rel32_target(&snapshot->transfer_sites[0],
                             &rebalancer_target) ||
        bindings->rebalancer_target != rebalancer_target)
        return false;

    int protection = 0;
    if (davinci_find_mapping_range_protection(bindings->callback_target, 1U,
                                              &protection) != 0 ||
        (protection & (PROT_READ | PROT_EXEC)) !=
            (PROT_READ | PROT_EXEC))
        return false;
    if (davinci_find_mapping_range_protection(bindings->rebalancer_target, 1U,
                                              &protection) != 0 ||
        (protection & (PROT_READ | PROT_EXEC)) !=
            (PROT_READ | PROT_EXEC))
        return false;
    for (uint32_t index = 0U; index < DAVINCI_AV1_WRAPPER_COUNT; ++index) {
        if (bindings->wrapper_targets[index] == 0U ||
            davinci_find_mapping_range_protection(
                bindings->wrapper_targets[index], 1U, &protection) != 0 ||
            (protection & (PROT_READ | PROT_EXEC)) !=
                (PROT_READ | PROT_EXEC))
            return false;
    }
    return true;
}

static int synchronize_instruction_stream(void) {
#ifdef SYS_membarrier
    const long registered = syscall(
        SYS_membarrier, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE,
        0, 0);
    if (registered != 0 && errno != EBUSY) return -1;
    return syscall(SYS_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE,
                   0, 0) == 0 ? 0 : -1;
#else
    errno = ENOTSUP;
    return -1;
#endif
}

static int make_code_pages_writable(
    const DavinciResolveTransactionSnapshotV1 *snapshot,
    DavinciAv1TransactionExecutionV1 *execution) {
    for (uint32_t index = 0U; index < snapshot->page_count; ++index) {
        const uint32_t bit = UINT32_C(1) << index;
        if ((execution->code_page_writable_mask & bit) != 0U) continue;
        const DavinciResolveCodePageSnapshotV1 *page =
            &snapshot->pages[index];
        if (mprotect((void *)page->address, page->length,
                     (int)page->protection | PROT_WRITE) != 0)
            return -1;
        execution->code_page_writable_mask |= bit;
    }
    return 0;
}

static int restore_code_page_protections(
    const DavinciResolveTransactionSnapshotV1 *snapshot,
    DavinciAv1TransactionExecutionV1 *execution) {
    int result = 0;
    for (uint32_t index = 0U; index < snapshot->page_count; ++index) {
        const uint32_t bit = UINT32_C(1) << index;
        if ((execution->code_page_writable_mask & bit) == 0U) continue;
        const DavinciResolveCodePageSnapshotV1 *page =
            &snapshot->pages[index];
        if (mprotect((void *)page->address, page->length,
                     (int)page->protection) != 0) {
            result = -1;
            continue;
        }
        execution->code_page_writable_mask &= ~bit;
    }
    return result;
}

static int build_relay(
    const DavinciResolveTransactionSnapshotV1 *snapshot,
    const DavinciAv1TransactionBindingsV1 *bindings,
    DavinciAv1TransactionExecutionV1 *execution) {
    void *mapping = mmap((void *)snapshot->relay_candidate,
                         snapshot->relay_length, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                         -1, 0);
    if (mapping == MAP_FAILED ||
        (uintptr_t)mapping != snapshot->relay_candidate) {
        if (mapping != MAP_FAILED)
            (void)munmap(mapping, snapshot->relay_length);
        return -1;
    }
    execution->relay_page = (uintptr_t)mapping;

    uint8_t *relay = (uint8_t *)mapping;
    memcpy(relay + DAVINCI_AV1_RELAY_CALLBACK_CELL_OFFSET,
           &bindings->callback_target, sizeof(bindings->callback_target));

    for (uint32_t index = 0U; index < DAVINCI_AV1_WRAPPER_COUNT; ++index) {
        const uintptr_t stub_address = execution->relay_page +
            index * DAVINCI_AV1_RELAY_STUB_STRIDE;
        const uintptr_t pointer_cell = execution->relay_page +
            DAVINCI_AV1_RELAY_WRAPPER_TABLE_OFFSET +
            index * sizeof(uintptr_t);
        memcpy(relay + (pointer_cell - execution->relay_page),
               &bindings->wrapper_targets[index], sizeof(uintptr_t));

        int32_t displacement = 0;
        if (!rel32_value(stub_address + 6U, pointer_cell, &displacement))
            return -1;
        uint8_t stub[DAVINCI_AV1_RELAY_STUB_STRIDE];
        memset(stub, 0x90, sizeof(stub));
        stub[0] = 0xff;
        stub[1] = 0x25;
        memcpy(stub + 2U, &displacement, sizeof(displacement));
        memcpy(relay + index * DAVINCI_AV1_RELAY_STUB_STRIDE,
               stub, sizeof(stub));
    }

    __builtin___clear_cache((char *)relay,
                            (char *)relay + snapshot->relay_length);
    return mprotect(mapping, snapshot->relay_length,
                    PROT_READ | PROT_EXEC);
}

static int patch_call_site(
    const DavinciResolveTransactionSnapshotV1 *snapshot,
    DavinciAv1TransactionExecutionV1 *execution, uint32_t index) {
    if (index >= snapshot->call_site_count) return -1;
    const DavinciResolveCodeSiteSnapshotV1 *site =
        &snapshot->call_sites[index];
    if (memcmp((const void *)site->address, site->bytes, 5U) != 0)
        return -1;
    const uintptr_t stub = execution->relay_page +
        site->dispatch_index * DAVINCI_AV1_RELAY_STUB_STRIDE;
    int32_t displacement = 0;
    if (!rel32_value(site->address + 5U, stub, &displacement))
        return -1;
    uint8_t patch[5] = {0xe8, 0, 0, 0, 0};
    memcpy(patch + 1U, &displacement, sizeof(displacement));
    memcpy((void *)site->address, patch, sizeof(patch));
    __builtin___clear_cache((char *)site->address,
                            (char *)site->address + sizeof(patch));
    execution->call_patch_mask |= UINT32_C(1) << index;
    return 0;
}

static int insert_av1_map_node(
    const DavinciResolveTransactionSnapshotV1 *snapshot,
    const DavinciAv1TransactionBindingsV1 *bindings,
    DavinciAv1TransactionExecutionV1 *execution) {
    ResolveTransactionMap *map =
        (ResolveTransactionMap *)snapshot->generic_map;
    ResolveTransactionNode *sentinel =
        (ResolveTransactionNode *)(snapshot->generic_map +
            offsetof(ResolveTransactionMap, root));
    uintptr_t *link_address = &map->root;
    uintptr_t parent_address = (uintptr_t)sentinel;

    while (*link_address != 0U) {
        ResolveTransactionNode *parent =
            (ResolveTransactionNode *)*link_address;
        parent_address = (uintptr_t)parent;
        if (RESOLVE_AV1_FOURCC < parent->key)
            link_address = &parent->left;
        else if (RESOLVE_AV1_FOURCC > parent->key)
            link_address = &parent->right;
        else
            return -1;
    }

    if (execution->map_node_allocated == 0U ||
        execution->inserted_node == 0U ||
        (execution->inserted_node & (sizeof(uintptr_t) - 1U)) != 0U)
        return -1;

    int protection = 0;
    if (davinci_find_mapping_range_protection(
            execution->inserted_node, DAVINCI_RESOLVE_TREE_NODE_SIZE,
            &protection) != 0 ||
        (protection & (PROT_READ | PROT_WRITE)) !=
            (PROT_READ | PROT_WRITE))
        return -1;

    ResolveTransactionNode *node =
        (ResolveTransactionNode *)execution->inserted_node;
    node->left = 0U;
    node->right = 0U;
    node->parent = parent_address;
    const uint32_t key_value[2] = {RESOLVE_AV1_FOURCC,
                                   RESOLVE_AV1_CODEC_ID};
    memcpy(&node->key, key_value, sizeof(key_value));
    *link_address = (uintptr_t)node;

    if (map->begin == 0U ||
        RESOLVE_AV1_FOURCC <
            ((ResolveTransactionNode *)map->begin)->key)
        map->begin = (uintptr_t)node;

    ResolveRebalanceFunction rebalance = NULL;
    memcpy(&rebalance, &bindings->rebalancer_target, sizeof(rebalance));
    rebalance((ResolveTransactionNode *)map->root, node);
    ++map->size;
    execution->map_node_linked = 1U;

    if (!map_tree_valid(map, snapshot->generic_map,
                        RESOLVE_MAP_NEW_SIZE, true))
        return -1;
    for (uint32_t index = 0U; index < snapshot->node_count; ++index) {
        const ResolveTransactionNode *old_node =
            (const ResolveTransactionNode *)snapshot->nodes[index].address;
        uint32_t original_key_value[2];
        memcpy(original_key_value, snapshot->nodes[index].bytes + 0x1cU,
               sizeof(original_key_value));
        if (old_node->key != original_key_value[0] ||
            old_node->value != original_key_value[1])
            return -1;
    }
    return 0;
}

static bool installed_state_valid(
    const DavinciResolveTransactionSnapshotV1 *snapshot,
    const DavinciAv1TransactionBindingsV1 *bindings,
    const DavinciAv1TransactionExecutionV1 *execution) {
    const ResolveTransactionMap *map =
        (const ResolveTransactionMap *)snapshot->generic_map;
    if (execution->state != DAVINCI_AV1_EXECUTION_PREPARING ||
        execution->call_patch_mask != ((UINT32_C(1) << 10U) - 1U) ||
        execution->map_node_linked == 0U ||
        execution->callback_published == 0U ||
        !map_tree_valid(map, snapshot->generic_map, RESOLVE_MAP_NEW_SIZE, true))
        return false;

    const ResolveTransactionNode *node = (const ResolveTransactionNode *)
        (uintptr_t)execution->inserted_node;
    if (node->key != RESOLVE_AV1_FOURCC ||
        node->value != RESOLVE_AV1_CODEC_ID)
        return false;

    const uint8_t *callback = (const uint8_t *)bindings->callback_entry;
    if (callback[0] != 0xffU || callback[1] != 0x25U ||
        callback[6] != 0x90U || callback[7] != 0x90U)
        return false;
    int32_t callback_relative = 0;
    memcpy(&callback_relative, callback + 2U, sizeof(callback_relative));
    const uintptr_t callback_cell = bindings->callback_entry + 6U +
                                    (intptr_t)callback_relative;
    if (callback_cell != snapshot->callback_pointer_cell) return false;

    for (uint32_t index = 0U; index < snapshot->call_site_count; ++index) {
        const DavinciResolveCodeSiteSnapshotV1 *site =
            &snapshot->call_sites[index];
        const uint8_t *bytes = (const uint8_t *)site->address;
        if (bytes[0] != 0xe8) return false;
        int32_t relative = 0;
        memcpy(&relative, bytes + 1U, sizeof(relative));
        const uintptr_t target = site->address + 5U + (intptr_t)relative;
        if (target != execution->relay_page +
                          site->dispatch_index * DAVINCI_AV1_RELAY_STUB_STRIDE)
            return false;
    }

    uintptr_t callback_target = 0U;
    memcpy(&callback_target,
           (const void *)snapshot->callback_pointer_cell,
           sizeof(callback_target));
    if (callback_target != bindings->callback_target) return false;
    for (uint32_t index = 0U; index < DAVINCI_AV1_WRAPPER_COUNT; ++index) {
        uintptr_t wrapper = 0U;
        memcpy(&wrapper,
               (const void *)(execution->relay_page +
                   DAVINCI_AV1_RELAY_WRAPPER_TABLE_OFFSET +
                   index * sizeof(uintptr_t)), sizeof(wrapper));
        if (wrapper != bindings->wrapper_targets[index]) return false;
    }

    for (uint32_t index = 0U; index < snapshot->page_count; ++index) {
        int protection = 0;
        if (davinci_find_mapping_range_protection(
                snapshot->pages[index].address,
                snapshot->pages[index].length, &protection) != 0 ||
            protection != (int)snapshot->pages[index].protection)
            return false;
    }
    int relay_protection = 0;
    return davinci_find_mapping_range_protection(
               execution->relay_page, snapshot->relay_length,
               &relay_protection) == 0 &&
           relay_protection == (PROT_READ | PROT_EXEC);
}

static int restore_map_snapshot(
    const DavinciResolveTransactionSnapshotV1 *snapshot) {
    ResolveTransactionMap *map =
        (ResolveTransactionMap *)snapshot->generic_map;
    if (!map_page_covers(snapshot, snapshot->generic_map,
                         sizeof(snapshot->original_map_header)))
        return -1;
    for (uint32_t index = 0U; index < snapshot->node_count; ++index) {
        if (!map_page_covers(snapshot, snapshot->nodes[index].address,
                             DAVINCI_RESOLVE_TREE_NODE_SIZE))
            return -1;
    }

    for (uint32_t index = 0U; index < snapshot->node_count; ++index) {
        memcpy((void *)snapshot->nodes[index].address,
               snapshot->nodes[index].bytes,
               DAVINCI_RESOLVE_TREE_NODE_SIZE);
    }
    memcpy(map, snapshot->original_map_header,
           sizeof(snapshot->original_map_header));
    return 0;
}

static bool original_state_valid(
    const DavinciResolveTransactionSnapshotV1 *snapshot,
    const DavinciAv1TransactionBindingsV1 *bindings) {
    if (!resolve_map_snapshot_matches(snapshot) ||
        !validate_snapshot_mappings(snapshot) ||
        !validate_allocator_snapshot(snapshot) ||
        memcmp((const void *)bindings->callback_entry,
               snapshot->original_callback, 8U) != 0)
        return false;
    for (uint32_t index = 0U; index < snapshot->call_site_count; ++index) {
        if (memcmp((const void *)snapshot->call_sites[index].address,
                   snapshot->call_sites[index].bytes, 5U) != 0)
            return false;
    }
    for (uint32_t index = 0U; index < snapshot->transfer_site_count; ++index) {
        if (memcmp((const void *)snapshot->transfer_sites[index].address,
                   snapshot->transfer_sites[index].bytes, 5U) != 0)
            return false;
    }
    for (uint32_t index = 0U; index < snapshot->page_count; ++index) {
        int protection = 0;
        if (davinci_find_mapping_range_protection(
                snapshot->pages[index].address, snapshot->pages[index].length,
                &protection) != 0 ||
            protection != (int)snapshot->pages[index].protection)
            return false;
    }
    return true;
}

static bool relay_is_unmapped(uintptr_t address, size_t length) {
    unsigned char residency = 0U;
    if (mincore((void *)address, length, &residency) == 0) return false;
    return errno == ENOMEM;
}

static int rollback_internal(
    const DavinciResolveTransactionSnapshotV1 *snapshot,
    const DavinciAv1TransactionBindingsV1 *bindings,
    DavinciAv1TransactionExecutionV1 *execution) {
    const bool relay_was_owned = execution->relay_page != 0U;
    const bool code_was_modified = execution->callback_published != 0U ||
                                   execution->call_patch_mask != 0U;
    if (code_was_modified) {
        if (make_code_pages_writable(snapshot, execution) != 0)
            goto fail_stop;
    }

    if (execution->callback_published != 0U) {
        uint64_t original_word = 0U;
        memcpy(&original_word, snapshot->original_callback,
               sizeof(original_word));
        __atomic_store_n((uint64_t *)bindings->callback_entry,
                         original_word, __ATOMIC_SEQ_CST);
        __builtin___clear_cache((char *)bindings->callback_entry,
                                (char *)bindings->callback_entry + 8U);
        execution->callback_published = 0U;
    }

    for (uint32_t reverse = snapshot->call_site_count; reverse > 0U; --reverse) {
        const uint32_t index = reverse - 1U;
        const uint32_t bit = UINT32_C(1) << index;
        if ((execution->call_patch_mask & bit) == 0U) continue;
        const DavinciResolveCodeSiteSnapshotV1 *site =
            &snapshot->call_sites[index];
        memcpy((void *)site->address, site->bytes, sizeof(site->bytes));
        __builtin___clear_cache((char *)site->address,
                                (char *)site->address + sizeof(site->bytes));
        execution->call_patch_mask &= ~bit;
    }

    if (code_was_modified && synchronize_instruction_stream() != 0)
        goto fail_stop;

    if (execution->map_node_linked != 0U) {
        if (restore_map_snapshot(snapshot) != 0) goto fail_stop;
        execution->map_node_linked = 0U;
    }

    if (execution->map_node_allocated != 0U) {
        if (davinci_av1_free_resolve_node_noexcept(
                bindings->sized_delete_target,
                (void *)execution->inserted_node,
                DAVINCI_RESOLVE_TREE_NODE_SIZE) != 0)
            goto fail_stop;
        execution->map_node_allocated = 0U;
        execution->inserted_node = 0U;
    }

    if (restore_code_page_protections(snapshot, execution) != 0)
        goto fail_stop;

    if (execution->relay_page != 0U) {
        if (munmap((void *)execution->relay_page, snapshot->relay_length) != 0)
            goto fail_stop;
        execution->relay_page = 0U;
    }

    if (!original_state_valid(snapshot, bindings) ||
        (relay_was_owned &&
         !relay_is_unmapped(snapshot->relay_candidate, snapshot->relay_length)))
        goto fail_stop;

    execution->state = DAVINCI_AV1_EXECUTION_ROLLED_BACK;
    return 0;

fail_stop:
    execution->state = DAVINCI_AV1_EXECUTION_FAIL_STOP;
    return -1;
}

int davinci_av1_rollback_prepared_transaction(
    const DavinciResolveTransactionSnapshotV1 *snapshot,
    const DavinciAv1TransactionBindingsV1 *bindings,
    DavinciAv1TransactionExecutionV1 *execution) {
    if (snapshot == NULL || bindings == NULL || execution == NULL ||
        execution->magic != DAVINCI_AV1_EXECUTION_MAGIC ||
        execution->version != DAVINCI_AV1_EXECUTION_VERSION ||
        execution->struct_size != sizeof(*execution) ||
        (execution->state != DAVINCI_AV1_EXECUTION_COMMITTED &&
         execution->state != DAVINCI_AV1_EXECUTION_PREPARING))
        return DAVINCI_AV1_EXECUTION_NOT_COMMITTED;
    if (rollback_internal(snapshot, bindings, execution) != 0)
        return DAVINCI_AV1_EXECUTION_ROLLBACK_FAILED;
    return DAVINCI_AV1_EXECUTION_OK;
}

int davinci_av1_execute_prepared_transaction(
    const DavinciResolveTransactionSnapshotV1 *snapshot,
    const DavinciAv1TransactionBindingsV1 *bindings,
    DavinciAv1TransactionExecutionV1 *execution) {
    if (snapshot == NULL || bindings == NULL || execution == NULL)
        return DAVINCI_AV1_EXECUTION_INVALID_ARGUMENT;
    if (!snapshot_digest_valid(snapshot))
        return DAVINCI_AV1_EXECUTION_SNAPSHOT_INVALID;
    if (execution->state != DAVINCI_AV1_EXECUTION_EMPTY)
        return DAVINCI_AV1_EXECUTION_INVALID_ARGUMENT;
    memset(execution, 0, sizeof(*execution));
    execution->magic = DAVINCI_AV1_EXECUTION_MAGIC;
    execution->version = DAVINCI_AV1_EXECUTION_VERSION;
    execution->struct_size = sizeof(*execution);
    execution->state = DAVINCI_AV1_EXECUTION_PREPARING;

    uint32_t thread_count = 0U;
    if (davinci_count_self_threads(&thread_count) != 0 || thread_count != 1U ||
        !validate_snapshot_mappings(snapshot) ||
        !validate_allocator_snapshot(snapshot) ||
        !validate_bindings(snapshot, bindings) ||
        !live_code_snapshot_matches(snapshot, bindings)) {
        execution->state = DAVINCI_AV1_EXECUTION_EMPTY;
        return DAVINCI_AV1_EXECUTION_TARGET_MISMATCH;
    }

    if (synchronize_instruction_stream() != 0) {
        execution->state = DAVINCI_AV1_EXECUTION_EMPTY;
        return DAVINCI_AV1_EXECUTION_SYNC_FAILED;
    }

    if (build_relay(snapshot, bindings, execution) != 0) {
        const int rollback = rollback_internal(snapshot, bindings, execution);
        return rollback == 0 ? DAVINCI_AV1_EXECUTION_RELAY_FAILED
                             : DAVINCI_AV1_EXECUTION_ROLLBACK_FAILED;
    }

    execution->inserted_node = (uintptr_t)davinci_av1_allocate_resolve_node_noexcept(
        bindings->allocator_target, DAVINCI_RESOLVE_TREE_NODE_SIZE);
    if (execution->inserted_node == 0U) {
        const int rollback = rollback_internal(snapshot, bindings, execution);
        return rollback == 0 ? DAVINCI_AV1_EXECUTION_ALLOCATOR_FAILED
                             : DAVINCI_AV1_EXECUTION_ROLLBACK_FAILED;
    }
    execution->map_node_allocated = 1U;
    int allocation_protection = 0;
    if ((execution->inserted_node & (sizeof(uintptr_t) - 1U)) != 0U ||
        davinci_find_mapping_range_protection(
            execution->inserted_node, DAVINCI_RESOLVE_TREE_NODE_SIZE,
            &allocation_protection) != 0 ||
        (allocation_protection & (PROT_READ | PROT_WRITE)) !=
            (PROT_READ | PROT_WRITE)) {
        const int rollback = rollback_internal(snapshot, bindings, execution);
        return rollback == 0 ? DAVINCI_AV1_EXECUTION_ALLOCATOR_FAILED
                             : DAVINCI_AV1_EXECUTION_ROLLBACK_FAILED;
    }

    if (make_code_pages_writable(snapshot, execution) != 0) {
        const int rollback = rollback_internal(snapshot, bindings, execution);
        return rollback == 0 ? DAVINCI_AV1_EXECUTION_PROTECTION_FAILED
                             : DAVINCI_AV1_EXECUTION_ROLLBACK_FAILED;
    }

    for (uint32_t index = 0U; index < snapshot->call_site_count; ++index) {
        if (patch_call_site(snapshot, execution, index) != 0) {
            const int rollback = rollback_internal(snapshot, bindings, execution);
            return rollback == 0 ? DAVINCI_AV1_EXECUTION_PATCH_FAILED
                                 : DAVINCI_AV1_EXECUTION_ROLLBACK_FAILED;
        }
        if (EXECUTOR_TEST_FAIL_STAGE(index + 1U)) {
            const int rollback = rollback_internal(snapshot, bindings, execution);
            return rollback == 0 ? DAVINCI_AV1_EXECUTION_TEST_INJECTED
                                 : DAVINCI_AV1_EXECUTION_ROLLBACK_FAILED;
        }
    }

    if (insert_av1_map_node(snapshot, bindings, execution) != 0) {
        const int rollback = rollback_internal(snapshot, bindings, execution);
        return rollback == 0 ? DAVINCI_AV1_EXECUTION_MAP_FAILED
                             : DAVINCI_AV1_EXECUTION_ROLLBACK_FAILED;
    }
    if (EXECUTOR_TEST_FAIL_STAGE(11U)) {
        const int rollback = rollback_internal(snapshot, bindings, execution);
        return rollback == 0 ? DAVINCI_AV1_EXECUTION_TEST_INJECTED
                             : DAVINCI_AV1_EXECUTION_ROLLBACK_FAILED;
    }

    int32_t callback_displacement = 0;
    if (!rel32_value(bindings->callback_entry + 6U,
                     snapshot->callback_pointer_cell,
                     &callback_displacement)) {
        const int rollback = rollback_internal(snapshot, bindings, execution);
        return rollback == 0 ? DAVINCI_AV1_EXECUTION_PATCH_FAILED
                             : DAVINCI_AV1_EXECUTION_ROLLBACK_FAILED;
    }
    uint8_t callback_patch[8] = {0xff, 0x25, 0, 0, 0, 0, 0x90, 0x90};
    memcpy(callback_patch + 2U, &callback_displacement,
           sizeof(callback_displacement));
    uint64_t callback_word = 0U;
    memcpy(&callback_word, callback_patch, sizeof(callback_word));
    __atomic_store_n((uint64_t *)bindings->callback_entry, callback_word,
                     __ATOMIC_SEQ_CST);
    __builtin___clear_cache((char *)bindings->callback_entry,
                            (char *)bindings->callback_entry + sizeof(callback_patch));
    execution->callback_published = 1U;

    if (EXECUTOR_TEST_FAIL_STAGE(12U)) {
        const int rollback = rollback_internal(snapshot, bindings, execution);
        return rollback == 0 ? DAVINCI_AV1_EXECUTION_TEST_INJECTED
                             : DAVINCI_AV1_EXECUTION_ROLLBACK_FAILED;
    }

    if (synchronize_instruction_stream() != 0 ||
        restore_code_page_protections(snapshot, execution) != 0) {
        const int rollback = rollback_internal(snapshot, bindings, execution);
        return rollback == 0 ? DAVINCI_AV1_EXECUTION_SYNC_FAILED
                             : DAVINCI_AV1_EXECUTION_ROLLBACK_FAILED;
    }

    if (!installed_state_valid(snapshot, bindings, execution)) {
        const int rollback = rollback_internal(snapshot, bindings, execution);
        return rollback == 0 ? DAVINCI_AV1_EXECUTION_VERIFY_FAILED
                             : DAVINCI_AV1_EXECUTION_ROLLBACK_FAILED;
    }

    execution->state = DAVINCI_AV1_EXECUTION_COMMITTED;
    return DAVINCI_AV1_EXECUTION_OK;
}
