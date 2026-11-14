#define _GNU_SOURCE
#include "installer/transaction_executor.h"
#include "installer/resolve_allocator_boundary.h"
#include "installer/common/target_identity.h"
#include "installer/common/sha256.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define main embedded_fourcc_probe_main
#include "support/direct_insert_probe.c"
#undef main

static ResolveMap g_test_map;
static uint64_t g_fake_got[9];
static uint8_t *g_code_mapping;
static uint8_t *g_transfer_mapping;
static uintptr_t g_code_address;
static uintptr_t g_transfer_address;
static size_t g_test_page_size;
static DavinciResolveTransactionSnapshotV1 g_snapshot;
static DavinciAv1TransactionBindingsV1 g_bindings;

static int fail(const char *message) {
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

typedef void *(*TestAllocateFunction)(size_t);
typedef void (*TestDeleteFunction)(void *, size_t);
typedef void (*TestFreeFunction)(void *);
typedef void (*TestVoidFunction)(void);
typedef uintptr_t (*TestWrapperFunction)(uintptr_t, uintptr_t, uintptr_t);
typedef uintptr_t (*TestCallbackFunction)(uintptr_t, uintptr_t, uintptr_t);
typedef void (*TestRebalanceFunction)(Node *, Node *);

static void *test_scalar_new(size_t size);
static void test_sized_delete(void *node, size_t size);
static void test_original_free(void *node);
static void test_codex_delete(void);
static void test_codex_malloc(void);
static void test_codex_free(void);
static void test_scalable_malloc(void);
static void test_safe_free(void);
static uintptr_t test_callback(uintptr_t operation, uintptr_t properties,
                               uintptr_t reserved);
static uintptr_t test_wrapper_0(uintptr_t a, uintptr_t b, uintptr_t c);
static uintptr_t test_wrapper_1(uintptr_t a, uintptr_t b, uintptr_t c);
static uintptr_t test_wrapper_2(uintptr_t a, uintptr_t b, uintptr_t c);
static uintptr_t test_wrapper_3(uintptr_t a, uintptr_t b, uintptr_t c);
static uintptr_t test_wrapper_4(uintptr_t a, uintptr_t b, uintptr_t c);
static uintptr_t test_wrapper_5(uintptr_t a, uintptr_t b, uintptr_t c);
static uintptr_t test_wrapper_6(uintptr_t a, uintptr_t b, uintptr_t c);
static uintptr_t test_wrapper_7(uintptr_t a, uintptr_t b, uintptr_t c);
static void fake_rebalance_entry(Node *root, Node *node);

#define DEFINE_FUNCTION_ADDRESS(name, type, function) \
    static uintptr_t name(void) { \
        type function_pointer = function; \
        uintptr_t address = 0U; \
        _Static_assert(sizeof(function_pointer) == sizeof(address), \
                       "function pointer width changed"); \
        memcpy(&address, &function_pointer, sizeof(address)); \
        return address; \
    }

DEFINE_FUNCTION_ADDRESS(address_scalar_new, TestAllocateFunction, test_scalar_new)
DEFINE_FUNCTION_ADDRESS(address_sized_delete, TestDeleteFunction, test_sized_delete)
DEFINE_FUNCTION_ADDRESS(address_original_free, TestFreeFunction, test_original_free)
DEFINE_FUNCTION_ADDRESS(address_codex_delete, TestVoidFunction, test_codex_delete)
DEFINE_FUNCTION_ADDRESS(address_codex_malloc, TestVoidFunction, test_codex_malloc)
DEFINE_FUNCTION_ADDRESS(address_codex_free, TestVoidFunction, test_codex_free)
DEFINE_FUNCTION_ADDRESS(address_scalable_malloc, TestVoidFunction, test_scalable_malloc)
DEFINE_FUNCTION_ADDRESS(address_safe_free, TestVoidFunction, test_safe_free)
DEFINE_FUNCTION_ADDRESS(address_callback, TestCallbackFunction, test_callback)
DEFINE_FUNCTION_ADDRESS(address_fake_rebalance_entry,
                       TestRebalanceFunction, fake_rebalance_entry)
DEFINE_FUNCTION_ADDRESS(address_wrapper_0, TestWrapperFunction, test_wrapper_0)
DEFINE_FUNCTION_ADDRESS(address_wrapper_1, TestWrapperFunction, test_wrapper_1)
DEFINE_FUNCTION_ADDRESS(address_wrapper_2, TestWrapperFunction, test_wrapper_2)
DEFINE_FUNCTION_ADDRESS(address_wrapper_3, TestWrapperFunction, test_wrapper_3)
DEFINE_FUNCTION_ADDRESS(address_wrapper_4, TestWrapperFunction, test_wrapper_4)
DEFINE_FUNCTION_ADDRESS(address_wrapper_5, TestWrapperFunction, test_wrapper_5)
DEFINE_FUNCTION_ADDRESS(address_wrapper_6, TestWrapperFunction, test_wrapper_6)
DEFINE_FUNCTION_ADDRESS(address_wrapper_7, TestWrapperFunction, test_wrapper_7)

#undef DEFINE_FUNCTION_ADDRESS

static bool test_relay_is_unmapped(uintptr_t address, size_t length) {
    unsigned char residency = 0U;
    if (mincore((void *)address, length, &residency) == 0) return false;
    return errno == ENOMEM;
}

static void *test_scalar_new(size_t size) {
    if (size != sizeof(Node)) return NULL;
    return allocate_node();
}

static void test_sized_delete(void *node, size_t size) {
    if (size == sizeof(Node)) free(node);
}

static void test_original_free(void *node) { free(node); }

static void test_codex_delete(void) {}
static void test_codex_malloc(void) {}
static void test_codex_free(void) {}
static void test_scalable_malloc(void) {}
static void test_safe_free(void) {}

static uintptr_t test_callback(uintptr_t operation, uintptr_t properties,
                               uintptr_t reserved) {
    return operation + properties + reserved;
}

#define DECLARE_WRAPPER(index) \
    static uintptr_t test_wrapper_##index(uintptr_t a, uintptr_t b, uintptr_t c) { \
        return a ^ b ^ c ^ (uintptr_t)(index); \
    }
DECLARE_WRAPPER(0)
DECLARE_WRAPPER(1)
DECLARE_WRAPPER(2)
DECLARE_WRAPPER(3)
DECLARE_WRAPPER(4)
DECLARE_WRAPPER(5)
DECLARE_WRAPPER(6)
DECLARE_WRAPPER(7)
#undef DECLARE_WRAPPER

static void fake_rebalance_entry(Node *root, Node *node) {
    if (root == NULL || node == NULL || root->parent == NULL) abort();
    const uintptr_t sentinel = (uintptr_t)root->parent;
    ResolveMap *map = (ResolveMap *)(sentinel - offsetof(ResolveMap, root));
    if (&map->root != (Node **)(void *)(sentinel)) abort();
    rebalance_after_insert(map, node);
}

static void write_absolute_jump(uint8_t *address, uintptr_t target) {
    const uint8_t jump_prefix[6] = {0xff, 0x25, 0, 0, 0, 0};
    memcpy(address, jump_prefix, sizeof(jump_prefix));
    memcpy(address + sizeof(jump_prefix), &target, sizeof(target));
}

static bool write_relative_call(uint8_t *address, uintptr_t target,
                                uint8_t opcode) {
    const uintptr_t instruction_end = (uintptr_t)address + 5U;
    int64_t delta = 0;
    if (target >= instruction_end) {
        if (target - instruction_end > (uintptr_t)INT32_MAX) return false;
        delta = (int64_t)(target - instruction_end);
    } else {
        if (instruction_end - target > (uintptr_t)INT32_MAX + 1U)
            return false;
        delta = -(int64_t)(instruction_end - target);
    }
    const int32_t relative = (int32_t)delta;
    address[0] = opcode;
    memcpy(address + 1U, &relative, sizeof(relative));
    return true;
}

static int add_map_page(DavinciResolveTransactionSnapshotV1 *snapshot,
                        uintptr_t address) {
    const uintptr_t page_address = address &
        ~(uintptr_t)(DAVINCI_RESOLVE_PAGE_SIZE - 1U);
    for (uint32_t index = 0U; index < snapshot->map_page_count; ++index) {
        if (snapshot->map_pages[index].address == page_address) return 0;
    }
    if (snapshot->map_page_count >= DAVINCI_RESOLVE_MAP_PAGE_MAX) return -1;

    DavinciMappingIdentity mapping;
    if (davinci_find_mapping_identity(page_address, &mapping) != 0 ||
        mapping.start > page_address ||
        mapping.end < page_address + DAVINCI_RESOLVE_PAGE_SIZE ||
        mapping.protection != (PROT_READ | PROT_WRITE))
        return -1;

    DavinciResolveMapPageSnapshotV1 *entry =
        &snapshot->map_pages[snapshot->map_page_count++];
    memset(entry, 0, sizeof(*entry));
    entry->address = page_address;
    entry->length = DAVINCI_RESOLVE_PAGE_SIZE;
    entry->mapping_start = mapping.start;
    entry->mapping_end = mapping.end;
    entry->mapping_inode = mapping.inode;
    entry->device_major = mapping.device_major;
    entry->device_minor = mapping.device_minor;
    entry->protection = mapping.protection;
    return 0;
}

static int collect_tree_snapshot(Node *node) {
    if (node == NULL) return 0;
    if (g_snapshot.node_count >= DAVINCI_RESOLVE_TREE_NODE_COUNT) return -1;
    const uint32_t index = g_snapshot.node_count++;
    DavinciResolveTreeNodeSnapshotV1 *entry = &g_snapshot.nodes[index];
    entry->address = (uintptr_t)node;
    memcpy(entry->bytes, node, sizeof(*node));
    if (add_map_page(&g_snapshot, (uintptr_t)node) != 0) return -1;
    if (collect_tree_snapshot(node->left) != 0) return -1;
    if (collect_tree_snapshot(node->right) != 0) return -1;
    return 0;
}

static int add_allocator_owner(DavinciResolveAllocatorOwnerSnapshotV1 *owner,
                               uintptr_t target, uint32_t owner_kind) {
    DavinciMappingIdentity mapping;
    if (davinci_find_mapping_identity(target, &mapping) != 0 ||
        (mapping.protection & (PROT_READ | PROT_EXEC)) !=
            (PROT_READ | PROT_EXEC))
        return -1;
    memset(owner, 0, sizeof(*owner));
    owner->target = target;
    owner->mapping_start = mapping.start;
    owner->mapping_end = mapping.end;
    owner->mapping_inode = mapping.inode;
    owner->device_major = mapping.device_major;
    owner->device_minor = mapping.device_minor;
    owner->protection = mapping.protection;
    owner->owner = owner_kind;
    return 0;
}

static int prepare_fixture(void) {
    if (build_map(&g_test_map, source_records, 71U) != 0 ||
        g_test_map.size != 71U || !validate_map(&g_test_map))
        return -1;

    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size != DAVINCI_RESOLVE_PAGE_SIZE) return -1;
    g_test_page_size = (size_t)page_size;

    uintptr_t code_candidate = 0U;
    if (davinci_find_unmapped_range((uintptr_t)UINT64_C(0x50000000),
                                    (uintptr_t)UINT64_C(0x60000000),
                                    3U * g_test_page_size, g_test_page_size,
                                    &code_candidate) != 0)
        return -1;
    void *code = mmap((void *)code_candidate, 3U * g_test_page_size,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                      -1, 0);
    if (code == MAP_FAILED || (uintptr_t)code != code_candidate) {
        if (code != MAP_FAILED) (void)munmap(code, 3U * g_test_page_size);
        return -1;
    }
    g_code_mapping = (uint8_t *)code;
    g_code_address = (uintptr_t)code;
    memset(g_code_mapping, 0x90, 3U * g_test_page_size);

    uintptr_t transfer_candidate = 0U;
    if (davinci_find_unmapped_range((uintptr_t)UINT64_C(0x58000000),
                                    (uintptr_t)UINT64_C(0x59000000),
                                    g_test_page_size, g_test_page_size,
                                    &transfer_candidate) != 0)
        return -1;
    void *transfer = mmap((void *)transfer_candidate, g_test_page_size,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                          -1, 0);
    if (transfer == MAP_FAILED || (uintptr_t)transfer != transfer_candidate) {
        if (transfer != MAP_FAILED) (void)munmap(transfer, g_test_page_size);
        return -1;
    }
    g_transfer_mapping = (uint8_t *)transfer;
    g_transfer_address = (uintptr_t)transfer;
    memset(g_transfer_mapping, 0x90, g_test_page_size);

    const uintptr_t callback_entry = g_code_address + 0x100U;
    const uint8_t callback_bytes[8] =
        {0x53, 0x83, 0xff, 0x01, 0x75, 0x28, 0x48, 0x89};
    memcpy((void *)callback_entry, callback_bytes, sizeof(callback_bytes));

    const uintptr_t rebalance_stub = g_code_address +
                                     2U * g_test_page_size + 0x600U;
    const uintptr_t allocator_stub = rebalance_stub + 0x20U;
    const uintptr_t deallocator_stub = rebalance_stub + 0x40U;
    write_absolute_jump((uint8_t *)rebalance_stub,
                        address_fake_rebalance_entry());
    write_absolute_jump((uint8_t *)allocator_stub,
                        address_scalar_new());
    write_absolute_jump((uint8_t *)deallocator_stub,
                        address_sized_delete());

    static const size_t site_offsets[10] = {
        0x300U, 0x320U, 0x340U, 0x360U,
        0x300U, 0x320U, 0x340U, 0x360U,
        0x300U, 0x320U
    };
    static const uint8_t site_pages[10] = {0U, 0U, 0U, 0U, 1U,
                                            1U, 1U, 1U, 2U, 2U};
    static const uint8_t dispatch_indexes[10] = {0U, 1U, 2U, 3U, 4U,
                                                  5U, 6U, 7U, 3U, 4U};
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot.magic = DAVINCI_RESOLVE_SNAPSHOT_MAGIC;
    g_snapshot.version = DAVINCI_RESOLVE_SNAPSHOT_VERSION;
    g_snapshot.struct_size = sizeof(g_snapshot);
    g_snapshot.generic_map = (uintptr_t)&g_test_map;
    g_snapshot.node_count = 0U;
    g_snapshot.call_site_count = DAVINCI_RESOLVE_CALL_SITE_COUNT;
    g_snapshot.transfer_site_count = DAVINCI_RESOLVE_TRANSFER_SITE_COUNT;
    g_snapshot.page_count = DAVINCI_RESOLVE_CODE_PAGE_COUNT;
    g_snapshot.allocator_owner_count = DAVINCI_RESOLVE_ALLOCATOR_OWNER_COUNT;
    g_snapshot.system_page_size = DAVINCI_RESOLVE_PAGE_SIZE;
    g_snapshot.relay_length = DAVINCI_RESOLVE_PAGE_SIZE;
    g_snapshot.callback_pointer_cell = 0U;
    memcpy(g_snapshot.original_map_header, &g_test_map,
           sizeof(g_snapshot.original_map_header));
    memcpy(g_snapshot.original_callback, callback_bytes,
           sizeof(g_snapshot.original_callback));
    memset(g_snapshot.original_operator_new_plt, 0x90,
           sizeof(g_snapshot.original_operator_new_plt));
    memset(g_snapshot.original_sized_delete_plt, 0x90,
           sizeof(g_snapshot.original_sized_delete_plt));

    if (add_map_page(&g_snapshot, (uintptr_t)&g_test_map) != 0 ||
        collect_tree_snapshot(g_test_map.root) != 0 ||
        g_snapshot.node_count != DAVINCI_RESOLVE_TREE_NODE_COUNT)
        return -1;

    for (uint32_t index = 0U; index < DAVINCI_RESOLVE_CODE_PAGE_COUNT; ++index) {
        DavinciMappingIdentity mapping;
        const uintptr_t page_address = g_code_address +
            index * DAVINCI_RESOLVE_PAGE_SIZE;
        if (davinci_find_mapping_identity(page_address, &mapping) != 0)
            return -1;
        DavinciResolveCodePageSnapshotV1 *page = &g_snapshot.pages[index];
        memset(page, 0, sizeof(*page));
        page->address = page_address;
        page->length = DAVINCI_RESOLVE_PAGE_SIZE;
        page->mapping_start = page_address - DAVINCI_RESOLVE_PAGE_SIZE;
        page->mapping_end = page_address + 2U * DAVINCI_RESOLVE_PAGE_SIZE;
        page->mapping_inode = mapping.inode;
        page->device_major = mapping.device_major;
        page->device_minor = mapping.device_minor;
        page->protection = PROT_READ | PROT_EXEC;
    }

    const uintptr_t transfer_addresses[3] = {
        g_transfer_address + 0x200U,
        g_transfer_address + 0x220U,
        g_transfer_address + 0x240U
    };
    if (!write_relative_call((uint8_t *)transfer_addresses[0],
                             rebalance_stub, 0xe8) ||
        !write_relative_call((uint8_t *)transfer_addresses[1],
                             allocator_stub, 0xe8) ||
        !write_relative_call((uint8_t *)transfer_addresses[2],
                             deallocator_stub, 0xe9))
        return -1;

    for (uint32_t index = 0U; index < DAVINCI_RESOLVE_CALL_SITE_COUNT; ++index) {
        const uintptr_t site_address = g_code_address +
            site_pages[index] * DAVINCI_RESOLVE_PAGE_SIZE + site_offsets[index];
        if (!write_relative_call((uint8_t *)site_address, rebalance_stub, 0xe8))
            return -1;
        DavinciResolveCodeSiteSnapshotV1 *site = &g_snapshot.call_sites[index];
        memset(site, 0, sizeof(*site));
        site->address = site_address;
        memcpy(site->bytes, (const void *)site_address, 5U);
        site->dispatch_index = dispatch_indexes[index];
    }

    for (uint32_t index = 0U; index < DAVINCI_RESOLVE_TRANSFER_SITE_COUNT;
         ++index) {
        DavinciResolveCodeSiteSnapshotV1 *site =
            &g_snapshot.transfer_sites[index];
        memset(site, 0, sizeof(*site));
        site->address = transfer_addresses[index];
        memcpy(site->bytes, (const void *)site->address, 5U);
        site->dispatch_index = UINT8_MAX;
    }

    g_fake_got[0] = address_scalar_new();
    g_fake_got[1] = address_sized_delete();
    g_fake_got[2] = address_codex_delete();
    g_fake_got[3] = address_codex_malloc();
    g_fake_got[4] = address_codex_free();
    g_fake_got[5] = address_scalable_malloc();
    g_fake_got[6] = address_safe_free();
    g_fake_got[7] = 1U;
    g_fake_got[8] = address_original_free();
    for (uint32_t index = 0U; index < 9U; ++index) {
        g_snapshot.allocator_slot_addresses[index] =
            (uintptr_t)&g_fake_got[index];
        g_snapshot.allocator_values[index] = g_fake_got[index];
    }

    const uintptr_t owner_targets[8] = {
        g_fake_got[0], g_fake_got[1], g_fake_got[2], g_fake_got[3],
        g_fake_got[4], g_fake_got[5], g_fake_got[6], g_fake_got[8]
    };
    const uint32_t owner_kinds[8] = {
        DAVINCI_RESOLVE_OWNER_TBB_PROXY, DAVINCI_RESOLVE_OWNER_CODEX,
        DAVINCI_RESOLVE_OWNER_TBB_PROXY, DAVINCI_RESOLVE_OWNER_TBB_PROXY,
        DAVINCI_RESOLVE_OWNER_TBB_PROXY, DAVINCI_RESOLVE_OWNER_TBB_MALLOC,
        DAVINCI_RESOLVE_OWNER_TBB_MALLOC, DAVINCI_RESOLVE_OWNER_GLIBC
    };
    for (uint32_t index = 0U; index < 8U; ++index) {
        if (add_allocator_owner(&g_snapshot.allocator_owners[index],
                                owner_targets[index], owner_kinds[index]) != 0)
            return -1;
    }

    uintptr_t relay_candidate = 0U;
    if (davinci_find_unmapped_range((uintptr_t)UINT64_C(0x40000000),
                                    (uintptr_t)UINT64_C(0x85e3deb5),
                                    DAVINCI_RESOLVE_PAGE_SIZE,
                                    DAVINCI_RESOLVE_PAGE_SIZE,
                                    &relay_candidate) != 0)
        return -1;
    g_snapshot.relay_candidate = relay_candidate;
    g_snapshot.callback_pointer_cell = relay_candidate + 0x100U;

    g_bindings.context = NULL;
    g_bindings.allocator_target = g_fake_got[0];
    g_bindings.sized_delete_target = g_fake_got[1];
    g_bindings.rebalancer_target = rebalance_stub;
    g_bindings.callback_entry = callback_entry;
    g_bindings.callback_target = address_callback();
    const uintptr_t wrapper_targets[8] = {
        address_wrapper_0(), address_wrapper_1(),
        address_wrapper_2(), address_wrapper_3(),
        address_wrapper_4(), address_wrapper_5(),
        address_wrapper_6(), address_wrapper_7()
    };
    memcpy(g_bindings.wrapper_targets, wrapper_targets, sizeof(wrapper_targets));

    if (mprotect(g_code_mapping, 3U * g_test_page_size,
                 PROT_READ | PROT_EXEC) != 0 ||
        mprotect(g_transfer_mapping, g_test_page_size,
                 PROT_READ | PROT_EXEC) != 0)
        return -1;

    DavinciSha256 digest_context;
    davinci_sha256_init(&digest_context);
    davinci_sha256_update(
        &digest_context, &g_snapshot,
        offsetof(DavinciResolveTransactionSnapshotV1, snapshot_sha256));
    davinci_sha256_final(&digest_context, g_snapshot.snapshot_sha256);
    return 0;
}

static int verify_original_fixture(void) {
    if (g_test_map.size != 71U || !validate_map(&g_test_map) ||
        tree_find(&g_test_map, 0x61763031U) != NULL ||
        memcmp(&g_test_map, g_snapshot.original_map_header,
               sizeof(g_snapshot.original_map_header)) != 0)
        return 0;
    for (uint32_t index = 0U; index < g_snapshot.node_count; ++index) {
        if (memcmp((const void *)g_snapshot.nodes[index].address,
                   g_snapshot.nodes[index].bytes,
                   DAVINCI_RESOLVE_TREE_NODE_SIZE) != 0)
            return 0;
    }
    if (memcmp((const void *)g_bindings.callback_entry,
               g_snapshot.original_callback, 8U) != 0)
        return 0;
    for (uint32_t index = 0U; index < g_snapshot.call_site_count; ++index) {
        if (memcmp((const void *)g_snapshot.call_sites[index].address,
                   g_snapshot.call_sites[index].bytes, 5U) != 0)
            return 0;
    }
    for (uint32_t index = 0U; index < g_snapshot.page_count; ++index) {
        int protection = 0;
        if (davinci_find_mapping_range_protection(
                g_snapshot.pages[index].address,
                g_snapshot.pages[index].length, &protection) != 0 ||
            protection != PROT_READ + PROT_EXEC)
            return 0;
    }
    for (uint32_t index = 0U; index < g_snapshot.transfer_site_count; ++index) {
        const DavinciResolveCodeSiteSnapshotV1 *site =
            &g_snapshot.transfer_sites[index];
        DavinciMappingIdentity mapping;
        if (memcmp((const void *)site->address, site->bytes, 5U) != 0 ||
            davinci_find_mapping_identity(site->address, &mapping) != 0 ||
            mapping.protection != (PROT_READ | PROT_EXEC) ||
            mapping.inode != g_snapshot.pages[0].mapping_inode ||
            mapping.device_major != g_snapshot.pages[0].device_major ||
            mapping.device_minor != g_snapshot.pages[0].device_minor)
            return 0;
        for (uint32_t page = 0U; page < g_snapshot.page_count; ++page) {
            if ((site->address & ~(uintptr_t)(g_test_page_size - 1U)) ==
                g_snapshot.pages[page].address)
                return 0;
        }
    }
    return 1;
}

int main(void) {
    if (prepare_fixture() != 0) return fail("prepare synthetic Resolve fixture");
    if (!verify_original_fixture()) return fail("initial synthetic target snapshot");

    for (unsigned stage = 1U; stage <= 12U; ++stage) {
        DavinciAv1TransactionExecutionV1 execution;
        memset(&execution, 0, sizeof(execution));
        davinci_av1_executor_test_fail_after(stage);
        const int result = davinci_av1_execute_prepared_transaction(
            &g_snapshot, &g_bindings, &execution);
        davinci_av1_executor_test_fail_after(0U);
        if (result != DAVINCI_AV1_EXECUTION_TEST_INJECTED ||
            execution.state != DAVINCI_AV1_EXECUTION_ROLLED_BACK ||
            execution.call_patch_mask != 0U ||
            execution.callback_published != 0U ||
            execution.map_node_allocated != 0U ||
            execution.map_node_linked != 0U ||
            execution.code_page_writable_mask != 0U ||
            execution.relay_page != 0U ||
            !verify_original_fixture() ||
            !test_relay_is_unmapped(g_snapshot.relay_candidate,
                               g_snapshot.relay_length)) {
            fprintf(stderr, "FAIL: rollback at injected stage %u (result=%d, state=%u)\n",
                    stage, result, execution.state);
            return 2;
        }
    }
    puts("PASS: production executor rolls back after each of ten CALLs, map insertion and callback publication (12/12)");

    allocation_budget = 0;
    DavinciAv1TransactionExecutionV1 allocation_failure;
    memset(&allocation_failure, 0, sizeof(allocation_failure));
    const int allocation_result = davinci_av1_execute_prepared_transaction(
        &g_snapshot, &g_bindings, &allocation_failure);
    allocation_budget = -1;
    if (allocation_result != DAVINCI_AV1_EXECUTION_ALLOCATOR_FAILED ||
        allocation_failure.state != DAVINCI_AV1_EXECUTION_ROLLED_BACK ||
        !verify_original_fixture() ||
        !test_relay_is_unmapped(g_snapshot.relay_candidate, g_snapshot.relay_length))
        return fail("allocator failure cleanup before any target mutation");
    puts("PASS: allocation failure leaves map/code untouched and releases the staged relay");

    DavinciAv1TransactionExecutionV1 bad_snapshot_execution;
    memset(&bad_snapshot_execution, 0, sizeof(bad_snapshot_execution));
    DavinciResolveTransactionSnapshotV1 bad_snapshot = g_snapshot;
    bad_snapshot.call_sites[0].dispatch_index = 8U;
    const int bad_snapshot_result = davinci_av1_execute_prepared_transaction(
        &bad_snapshot, &g_bindings, &bad_snapshot_execution);
    if (bad_snapshot_result != DAVINCI_AV1_EXECUTION_SNAPSHOT_INVALID ||
        !verify_original_fixture())
        return fail("modified snapshot digest rejected before target mutation");
    puts("PASS: invalid snapshot digest rejected before target writes");

    DavinciAv1TransactionExecutionV1 committed;
    memset(&committed, 0, sizeof(committed));
    const int commit_result = davinci_av1_execute_prepared_transaction(
        &g_snapshot, &g_bindings, &committed);
    if (commit_result != DAVINCI_AV1_EXECUTION_OK ||
        committed.state != DAVINCI_AV1_EXECUTION_COMMITTED ||
        committed.call_patch_mask != ((UINT32_C(1) << 10U) - 1U) ||
        committed.map_node_linked == 0U || committed.callback_published == 0U ||
        g_test_map.size != 72U || !validate_map(&g_test_map))
        return fail("successful production executor commit");
    Node *av1 = tree_find(&g_test_map, 0x61763031U);
    if (av1 == NULL || av1->codec_id != 226U)
        return fail("production executor inserted av01 -> 226");
    puts("PASS: successful executor commit inserts av01 -> 226, patches ten CALLs and publishes callback last");

    if (davinci_av1_rollback_prepared_transaction(
            &g_snapshot, &g_bindings, &committed) != DAVINCI_AV1_EXECUTION_OK ||
        committed.state != DAVINCI_AV1_EXECUTION_ROLLED_BACK ||
        !verify_original_fixture() ||
        !test_relay_is_unmapped(g_snapshot.relay_candidate, g_snapshot.relay_length))
        return fail("successful transaction rollback restores all original state");
    puts("PASS: explicit rollback restores all original tree/code/protection bytes and unmaps relay");

    destroy_map(&g_test_map);
    if (munmap(g_code_mapping, 3U * g_test_page_size) != 0 ||
        munmap(g_transfer_mapping, g_test_page_size) != 0)
        return fail("release synthetic executable fixture");
    puts("LIMIT: production executor tested only against a synthetic map/code/allocator fixture; no Resolve process or binary was modified");
    return 0;
}
