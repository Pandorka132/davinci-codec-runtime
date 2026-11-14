#ifndef DAVINCI_AV1_RESOLVE_SNAPSHOT_V1_H
#define DAVINCI_AV1_RESOLVE_SNAPSHOT_V1_H

#include <stddef.h>
#include <stdint.h>

#define DAVINCI_RESOLVE_SNAPSHOT_MAGIC UINT64_C(0x41563152534e5031)
#define DAVINCI_RESOLVE_SNAPSHOT_VERSION 1U
#define DAVINCI_RESOLVE_TREE_NODE_COUNT 71U
#define DAVINCI_RESOLVE_CALL_SITE_COUNT 10U
#define DAVINCI_RESOLVE_TRANSFER_SITE_COUNT 3U
#define DAVINCI_RESOLVE_CODE_PAGE_COUNT 3U
#define DAVINCI_RESOLVE_MAP_PAGE_MAX 72U
#define DAVINCI_RESOLVE_ALLOCATOR_OWNER_COUNT 8U
#define DAVINCI_RESOLVE_TREE_NODE_SIZE 0x28U
#define DAVINCI_RESOLVE_PAGE_SIZE 0x1000U

/* These records are private installer state, not a public process ABI. */
typedef struct {
    uintptr_t address;
    uint8_t bytes[DAVINCI_RESOLVE_TREE_NODE_SIZE];
} DavinciResolveTreeNodeSnapshotV1;

typedef struct {
    uintptr_t address;
    uint8_t bytes[5];
    uint8_t dispatch_index;
    uint8_t reserved[2];
} DavinciResolveCodeSiteSnapshotV1;

typedef struct {
    uintptr_t address;
    uintptr_t length;
    uintptr_t mapping_start;
    uintptr_t mapping_end;
    uint64_t mapping_inode;
    uint32_t device_major;
    uint32_t device_minor;
    uint32_t protection;
    uint32_t reserved;
} DavinciResolveCodePageSnapshotV1;

typedef struct {
    uintptr_t address;
    uintptr_t length;
    uintptr_t mapping_start;
    uintptr_t mapping_end;
    uint64_t mapping_inode;
    uint32_t device_major;
    uint32_t device_minor;
    uint32_t protection;
    uint32_t reserved;
} DavinciResolveMapPageSnapshotV1;

enum DavinciResolveAllocatorOwner {
    DAVINCI_RESOLVE_OWNER_TBB_PROXY = 1U,
    DAVINCI_RESOLVE_OWNER_CODEX = 2U,
    DAVINCI_RESOLVE_OWNER_TBB_MALLOC = 3U,
    DAVINCI_RESOLVE_OWNER_GLIBC = 4U
};

typedef struct {
    uintptr_t target;
    uintptr_t mapping_start;
    uintptr_t mapping_end;
    uint64_t mapping_inode;
    uint32_t device_major;
    uint32_t device_minor;
    uint32_t protection;
    uint32_t owner;
} DavinciResolveAllocatorOwnerSnapshotV1;

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t struct_size;
    uint32_t node_count;
    uint32_t call_site_count;
    uint32_t transfer_site_count;
    uint32_t page_count;
    uint32_t map_page_count;
    uint32_t allocator_owner_count;
    uint32_t reserved;
    uintptr_t generic_map;
    uint8_t original_map_header[0x18];
    uintptr_t relay_candidate;
    uintptr_t relay_length;
    uintptr_t callback_pointer_cell;
    uintptr_t system_page_size;
    uint8_t original_callback[8];
    uint8_t original_operator_new_plt[16];
    uint8_t original_sized_delete_plt[16];
    uintptr_t allocator_slot_addresses[9];
    uint64_t allocator_values[9];
    DavinciResolveCodePageSnapshotV1 pages[DAVINCI_RESOLVE_CODE_PAGE_COUNT];
    DavinciResolveMapPageSnapshotV1 map_pages[DAVINCI_RESOLVE_MAP_PAGE_MAX];
    DavinciResolveCodeSiteSnapshotV1
        call_sites[DAVINCI_RESOLVE_CALL_SITE_COUNT];
    DavinciResolveCodeSiteSnapshotV1
        transfer_sites[DAVINCI_RESOLVE_TRANSFER_SITE_COUNT];
    DavinciResolveAllocatorOwnerSnapshotV1
        allocator_owners[DAVINCI_RESOLVE_ALLOCATOR_OWNER_COUNT];
    DavinciResolveTreeNodeSnapshotV1
        nodes[DAVINCI_RESOLVE_TREE_NODE_COUNT];
    uint8_t snapshot_sha256[32];
} DavinciResolveTransactionSnapshotV1;

#ifdef __cplusplus
#define DAVINCI_RESOLVE_SNAPSHOT_STATIC_ASSERT static_assert
#else
#define DAVINCI_RESOLVE_SNAPSHOT_STATIC_ASSERT _Static_assert
#endif

DAVINCI_RESOLVE_SNAPSHOT_STATIC_ASSERT(
    sizeof(DavinciResolveTreeNodeSnapshotV1) == 0x30U,
    "Resolve node snapshot size changed");
DAVINCI_RESOLVE_SNAPSHOT_STATIC_ASSERT(
    sizeof(DavinciResolveCodeSiteSnapshotV1) == 0x10U,
    "Resolve code-site snapshot size changed");
DAVINCI_RESOLVE_SNAPSHOT_STATIC_ASSERT(
    sizeof(DavinciResolveCodePageSnapshotV1) == 0x38U,
    "Resolve page snapshot size changed");
DAVINCI_RESOLVE_SNAPSHOT_STATIC_ASSERT(
    sizeof(DavinciResolveMapPageSnapshotV1) == 0x38U,
    "Resolve map-page snapshot size changed");
DAVINCI_RESOLVE_SNAPSHOT_STATIC_ASSERT(
    sizeof(DavinciResolveAllocatorOwnerSnapshotV1) == 0x30U,
    "Resolve allocator-owner snapshot size changed");
DAVINCI_RESOLVE_SNAPSHOT_STATIC_ASSERT(
    offsetof(DavinciResolveTransactionSnapshotV1, snapshot_sha256) + 32U ==
        sizeof(DavinciResolveTransactionSnapshotV1),
    "Resolve snapshot digest must be the final field");

#undef DAVINCI_RESOLVE_SNAPSHOT_STATIC_ASSERT

#endif
