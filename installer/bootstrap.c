#define _GNU_SOURCE
#include "bootstrap_api.h"
#include "common/target_identity.h"
#include "common/install_authorization.h"
#include "sha256.h"
#include "transaction_internal.h"

#include <stdatomic.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

uint32_t davinci_av1_bootstrap_abi_version(void) {
    return DAVINCI_AV1_BOOTSTRAP_ABI_VERSION;
}

static bool bytes_are_nonzero(const uint8_t *bytes, size_t length) {
    uint8_t combined = 0U;
    for (size_t index = 0; index < length; ++index)
        combined |= bytes[index];
    return combined != 0U;
}

static bool attestation_is_well_formed(
    const PreflightAttestation *attestation) {
    if (attestation == NULL ||
        attestation->magic != PREFLIGHT_MAGIC ||
        attestation->version != PREFLIGHT_VERSION ||
        attestation->struct_size != sizeof(*attestation) ||
        attestation->total_checks != PREFLIGHT_EXPECTED_CHECKS ||
        attestation->passed_checks != PREFLIGHT_EXPECTED_CHECKS ||
        attestation->failures != 0U || attestation->reserved != 0U ||
        attestation->session.reserved != 0U ||
        (attestation->flags & PREFLIGHT_REQUIRED_FLAGS) !=
            PREFLIGHT_REQUIRED_FLAGS ||
        (attestation->flags & ~(uint64_t)PREFLIGHT_REQUIRED_FLAGS) != 0U)
        return false;

    const TargetSessionIdentity *session = &attestation->session;
    return session->process_id != 0U &&
           session->process_start_ticks != 0U &&
           session->stopped_thread_count != 0U &&
           bytes_are_nonzero(session->session_nonce,
                             sizeof(session->session_nonce)) &&
           session->generic_map != 0U && session->av1_callback != 0U &&
           session->operator_new != 0U && session->sized_delete != 0U &&
           session->scalable_malloc != 0U && session->safe_free != 0U &&
           bytes_are_nonzero(session->executable_sha256,
                             sizeof(session->executable_sha256)) &&
           bytes_are_nonzero(session->loaded_dso_set_sha256,
                             sizeof(session->loaded_dso_set_sha256));
}

int davinci_av1_bootstrap_validate_session(
    const PreflightAttestation *attestation) {
    if (attestation == NULL) return DAVINCI_AV1_BOOTSTRAP_INVALID_ARGUMENT;
    if (!attestation_is_well_formed(attestation))
        return DAVINCI_AV1_BOOTSTRAP_INVALID_ATTESTATION;

    if (attestation->session.process_id != (uint64_t)getpid())
        return DAVINCI_AV1_BOOTSTRAP_STALE_PROCESS;

    uint64_t start_ticks = 0U;
    uint32_t thread_count = 0U;
    if (davinci_read_self_start_ticks(&start_ticks) != 0 ||
        davinci_count_self_threads(&thread_count) != 0)
        return DAVINCI_AV1_BOOTSTRAP_IDENTITY_ERROR;

    if (start_ticks != attestation->session.process_start_ticks)
        return DAVINCI_AV1_BOOTSTRAP_STALE_PROCESS;
    if (thread_count != attestation->session.stopped_thread_count)
        return DAVINCI_AV1_BOOTSTRAP_THREAD_MISMATCH;

    uint8_t executable_sha256[32];
    uint8_t loaded_dso_set_sha256[32];
    uint32_t loaded_dso_count = 0U;
    if (davinci_compute_process_file_identity(
            executable_sha256, loaded_dso_set_sha256,
            &loaded_dso_count) != 0 || loaded_dso_count == 0U)
        return DAVINCI_AV1_BOOTSTRAP_IDENTITY_ERROR;

    if (memcmp(executable_sha256,
               attestation->session.executable_sha256,
               sizeof(executable_sha256)) != 0 ||
        memcmp(loaded_dso_set_sha256,
               attestation->session.loaded_dso_set_sha256,
               sizeof(loaded_dso_set_sha256)) != 0)
        return DAVINCI_AV1_BOOTSTRAP_IMAGE_MISMATCH;

    return DAVINCI_AV1_BOOTSTRAP_OK;
}

typedef struct {
    uintptr_t left;
    uintptr_t right;
    uintptr_t parent;
    uint8_t color;
    uint8_t padding[3];
    uint32_t key;
    uint32_t value;
    uint8_t tail_padding[4];
} ResolveTreeNode;

typedef struct {
    uintptr_t begin;
    uintptr_t root;
    uint64_t size;
} ResolveTreeMap;

_Static_assert(sizeof(ResolveTreeNode) == 0x28U,
               "Resolve red-black node layout changed");
_Static_assert(offsetof(ResolveTreeNode, key) == 0x1cU,
               "Resolve red-black key offset changed");
_Static_assert(sizeof(ResolveTreeMap) == 0x18U,
               "Resolve generic decoder map layout changed");

enum { RESOLVE_NODE_RED = 0U, RESOLVE_NODE_BLACK = 1U };

static bool mapping_has_protection(uintptr_t address, size_t length,
                                   int required_protection) {
    int protection = 0;
    if (davinci_find_mapping_range_protection(address, length,
                                              &protection) != 0)
        return false;
    return (protection & required_protection) == required_protection;
}

static bool executable_range(uintptr_t address, size_t length) {
    return mapping_has_protection(address, length, PROT_READ | PROT_EXEC);
}

static bool executable_bytes_match(uintptr_t address,
                                   const uint8_t *expected,
                                   size_t length) {
    if (expected == NULL || !executable_range(address, length))
        return false;
    return memcmp((const void *)address, expected, length) == 0;
}

static bool executable_pointer(uintptr_t address) {
    return address != 0U &&
           mapping_has_protection(address, 1U, PROT_READ | PROT_EXEC);
}

static bool validate_resolve_tree_node(
    uintptr_t address, uintptr_t expected_parent,
    uint64_t minimum_key, uint64_t maximum_key,
    uintptr_t seen[71], uint32_t *seen_count,
    uintptr_t *minimum_node, uint32_t *minimum_value,
    uint32_t *black_height, uint8_t *subtree_color,
    bool *av01_found, DavinciResolveTransactionSnapshotV1 *snapshot) {
    if (address == 0U) {
        *black_height = 1U;
        *subtree_color = RESOLVE_NODE_BLACK;
        return true;
    }

    if (*seen_count >= 71U ||
        !mapping_has_protection(address, sizeof(ResolveTreeNode), PROT_READ))
        return false;

    for (uint32_t index = 0U; index < *seen_count; ++index) {
        if (seen[index] == address) return false;
    }
    const uint32_t snapshot_index = *seen_count;
    seen[(*seen_count)++] = address;

    ResolveTreeNode node;
    memcpy(&node, (const void *)address, sizeof(node));
    if (snapshot != NULL) {
        snapshot->nodes[snapshot_index].address = address;
        memcpy(snapshot->nodes[snapshot_index].bytes,
               (const void *)address, DAVINCI_RESOLVE_TREE_NODE_SIZE);
    }
    if (node.parent != expected_parent ||
        (uint64_t)node.key <= minimum_key ||
        (uint64_t)node.key >= maximum_key ||
        (node.color != RESOLVE_NODE_RED &&
         node.color != RESOLVE_NODE_BLACK))
        return false;

    if (*minimum_node == 0U || node.key < *minimum_value) {
        *minimum_node = address;
        *minimum_value = node.key;
    }
    if (node.key == UINT32_C(0x61763031)) *av01_found = true;

    uint32_t left_black_height = 0U;
    uint32_t right_black_height = 0U;
    uint8_t left_color = RESOLVE_NODE_BLACK;
    uint8_t right_color = RESOLVE_NODE_BLACK;

    if (!validate_resolve_tree_node(
            node.left, address, minimum_key, node.key, seen, seen_count,
            minimum_node, minimum_value, &left_black_height, &left_color,
            av01_found, snapshot) ||
        !validate_resolve_tree_node(
            node.right, address, node.key, maximum_key, seen, seen_count,
            minimum_node, minimum_value, &right_black_height, &right_color,
            av01_found, snapshot))
        return false;

    if (left_black_height != right_black_height ||
        (node.color == RESOLVE_NODE_RED &&
         (left_color == RESOLVE_NODE_RED ||
          right_color == RESOLVE_NODE_RED)))
        return false;

    *black_height = left_black_height +
                    (node.color == RESOLVE_NODE_BLACK ? 1U : 0U);
    *subtree_color = node.color;
    return true;
}

static bool validate_live_resolve_map(
    uintptr_t map_address, DavinciResolveTransactionSnapshotV1 *snapshot) {
    if (map_address != UINT64_C(0x29537cf0) ||
        !mapping_has_protection(map_address, sizeof(ResolveTreeMap),
                                PROT_READ))
        return false;

    ResolveTreeMap map;
    memcpy(&map, (const void *)map_address, sizeof(map));
    if (snapshot != NULL)
        memcpy(snapshot->original_map_header, (const void *)map_address,
               sizeof(snapshot->original_map_header));
    if (map.size != DAVINCI_RESOLVE_TREE_NODE_COUNT ||
        map.root == 0U || map.begin == 0U)
        return false;

    uintptr_t seen[71];
    uint32_t seen_count = 0U;
    uintptr_t minimum_node = 0U;
    uint32_t minimum_value = UINT32_MAX;
    uint32_t black_height = 0U;
    uint8_t root_color = RESOLVE_NODE_RED;
    bool av01_found = false;

    if (!validate_resolve_tree_node(
            map.root, map_address + offsetof(ResolveTreeMap, root),
            0U, UINT64_C(0x100000000), seen, &seen_count,
            &minimum_node, &minimum_value, &black_height, &root_color,
            &av01_found, snapshot))
        return false;

    if (snapshot != NULL) snapshot->node_count = seen_count;
    return seen_count == DAVINCI_RESOLVE_TREE_NODE_COUNT &&
           root_color == RESOLVE_NODE_BLACK &&
           map.begin == minimum_node &&
           !av01_found;
}

typedef struct {
    uintptr_t address;
    uintptr_t target;
    uint8_t bytes[5];
} ResolveCodeSite;

static bool validate_resolve_code_site(const ResolveCodeSite *site) {
    if (site == NULL ||
        !executable_bytes_match(site->address, site->bytes,
                                sizeof(site->bytes)))
        return false;

    int32_t displacement = 0;
    memcpy(&displacement, site->bytes + 1U, sizeof(displacement));
    const int64_t actual_target = (int64_t)(site->address + 5U) +
                                  (int64_t)displacement;
    return actual_target >= 0 &&
           (uintptr_t)actual_target == site->target;
}

int davinci_av1_bootstrap_validate_resolve_state(
    const PreflightAttestation *attestation) {
    static const uint8_t expected_resolve_sha256[32] = {
        0xd0, 0x83, 0xcf, 0x9f, 0x1e, 0xb8, 0x75, 0xbd,
        0xb9, 0x28, 0xdb, 0x54, 0xb2, 0x04, 0x25, 0xb4,
        0x11, 0x1b, 0x53, 0xa1, 0x6c, 0xae, 0x72, 0xa1,
        0xae, 0xf1, 0xc3, 0xee, 0xb6, 0x03, 0xa1, 0x21
    };
    static const uint8_t expected_callback[8] = {
        0x53, 0x83, 0xff, 0x01, 0x75, 0x28, 0x48, 0x89
    };
    static const ResolveCodeSite expected_call_sites[10] = {
        {0x05eef886U, 0x007f74c0U, {0xe8, 0x35, 0x7c, 0x90, 0xfa}},
        {0x05eef92cU, 0x0080b610U, {0xe8, 0xdf, 0xbc, 0x91, 0xfa}},
        {0x05eefa86U, 0x008051a0U, {0xe8, 0x15, 0x57, 0x91, 0xfa}},
        {0x05eefbc4U, 0x007fff30U, {0xe8, 0x67, 0x03, 0x91, 0xfa}},
        {0x05eefbceU, 0x00809270U, {0xe8, 0x9d, 0x96, 0x91, 0xfa}},
        {0x05ef0006U, 0x007f87e0U, {0xe8, 0xd5, 0x87, 0x90, 0xfa}},
        {0x05ef005dU, 0x008091a0U, {0xe8, 0x3e, 0x91, 0x91, 0xfa}},
        {0x05ef030fU, 0x007f75d0U, {0xe8, 0xbc, 0x72, 0x90, 0xfa}},
        {0x05ef066fU, 0x007fff30U, {0xe8, 0xbc, 0xf8, 0x90, 0xfa}},
        {0x05ef0678U, 0x00809270U, {0xe8, 0xf3, 0x8b, 0x91, 0xfa}}
    };
    static const ResolveCodeSite expected_tree_sites[3] = {
        {0x05c87387U, 0x00ecdba0U, {0xe8, 0x14, 0x68, 0x24, 0xfb}},
        {0x05c87445U, 0x00809dd0U, {0xe8, 0x86, 0x29, 0xb8, 0xfa}},
        {0x05c88485U, 0x00805d10U, {0xe9, 0x86, 0xd8, 0xb7, 0xfa}}
    };
    static const uint8_t expected_new_plt[16] = {
        0xff, 0x25, 0x12, 0x49, 0x84, 0x28, 0x68, 0xda,
        0x16, 0x00, 0x00, 0xe9, 0x40, 0x92, 0xfe, 0xff
    };
    static const uint8_t expected_sized_delete_plt[16] = {
        0xff, 0x25, 0x72, 0x69, 0x84, 0x28, 0x68, 0xce,
        0x12, 0x00, 0x00, 0xe9, 0x00, 0xd3, 0xfe, 0xff
    };
    static const uint8_t expected_rebalancer_sha256[32] = {
        0xea, 0x28, 0xd6, 0x07, 0xc2, 0x41, 0xa5, 0xbb,
        0xf6, 0x05, 0x17, 0x5e, 0xb9, 0xa3, 0xd8, 0x25,
        0xeb, 0xb5, 0x36, 0xa1, 0xf9, 0xb7, 0xde, 0xd0,
        0xb8, 0x59, 0xe2, 0x6a, 0x24, 0xae, 0x90, 0xf8
    };

    if (attestation == NULL) return DAVINCI_AV1_BOOTSTRAP_INVALID_ARGUMENT;
    const int session_status =
        davinci_av1_bootstrap_validate_session(attestation);
    if (session_status != DAVINCI_AV1_BOOTSTRAP_OK) return session_status;

    const TargetSessionIdentity *session = &attestation->session;
    if (memcmp(session->executable_sha256, expected_resolve_sha256,
               sizeof(expected_resolve_sha256)) != 0 ||
        session->generic_map != UINT64_C(0x29537cf0) ||
        session->av1_callback != UINT64_C(0x05e3deb0) ||
        session->stopped_thread_count != 1U ||
        !validate_live_resolve_map(session->generic_map, NULL))
        return DAVINCI_AV1_BOOTSTRAP_RESOLVE_STATE_MISMATCH;

    if (!executable_bytes_match(session->av1_callback,
                                expected_callback,
                                sizeof(expected_callback)))
        return DAVINCI_AV1_BOOTSTRAP_RESOLVE_STATE_MISMATCH;

    for (size_t index = 0U;
         index < sizeof(expected_call_sites) / sizeof(expected_call_sites[0]);
         ++index) {
        if (!validate_resolve_code_site(&expected_call_sites[index]))
            return DAVINCI_AV1_BOOTSTRAP_RESOLVE_STATE_MISMATCH;
    }

    for (size_t index = 0U;
         index < sizeof(expected_tree_sites) / sizeof(expected_tree_sites[0]);
         ++index) {
        if (!validate_resolve_code_site(&expected_tree_sites[index]))
            return DAVINCI_AV1_BOOTSTRAP_RESOLVE_STATE_MISMATCH;
    }

    if (!executable_range(UINT64_C(0x00ecdba0), 0x151U) ||
        !executable_bytes_match(UINT64_C(0x00809dd0), expected_new_plt,
                                sizeof(expected_new_plt)) ||
        !executable_bytes_match(UINT64_C(0x00805d10),
                                expected_sized_delete_plt,
                                sizeof(expected_sized_delete_plt)))
        return DAVINCI_AV1_BOOTSTRAP_RESOLVE_STATE_MISMATCH;

    uint8_t rebalancer_sha256[32];
    DavinciSha256 sha_context;
    davinci_sha256_init(&sha_context);
    davinci_sha256_update(&sha_context,
                          (const void *)UINT64_C(0x00ecdba0), 0x151U);
    davinci_sha256_final(&sha_context, rebalancer_sha256);
    if (memcmp(rebalancer_sha256, expected_rebalancer_sha256,
               sizeof(expected_rebalancer_sha256)) != 0)
        return DAVINCI_AV1_BOOTSTRAP_RESOLVE_STATE_MISMATCH;

    if (!executable_pointer(session->operator_new) ||
        !executable_pointer(session->sized_delete) ||
        !executable_pointer(session->scalable_malloc) ||
        !executable_pointer(session->safe_free))
        return DAVINCI_AV1_BOOTSTRAP_RESOLVE_STATE_MISMATCH;

    return DAVINCI_AV1_BOOTSTRAP_OK;
}

static bool checked_address_add(uintptr_t base, uintptr_t offset,
                                uintptr_t *result) {
    if (result == NULL || base > UINTPTR_MAX - offset) return false;
    *result = base + offset;
    return true;
}

static bool read_mapped_u64(uintptr_t address, uint64_t *value) {
    if (value == NULL) return false;
    int protection = 0;
    if (davinci_find_mapping_range_protection(address, sizeof(*value),
                                              &protection) != 0 ||
        (protection & PROT_READ) == 0)
        return false;
    memcpy(value, (const void *)address, sizeof(*value));
    return true;
}

static bool mapping_path_has_suffix(const DavinciMappingIdentity *mapping,
                                    const char *suffix) {
    if (mapping == NULL || suffix == NULL) return false;
    const size_t suffix_length = strlen(suffix);
    return suffix_length != 0U &&
           mapping->path_length >= suffix_length &&
           memcmp(mapping->path + mapping->path_length - suffix_length,
                  suffix, suffix_length) == 0;
}

static bool capture_allocator_owner(
    DavinciResolveAllocatorOwnerSnapshotV1 *snapshot, uintptr_t target,
    uint32_t expected_owner, const char *expected_suffix) {
    if (snapshot == NULL || target == 0U) return false;

    DavinciMappingIdentity mapping;
    if (davinci_find_mapping_identity(target, &mapping) != 0 ||
        (mapping.protection & (PROT_READ | PROT_EXEC)) !=
            (PROT_READ | PROT_EXEC) ||
        !mapping_path_has_suffix(&mapping, expected_suffix))
        return false;

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->target = target;
    snapshot->mapping_start = mapping.start;
    snapshot->mapping_end = mapping.end;
    snapshot->mapping_inode = mapping.inode;
    snapshot->device_major = mapping.device_major;
    snapshot->device_minor = mapping.device_minor;
    snapshot->protection = mapping.protection;
    snapshot->owner = expected_owner;
    return true;
}

static bool capture_allocator_chain(
    const TargetSessionIdentity *session,
    DavinciResolveTransactionSnapshotV1 *snapshot) {
    if (session == NULL || snapshot == NULL) return false;

    const uintptr_t new_slot = UINT64_C(0x2904e6e8);
    const uintptr_t sized_delete_slot = UINT64_C(0x2904c688);
    uint64_t new_target64 = 0U;
    uint64_t sized_delete_target64 = 0U;
    if (!read_mapped_u64(new_slot, &new_target64) ||
        !read_mapped_u64(sized_delete_slot, &sized_delete_target64))
        return false;

    const uintptr_t new_target = (uintptr_t)new_target64;
    const uintptr_t sized_delete_target = (uintptr_t)sized_delete_target64;
    if (new_target != session->operator_new ||
        sized_delete_target != session->sized_delete ||
        new_target < UINT64_C(0x26e0) ||
        sized_delete_target < UINT64_C(0x81670))
        return false;

    const uintptr_t proxy_base = new_target - UINT64_C(0x26e0);
    const uintptr_t codex_base = sized_delete_target - UINT64_C(0x81670);
    uintptr_t codex_delete_slot = 0U;
    uintptr_t codex_malloc_slot = 0U;
    uintptr_t codex_free_slot = 0U;
    uintptr_t proxy_scalable_slot = 0U;
    uintptr_t proxy_safe_free_slot = 0U;
    uintptr_t proxy_orig_searched_slot = 0U;
    uintptr_t proxy_orig_free_slot = 0U;
    if (!checked_address_add(codex_base, UINT64_C(0x3780d8),
                             &codex_delete_slot) ||
        !checked_address_add(codex_base, UINT64_C(0x378110),
                             &codex_malloc_slot) ||
        !checked_address_add(codex_base, UINT64_C(0x3781a0),
                             &codex_free_slot) ||
        !checked_address_add(proxy_base, UINT64_C(0x5058),
                             &proxy_scalable_slot) ||
        !checked_address_add(proxy_base, UINT64_C(0x5030),
                             &proxy_safe_free_slot) ||
        !checked_address_add(proxy_base, UINT64_C(0x50d8),
                             &proxy_orig_searched_slot) ||
        !checked_address_add(proxy_base, UINT64_C(0x50a8),
                             &proxy_orig_free_slot))
        return false;

    const uintptr_t slots[9] = {
        new_slot, sized_delete_slot, codex_delete_slot, codex_malloc_slot,
        codex_free_slot, proxy_scalable_slot, proxy_safe_free_slot,
        proxy_orig_searched_slot, proxy_orig_free_slot
    };
    uint64_t values[9];
    for (size_t index = 0U; index < 9U; ++index) {
        if (!read_mapped_u64(slots[index], &values[index])) return false;
    }

    uintptr_t expected_codex_delete = 0U;
    uintptr_t expected_codex_malloc = 0U;
    uintptr_t expected_codex_free = 0U;
    if (!checked_address_add(proxy_base, UINT64_C(0x27a0),
                             &expected_codex_delete) ||
        !checked_address_add(proxy_base, UINT64_C(0x2200),
                             &expected_codex_malloc) ||
        !checked_address_add(proxy_base, UINT64_C(0x2220),
                             &expected_codex_free))
        return false;

    const uintptr_t codex_delete_target = (uintptr_t)values[2];
    const uintptr_t codex_malloc_target = (uintptr_t)values[3];
    const uintptr_t codex_free_target = (uintptr_t)values[4];
    const uintptr_t scalable_malloc_target = (uintptr_t)values[5];
    const uintptr_t safe_free_target = (uintptr_t)values[6];
    const uintptr_t original_free_target = (uintptr_t)values[8];
    if (codex_delete_target != expected_codex_delete ||
        codex_malloc_target != expected_codex_malloc ||
        codex_free_target != expected_codex_free ||
        scalable_malloc_target != session->scalable_malloc ||
        safe_free_target != session->safe_free || values[7] != 1U ||
        original_free_target == 0U ||
        scalable_malloc_target < UINT64_C(0x16640))
        return false;

    const uintptr_t tbbmalloc_base =
        scalable_malloc_target - UINT64_C(0x16640);
    uintptr_t expected_safe_free = 0U;
    if (!checked_address_add(tbbmalloc_base, UINT64_C(0x16900),
                             &expected_safe_free) ||
        safe_free_target != expected_safe_free)
        return false;

    const uintptr_t owner_targets[8] = {
        new_target, sized_delete_target, codex_delete_target,
        codex_malloc_target, codex_free_target, scalable_malloc_target,
        safe_free_target, original_free_target
    };
    static const uint32_t expected_owners[8] = {
        DAVINCI_RESOLVE_OWNER_TBB_PROXY,
        DAVINCI_RESOLVE_OWNER_CODEX,
        DAVINCI_RESOLVE_OWNER_TBB_PROXY,
        DAVINCI_RESOLVE_OWNER_TBB_PROXY,
        DAVINCI_RESOLVE_OWNER_TBB_PROXY,
        DAVINCI_RESOLVE_OWNER_TBB_MALLOC,
        DAVINCI_RESOLVE_OWNER_TBB_MALLOC,
        DAVINCI_RESOLVE_OWNER_GLIBC
    };
    static const char *const expected_suffixes[8] = {
        "libtbbmalloc_proxy.so.2",
        "libcodexhdedecoder.so.5.2.0",
        "libtbbmalloc_proxy.so.2",
        "libtbbmalloc_proxy.so.2",
        "libtbbmalloc_proxy.so.2",
        "libtbbmalloc.so.2",
        "libtbbmalloc.so.2",
        "libc.so.6"
    };
    for (size_t index = 0U; index < 8U; ++index) {
        if (!capture_allocator_owner(&snapshot->allocator_owners[index],
                                     owner_targets[index],
                                     expected_owners[index],
                                     expected_suffixes[index]))
            return false;
    }

    memcpy(snapshot->allocator_slot_addresses, slots, sizeof(slots));
    memcpy(snapshot->allocator_values, values, sizeof(values));
    return true;
}

static bool rel32_reachable(uintptr_t instruction_end, uintptr_t target) {
    if (target >= instruction_end)
        return target - instruction_end <= (uintptr_t)INT32_MAX;
    return instruction_end - target <= (uintptr_t)INT32_MAX + 1U;
}

static bool capture_resolve_map_page(
    DavinciResolveTransactionSnapshotV1 *snapshot, uintptr_t address,
    size_t page_size) {
    if (snapshot == NULL || page_size != DAVINCI_RESOLVE_PAGE_SIZE)
        return false;
    const uintptr_t page_mask = (uintptr_t)page_size - 1U;
    const uintptr_t page_address = address & ~page_mask;
    if (page_address > UINTPTR_MAX - page_size) return false;

    for (uint32_t index = 0U; index < snapshot->map_page_count; ++index) {
        if (snapshot->map_pages[index].address == page_address) return true;
    }
    if (snapshot->map_page_count >= DAVINCI_RESOLVE_MAP_PAGE_MAX)
        return false;

    DavinciMappingIdentity mapping;
    if (davinci_find_mapping_identity(page_address, &mapping) != 0 ||
        mapping.start > page_address ||
        mapping.end < page_address + page_size ||
        mapping.protection != (PROT_READ | PROT_WRITE))
        return false;

    DavinciResolveMapPageSnapshotV1 *entry =
        &snapshot->map_pages[snapshot->map_page_count++];
    memset(entry, 0, sizeof(*entry));
    entry->address = page_address;
    entry->length = page_size;
    entry->mapping_start = mapping.start;
    entry->mapping_end = mapping.end;
    entry->mapping_inode = mapping.inode;
    entry->device_major = mapping.device_major;
    entry->device_minor = mapping.device_minor;
    entry->protection = mapping.protection;
    return true;
}

static bool capture_resolve_transaction_snapshot(
    const TargetSessionIdentity *session,
    DavinciResolveTransactionSnapshotV1 *snapshot) {
    if (session == NULL || snapshot == NULL) return false;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->magic = DAVINCI_RESOLVE_SNAPSHOT_MAGIC;
    snapshot->version = DAVINCI_RESOLVE_SNAPSHOT_VERSION;
    snapshot->struct_size = (uint32_t)sizeof(*snapshot);
    snapshot->generic_map = session->generic_map;

    static const ResolveCodeSite expected_call_sites[10] = {
        {0x05eef886U, 0x007f74c0U, {0xe8, 0x35, 0x7c, 0x90, 0xfa}},
        {0x05eef92cU, 0x0080b610U, {0xe8, 0xdf, 0xbc, 0x91, 0xfa}},
        {0x05eefa86U, 0x008051a0U, {0xe8, 0x15, 0x57, 0x91, 0xfa}},
        {0x05eefbc4U, 0x007fff30U, {0xe8, 0x67, 0x03, 0x91, 0xfa}},
        {0x05eefbceU, 0x00809270U, {0xe8, 0x9d, 0x96, 0x91, 0xfa}},
        {0x05ef0006U, 0x007f87e0U, {0xe8, 0xd5, 0x87, 0x90, 0xfa}},
        {0x05ef005dU, 0x008091a0U, {0xe8, 0x3e, 0x91, 0x91, 0xfa}},
        {0x05ef030fU, 0x007f75d0U, {0xe8, 0xbc, 0x72, 0x90, 0xfa}},
        {0x05ef066fU, 0x007fff30U, {0xe8, 0xbc, 0xf8, 0x90, 0xfa}},
        {0x05ef0678U, 0x00809270U, {0xe8, 0xf3, 0x8b, 0x91, 0xfa}}
    };
    static const ResolveCodeSite expected_transfer_sites[3] = {
        {0x05c87387U, 0x00ecdba0U, {0xe8, 0x14, 0x68, 0x24, 0xfb}},
        {0x05c87445U, 0x00809dd0U, {0xe8, 0x86, 0x29, 0xb8, 0xfa}},
        {0x05c88485U, 0x00805d10U, {0xe9, 0x86, 0xd8, 0xb7, 0xfa}}
    };
    static const uint8_t expected_callback[8] = {
        0x53, 0x83, 0xff, 0x01, 0x75, 0x28, 0x48, 0x89
    };
    static const uint8_t expected_new_plt[16] = {
        0xff, 0x25, 0x12, 0x49, 0x84, 0x28, 0x68, 0xda,
        0x16, 0x00, 0x00, 0xe9, 0x40, 0x92, 0xfe, 0xff
    };
    static const uint8_t expected_sized_delete_plt[16] = {
        0xff, 0x25, 0x72, 0x69, 0x84, 0x28, 0x68, 0xce,
        0x12, 0x00, 0x00, 0xe9, 0x00, 0xd3, 0xfe, 0xff
    };
    static const uintptr_t code_pages[3] = {
        0x05e3d000U, 0x05eef000U, 0x05ef0000U
    };
    static const uintptr_t call_stub_offsets[8] = {
        0x00U, 0x10U, 0x20U, 0x30U, 0x40U, 0x50U, 0x60U, 0x70U
    };
    static const uint8_t call_stub_indices[10] = {
        0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 3U, 4U
    };

    if (session->stopped_thread_count != 1U ||
        !validate_live_resolve_map(session->generic_map, snapshot))
        return false;
    snapshot->call_site_count = DAVINCI_RESOLVE_CALL_SITE_COUNT;
    snapshot->transfer_site_count = DAVINCI_RESOLVE_TRANSFER_SITE_COUNT;
    snapshot->page_count = DAVINCI_RESOLVE_CODE_PAGE_COUNT;
    snapshot->allocator_owner_count = DAVINCI_RESOLVE_ALLOCATOR_OWNER_COUNT;

    const long page_size_value = sysconf(_SC_PAGESIZE);
    if (page_size_value != (long)DAVINCI_RESOLVE_PAGE_SIZE) return false;
    const size_t page_size = (size_t)page_size_value;
    snapshot->system_page_size = page_size;

    if (!capture_resolve_map_page(snapshot, session->generic_map, page_size))
        return false;
    for (uint32_t index = 0U; index < snapshot->node_count; ++index) {
        if (!capture_resolve_map_page(snapshot, snapshot->nodes[index].address,
                                      page_size))
            return false;
    }

    for (size_t index = 0U; index < DAVINCI_RESOLVE_CODE_PAGE_COUNT; ++index) {
        DavinciMappingIdentity mapping;
        if (davinci_find_mapping_identity(code_pages[index], &mapping) != 0 ||
            mapping.start > code_pages[index] ||
            mapping.end < code_pages[index] + page_size ||
            mapping.protection != (PROT_READ | PROT_EXEC))
            return false;
        snapshot->pages[index].address = code_pages[index];
        snapshot->pages[index].length = page_size;
        snapshot->pages[index].mapping_start = mapping.start;
        snapshot->pages[index].mapping_end = mapping.end;
        snapshot->pages[index].mapping_inode = mapping.inode;
        snapshot->pages[index].device_major = mapping.device_major;
        snapshot->pages[index].device_minor = mapping.device_minor;
        snapshot->pages[index].protection = mapping.protection;
    }

    if (!executable_bytes_match(session->av1_callback, expected_callback,
                                sizeof(expected_callback)))
        return false;
    memcpy(snapshot->original_callback, (const void *)session->av1_callback,
           sizeof(snapshot->original_callback));

    for (size_t index = 0U; index < DAVINCI_RESOLVE_CALL_SITE_COUNT; ++index) {
        const ResolveCodeSite *source = &expected_call_sites[index];
        if (!validate_resolve_code_site(source)) return false;
        snapshot->call_sites[index].address = source->address;
        memcpy(snapshot->call_sites[index].bytes, source->bytes,
               sizeof(source->bytes));
        snapshot->call_sites[index].dispatch_index = call_stub_indices[index];
    }

    for (size_t index = 0U;
         index < DAVINCI_RESOLVE_TRANSFER_SITE_COUNT; ++index) {
        const ResolveCodeSite *source = &expected_transfer_sites[index];
        if (!validate_resolve_code_site(source)) return false;
        snapshot->transfer_sites[index].address = source->address;
        memcpy(snapshot->transfer_sites[index].bytes, source->bytes,
               sizeof(source->bytes));
        snapshot->transfer_sites[index].dispatch_index = UINT8_MAX;
    }

    if (!executable_bytes_match(UINT64_C(0x00809dd0), expected_new_plt,
                                sizeof(expected_new_plt)) ||
        !executable_bytes_match(UINT64_C(0x00805d10),
                                expected_sized_delete_plt,
                                sizeof(expected_sized_delete_plt)))
        return false;
    memcpy(snapshot->original_operator_new_plt, expected_new_plt,
           sizeof(snapshot->original_operator_new_plt));
    memcpy(snapshot->original_sized_delete_plt, expected_sized_delete_plt,
           sizeof(snapshot->original_sized_delete_plt));

    if (!capture_allocator_chain(session, snapshot)) return false;

    uintptr_t relay_candidate = 0U;
    if (davinci_find_unmapped_range(
            UINT64_C(0x40000000), UINT64_C(0x85e3deb5), page_size,
            page_size, &relay_candidate) != 0)
        return false;
    DavinciMappingIdentity relay_mapping;
    if (davinci_find_mapping_identity(relay_candidate, &relay_mapping) == 0)
        return false;

    uintptr_t callback_cell = 0U;
    if (!checked_address_add(relay_candidate, 0x100U, &callback_cell) ||
        !rel32_reachable(session->av1_callback + 6U, callback_cell))
        return false;
    for (size_t index = 0U; index < DAVINCI_RESOLVE_CALL_SITE_COUNT; ++index) {
        const uintptr_t stub = relay_candidate +
                               call_stub_offsets[call_stub_indices[index]];
        if (!rel32_reachable(expected_call_sites[index].address + 5U, stub))
            return false;
    }

    snapshot->relay_candidate = relay_candidate;
    snapshot->relay_length = page_size;
    snapshot->callback_pointer_cell = callback_cell;

    DavinciSha256 sha_context;
    davinci_sha256_init(&sha_context);
    davinci_sha256_update(
        &sha_context, snapshot,
        offsetof(DavinciResolveTransactionSnapshotV1, snapshot_sha256));
    davinci_sha256_final(&sha_context, snapshot->snapshot_sha256);
    return true;
}


_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
               "transaction one-shot state requires lock-free integer atomics");

static _Atomic uint32_t g_transaction_state =
    DAVINCI_AV1_TRANSACTION_AVAILABLE;
static _Atomic uint32_t g_transaction_completed_stage =
    DAVINCI_AV1_TRANSACTION_STAGE_NONE;
static PreflightAttestation g_transaction_attestation;
static DavinciResolveTransactionSnapshotV1 g_transaction_snapshot;
static DavinciAv1TransactionExecutionV1 g_transaction_execution;
static uint8_t g_transaction_nonce[16];

static bool transaction_request_is_well_formed(
    const DavinciAv1TransactionRequestV1 *request) {
    return request != NULL &&
           request->magic == DAVINCI_AV1_TRANSACTION_REQUEST_MAGIC &&
           request->version == DAVINCI_AV1_TRANSACTION_API_VERSION &&
           request->struct_size == sizeof(*request) &&
           request->flags == 0U && request->reserved == 0U &&
           request->reserved_tail == 0U && request->attestation != NULL &&
           bytes_are_nonzero(request->session_nonce,
                             sizeof(request->session_nonce));
}

static bool transaction_ranges_overlap(uintptr_t left, size_t left_size,
                                         uintptr_t right, size_t right_size) {
    if (left_size > UINTPTR_MAX - left ||
        right_size > UINTPTR_MAX - right)
        return true;
    const uintptr_t left_end = left + left_size;
    const uintptr_t right_end = right + right_size;
    return left < right_end && right < left_end;
}

static void fill_transaction_status(
    DavinciAv1TransactionStatusV1 *status, int result, uint32_t state,
    uint32_t completed_stage, const uint8_t nonce[16]) {
    memset(status, 0, sizeof(*status));
    status->magic = DAVINCI_AV1_TRANSACTION_STATUS_MAGIC;
    status->version = DAVINCI_AV1_TRANSACTION_API_VERSION;
    status->struct_size = sizeof(*status);
    status->result = result;
    status->state = state;
    status->completed_stage = completed_stage;
    if (nonce != NULL)
        memcpy(status->session_nonce, nonce, sizeof(status->session_nonce));
}

int davinci_av1_bootstrap_prepare_transaction(
    const DavinciAv1TransactionRequestV1 *request,
    DavinciAv1TransactionStatusV1 *status) {
    if (status == NULL ||
        ((uintptr_t)status % _Alignof(DavinciAv1TransactionStatusV1)) != 0U ||
        !mapping_has_protection((uintptr_t)status, sizeof(*status),
                               PROT_READ | PROT_WRITE))
        return DAVINCI_AV1_BOOTSTRAP_INVALID_ARGUMENT;

    const uint32_t current_state = atomic_load_explicit(
        &g_transaction_state, memory_order_acquire);
    if (request == NULL) {
        fill_transaction_status(status, DAVINCI_AV1_BOOTSTRAP_INVALID_ARGUMENT,
                                current_state,
                                DAVINCI_AV1_TRANSACTION_STAGE_NONE, NULL);
        return DAVINCI_AV1_BOOTSTRAP_INVALID_ARGUMENT;
    }

    const uintptr_t request_address = (uintptr_t)request;
    if ((request_address % _Alignof(DavinciAv1TransactionRequestV1)) != 0U ||
        !mapping_has_protection(request_address, sizeof(*request), PROT_READ))
        {
            fill_transaction_status(
                status, DAVINCI_AV1_BOOTSTRAP_TRANSACTION_REQUEST_INVALID,
                current_state, DAVINCI_AV1_TRANSACTION_STAGE_NONE, NULL);
            return DAVINCI_AV1_BOOTSTRAP_TRANSACTION_REQUEST_INVALID;
        }

    if (transaction_ranges_overlap(request_address, sizeof(*request),
                                   (uintptr_t)status, sizeof(*status)))
        return DAVINCI_AV1_BOOTSTRAP_INVALID_ARGUMENT;

    const uintptr_t attestation_address = (uintptr_t)request->attestation;
    if (request->attestation != NULL &&
        (((attestation_address % _Alignof(PreflightAttestation)) != 0U) ||
         !mapping_has_protection(attestation_address,
                                 sizeof(PreflightAttestation), PROT_READ) ||
         transaction_ranges_overlap(attestation_address,
                                    sizeof(PreflightAttestation),
                                    (uintptr_t)status, sizeof(*status)) ||
         transaction_ranges_overlap(attestation_address,
                                    sizeof(PreflightAttestation),
                                    request_address, sizeof(*request))))
        return DAVINCI_AV1_BOOTSTRAP_INVALID_ARGUMENT;

    if (!transaction_request_is_well_formed(request)) {
        const uint32_t state = atomic_load_explicit(
            &g_transaction_state, memory_order_acquire);
        fill_transaction_status(
            status, DAVINCI_AV1_BOOTSTRAP_TRANSACTION_REQUEST_INVALID, state,
            DAVINCI_AV1_TRANSACTION_STAGE_NONE, NULL);
        return DAVINCI_AV1_BOOTSTRAP_TRANSACTION_REQUEST_INVALID;
    }

    uint32_t expected_state = DAVINCI_AV1_TRANSACTION_AVAILABLE;
    if (!atomic_compare_exchange_strong_explicit(
            &g_transaction_state, &expected_state,
            DAVINCI_AV1_TRANSACTION_VALIDATING, memory_order_acq_rel,
            memory_order_acquire)) {
        const uint32_t established_stage = atomic_load_explicit(
            &g_transaction_completed_stage, memory_order_acquire);
        const uint32_t stage =
            established_stage == DAVINCI_AV1_TRANSACTION_STAGE_SNAPSHOT_CAPTURED &&
            expected_state != DAVINCI_AV1_TRANSACTION_VALIDATING
                ? established_stage : DAVINCI_AV1_TRANSACTION_STAGE_NONE;
        const uint8_t *nonce =
            expected_state == DAVINCI_AV1_TRANSACTION_VALIDATING
                ? NULL : g_transaction_nonce;
        fill_transaction_status(
            status, DAVINCI_AV1_BOOTSTRAP_TRANSACTION_ALREADY_CLAIMED,
            expected_state, stage, nonce);
        return DAVINCI_AV1_BOOTSTRAP_TRANSACTION_ALREADY_CLAIMED;
    }

    memcpy(g_transaction_nonce, request->session_nonce,
           sizeof(g_transaction_nonce));

    int result = DAVINCI_AV1_BOOTSTRAP_OK;
    uint32_t final_state = DAVINCI_AV1_TRANSACTION_REJECTED;
    uint32_t completed_stage = DAVINCI_AV1_TRANSACTION_STAGE_NONE;
    atomic_store_explicit(&g_transaction_completed_stage,
                          DAVINCI_AV1_TRANSACTION_STAGE_NONE,
                          memory_order_relaxed);
    memset(&g_transaction_snapshot, 0, sizeof(g_transaction_snapshot));

    if (memcmp(request->session_nonce,
               request->attestation->session.session_nonce,
               sizeof(request->session_nonce)) != 0) {
        result = DAVINCI_AV1_BOOTSTRAP_TRANSACTION_NONCE_MISMATCH;
    } else {
        result = davinci_av1_bootstrap_validate_resolve_state(
            request->attestation);
    }

    if (result == DAVINCI_AV1_BOOTSTRAP_OK &&
        !capture_resolve_transaction_snapshot(&request->attestation->session,
                                               &g_transaction_snapshot))
        result = DAVINCI_AV1_BOOTSTRAP_TRANSACTION_SNAPSHOT_FAILED;

    if (result == DAVINCI_AV1_BOOTSTRAP_OK) {
        memcpy(&g_transaction_attestation, request->attestation,
               sizeof(g_transaction_attestation));
        final_state = DAVINCI_AV1_TRANSACTION_PREFLIGHT_READY;
        completed_stage = DAVINCI_AV1_TRANSACTION_STAGE_SNAPSHOT_CAPTURED;
        atomic_store_explicit(&g_transaction_completed_stage,
                              completed_stage, memory_order_relaxed);
    } else {
        memset(&g_transaction_snapshot, 0, sizeof(g_transaction_snapshot));
        atomic_store_explicit(&g_transaction_completed_stage,
                              DAVINCI_AV1_TRANSACTION_STAGE_NONE,
                              memory_order_relaxed);
    }

    atomic_store_explicit(&g_transaction_state, final_state,
                          memory_order_release);
    fill_transaction_status(status, result, final_state, completed_stage,
                            g_transaction_nonce);
    return result;
}

static bool transaction_snapshot_is_valid(
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
        snapshot->relay_candidate == 0U ||
        snapshot->callback_pointer_cell != snapshot->relay_candidate + 0x100U)
        return false;

    uint8_t digest[32];
    DavinciSha256 sha_context;
    davinci_sha256_init(&sha_context);
    davinci_sha256_update(
        &sha_context, snapshot,
        offsetof(DavinciResolveTransactionSnapshotV1, snapshot_sha256));
    davinci_sha256_final(&sha_context, digest);
    return memcmp(digest, snapshot->snapshot_sha256, sizeof(digest)) == 0;
}

int davinci_av1_installer_copy_prepared_state(
    PreflightAttestation *attestation,
    DavinciResolveTransactionSnapshotV1 *snapshot,
    size_t snapshot_size, uint8_t session_nonce[16]) {
    if (attestation == NULL || snapshot == NULL || session_nonce == NULL ||
        snapshot_size != sizeof(g_transaction_snapshot))
        return DAVINCI_AV1_BOOTSTRAP_INVALID_ARGUMENT;

    const uint32_t transaction_state = atomic_load_explicit(
        &g_transaction_state, memory_order_acquire);
    const bool snapshot_state_available =
        transaction_state == DAVINCI_AV1_TRANSACTION_PREFLIGHT_READY ||
        transaction_state == DAVINCI_AV1_TRANSACTION_EXECUTING ||
        transaction_state == DAVINCI_AV1_TRANSACTION_COMMITTED ||
        transaction_state == DAVINCI_AV1_TRANSACTION_ROLLED_BACK ||
        transaction_state == DAVINCI_AV1_TRANSACTION_FAILED ||
        transaction_state == DAVINCI_AV1_TRANSACTION_FAIL_STOP;
    if (!snapshot_state_available ||
        atomic_load_explicit(&g_transaction_completed_stage,
                             memory_order_acquire) !=
            DAVINCI_AV1_TRANSACTION_STAGE_SNAPSHOT_CAPTURED)
        return DAVINCI_AV1_BOOTSTRAP_TRANSACTION_NOT_READY;

    if (!transaction_snapshot_is_valid(&g_transaction_snapshot))
        return DAVINCI_AV1_BOOTSTRAP_TRANSACTION_SNAPSHOT_FAILED;

    memcpy(attestation, &g_transaction_attestation, sizeof(*attestation));
    memcpy(snapshot, &g_transaction_snapshot, sizeof(*snapshot));
    memcpy(session_nonce, g_transaction_nonce, sizeof(g_transaction_nonce));
    return DAVINCI_AV1_BOOTSTRAP_OK;
}


int davinci_av1_installer_revalidate_prepared_state(
    const PreflightAttestation *attestation,
    const DavinciResolveTransactionSnapshotV1 *snapshot,
    size_t snapshot_size, const uint8_t session_nonce[16]) {
    if (attestation == NULL || snapshot == NULL || session_nonce == NULL ||
        snapshot_size != sizeof(g_transaction_snapshot))
        return DAVINCI_AV1_BOOTSTRAP_INVALID_ARGUMENT;

    const uintptr_t attestation_address = (uintptr_t)attestation;
    const uintptr_t snapshot_address = (uintptr_t)snapshot;
    const uintptr_t nonce_address = (uintptr_t)session_nonce;
    if ((attestation_address % _Alignof(PreflightAttestation)) != 0U ||
        (snapshot_address % _Alignof(DavinciResolveTransactionSnapshotV1)) !=
            0U ||
        !mapping_has_protection(attestation_address, sizeof(*attestation),
                               PROT_READ) ||
        !mapping_has_protection(snapshot_address, snapshot_size, PROT_READ) ||
        !mapping_has_protection(nonce_address, 16U, PROT_READ) ||
        transaction_ranges_overlap(attestation_address, sizeof(*attestation),
                                   snapshot_address, snapshot_size) ||
        transaction_ranges_overlap(attestation_address, sizeof(*attestation),
                                   nonce_address, 16U) ||
        transaction_ranges_overlap(snapshot_address, snapshot_size,
                                   nonce_address, 16U))
        return DAVINCI_AV1_BOOTSTRAP_INVALID_ARGUMENT;

    if (atomic_load_explicit(&g_transaction_state, memory_order_acquire) !=
            DAVINCI_AV1_TRANSACTION_PREFLIGHT_READY ||
        atomic_load_explicit(&g_transaction_completed_stage,
                             memory_order_acquire) !=
            DAVINCI_AV1_TRANSACTION_STAGE_SNAPSHOT_CAPTURED)
        return DAVINCI_AV1_BOOTSTRAP_TRANSACTION_NOT_READY;

    if (memcmp(session_nonce, g_transaction_nonce,
               sizeof(g_transaction_nonce)) != 0)
        return DAVINCI_AV1_BOOTSTRAP_TRANSACTION_NONCE_MISMATCH;
    if (memcmp(attestation, &g_transaction_attestation,
               sizeof(g_transaction_attestation)) != 0)
        return DAVINCI_AV1_BOOTSTRAP_INVALID_ATTESTATION;
    if (!transaction_snapshot_is_valid(snapshot) ||
        memcmp(snapshot, &g_transaction_snapshot,
               sizeof(g_transaction_snapshot)) != 0)
        return DAVINCI_AV1_BOOTSTRAP_TRANSACTION_SNAPSHOT_FAILED;

    const int resolve_status =
        davinci_av1_bootstrap_validate_resolve_state(attestation);
    if (resolve_status != DAVINCI_AV1_BOOTSTRAP_OK) return resolve_status;

    DavinciResolveTransactionSnapshotV1 current_snapshot;
    if (!capture_resolve_transaction_snapshot(&attestation->session,
                                               &current_snapshot) ||
        memcmp(&current_snapshot, snapshot, sizeof(current_snapshot)) != 0)
        return DAVINCI_AV1_BOOTSTRAP_TRANSACTION_SNAPSHOT_FAILED;

    return DAVINCI_AV1_BOOTSTRAP_OK;
}


static bool installer_bindings_match_attestation(
    const DavinciAv1TransactionBindingsV1 *bindings,
    const PreflightAttestation *attestation,
    const DavinciResolveTransactionSnapshotV1 *snapshot) {
    if (bindings == NULL || attestation == NULL || snapshot == NULL ||
        bindings->callback_entry != attestation->session.av1_callback ||
        bindings->allocator_target != attestation->session.operator_new ||
        bindings->sized_delete_target != attestation->session.sized_delete ||
        snapshot->transfer_sites[0].bytes[0] != 0xe8)
        return false;

    int32_t relative = 0;
    memcpy(&relative, snapshot->transfer_sites[0].bytes + 1U,
           sizeof(relative));
    const uintptr_t instruction_end = snapshot->transfer_sites[0].address + 5U;
    uintptr_t rebalancer_target = 0U;
    if (relative >= 0) {
        if ((uintptr_t)relative > UINTPTR_MAX - instruction_end) return false;
        rebalancer_target = instruction_end + (uintptr_t)relative;
    } else {
        const uintptr_t magnitude = (uintptr_t)(-(int64_t)relative);
        if (magnitude > instruction_end) return false;
        rebalancer_target = instruction_end - magnitude;
    }
    return bindings->rebalancer_target == rebalancer_target;
}

static bool installer_buffers_valid(
    const DavinciAv1TransactionBindingsV1 *bindings,
    DavinciAv1TransactionExecutionV1 *execution) {
    if (bindings == NULL || execution == NULL) return false;
    const uintptr_t bindings_address = (uintptr_t)bindings;
    const uintptr_t execution_address = (uintptr_t)execution;
    if ((bindings_address % _Alignof(DavinciAv1TransactionBindingsV1)) != 0U ||
        (execution_address % _Alignof(DavinciAv1TransactionExecutionV1)) != 0U ||
        !mapping_has_protection(bindings_address, sizeof(*bindings), PROT_READ) ||
        !mapping_has_protection(execution_address, sizeof(*execution),
                                PROT_READ | PROT_WRITE) ||
        transaction_ranges_overlap(bindings_address, sizeof(*bindings),
                                   execution_address, sizeof(*execution)))
        return false;
    return true;
}

int davinci_av1_installer_execute_prepared_state(
    const DavinciAv1TransactionBindingsV1 *bindings,
    DavinciAv1TransactionExecutionV1 *execution) {
    if (!installer_buffers_valid(bindings, execution))
        return DAVINCI_AV1_BOOTSTRAP_INVALID_ARGUMENT;

    PreflightAttestation attestation;
    DavinciResolveTransactionSnapshotV1 snapshot;
    uint8_t nonce[16];
    const int copy_status = davinci_av1_installer_copy_prepared_state(
        &attestation, &snapshot, sizeof(snapshot), nonce);
    if (copy_status != DAVINCI_AV1_BOOTSTRAP_OK) return copy_status;

    const int revalidate_status =
        davinci_av1_installer_revalidate_prepared_state(
            &attestation, &snapshot, sizeof(snapshot), nonce);
    if (revalidate_status != DAVINCI_AV1_BOOTSTRAP_OK)
        return revalidate_status;
    if (!installer_bindings_match_attestation(bindings, &attestation,
                                               &snapshot))
        return DAVINCI_AV1_BOOTSTRAP_RESOLVE_STATE_MISMATCH;

    uint32_t expected_state = DAVINCI_AV1_TRANSACTION_PREFLIGHT_READY;
    if (!atomic_compare_exchange_strong_explicit(
            &g_transaction_state, &expected_state,
            DAVINCI_AV1_TRANSACTION_EXECUTING, memory_order_acq_rel,
            memory_order_acquire))
        return DAVINCI_AV1_BOOTSTRAP_TRANSACTION_ALREADY_CLAIMED;

    memset(&g_transaction_execution, 0, sizeof(g_transaction_execution));
    const int execution_status = davinci_av1_execute_prepared_transaction(
        &snapshot, bindings, &g_transaction_execution);

    uint32_t final_state = DAVINCI_AV1_TRANSACTION_FAILED;
    if (g_transaction_execution.state == DAVINCI_AV1_EXECUTION_COMMITTED &&
        execution_status == DAVINCI_AV1_EXECUTION_OK)
        final_state = DAVINCI_AV1_TRANSACTION_COMMITTED;
    else if (g_transaction_execution.state == DAVINCI_AV1_EXECUTION_ROLLED_BACK)
        final_state = DAVINCI_AV1_TRANSACTION_ROLLED_BACK;
    else if (g_transaction_execution.state == DAVINCI_AV1_EXECUTION_FAIL_STOP)
        final_state = DAVINCI_AV1_TRANSACTION_FAIL_STOP;
    memcpy(execution, &g_transaction_execution, sizeof(*execution));
    atomic_store_explicit(&g_transaction_state, final_state,
                          memory_order_release);
    return execution_status;
}

int davinci_av1_installer_rollback_prepared_state(
    const DavinciAv1TransactionBindingsV1 *bindings,
    DavinciAv1TransactionExecutionV1 *execution) {
    if (!installer_buffers_valid(bindings, execution))
        return DAVINCI_AV1_BOOTSTRAP_INVALID_ARGUMENT;

    PreflightAttestation attestation;
    DavinciResolveTransactionSnapshotV1 snapshot;
    uint8_t nonce[16];
    const int copy_status = davinci_av1_installer_copy_prepared_state(
        &attestation, &snapshot, sizeof(snapshot), nonce);
    if (copy_status != DAVINCI_AV1_BOOTSTRAP_OK) return copy_status;
    if (!installer_bindings_match_attestation(bindings, &attestation,
                                               &snapshot))
        return DAVINCI_AV1_BOOTSTRAP_RESOLVE_STATE_MISMATCH;
    if (memcmp(execution, &g_transaction_execution,
               sizeof(g_transaction_execution)) != 0)
        return DAVINCI_AV1_BOOTSTRAP_TRANSACTION_SNAPSHOT_FAILED;

    uint32_t expected_state = DAVINCI_AV1_TRANSACTION_COMMITTED;
    if (!atomic_compare_exchange_strong_explicit(
            &g_transaction_state, &expected_state,
            DAVINCI_AV1_TRANSACTION_EXECUTING, memory_order_acq_rel,
            memory_order_acquire))
        return DAVINCI_AV1_BOOTSTRAP_TRANSACTION_ALREADY_CLAIMED;

    const int rollback_status = davinci_av1_rollback_prepared_transaction(
        &snapshot, bindings, &g_transaction_execution);
    const uint32_t final_state = rollback_status == DAVINCI_AV1_EXECUTION_OK
        ? DAVINCI_AV1_TRANSACTION_ROLLED_BACK
        : DAVINCI_AV1_TRANSACTION_FAIL_STOP;
    memcpy(execution, &g_transaction_execution, sizeof(*execution));
    atomic_store_explicit(&g_transaction_state, final_state,
                          memory_order_release);
    return rollback_status;
}



static bool installer_authorization_valid(const char *authorization,
                                          size_t authorization_size) {
    if (authorization == NULL ||
        authorization_size !=
            sizeof(DAVINCI_AV1_INSTALL_AUTHORIZATION_TEXT) - 1U ||
        !mapping_has_protection((uintptr_t)authorization,
                                authorization_size, PROT_READ))
        return false;
    return davinci_av1_install_authorization_is_valid(
        authorization, authorization_size) != 0;
}

int davinci_av1_bootstrap_execute_prepared_transaction(
    const char *authorization, size_t authorization_size,
    const DavinciAv1TransactionBindingsV1 *bindings,
    DavinciAv1TransactionExecutionV1 *execution) {
    if (!installer_authorization_valid(authorization, authorization_size))
        return DAVINCI_AV1_BOOTSTRAP_EXECUTION_NOT_AUTHORIZED;
    return davinci_av1_installer_execute_prepared_state(bindings, execution);
}

int davinci_av1_bootstrap_rollback_prepared_transaction(
    const char *authorization, size_t authorization_size,
    const DavinciAv1TransactionBindingsV1 *bindings,
    DavinciAv1TransactionExecutionV1 *execution) {
    if (!installer_authorization_valid(authorization, authorization_size))
        return DAVINCI_AV1_BOOTSTRAP_EXECUTION_NOT_AUTHORIZED;
    return davinci_av1_installer_rollback_prepared_state(bindings, execution);
}
