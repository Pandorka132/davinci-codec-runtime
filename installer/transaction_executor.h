#ifndef DAVINCI_AV1_TRANSACTION_EXECUTOR_H
#define DAVINCI_AV1_TRANSACTION_EXECUTOR_H

#include <stddef.h>
#include <stdint.h>

#include "resolve_snapshot_v1.h"

#define DAVINCI_AV1_EXECUTION_MAGIC UINT64_C(0x4156314558454331)
#define DAVINCI_AV1_EXECUTION_VERSION 1U
#define DAVINCI_AV1_WRAPPER_COUNT 8U
#define DAVINCI_AV1_RELAY_STUB_STRIDE 16U
#define DAVINCI_AV1_RELAY_CALLBACK_CELL_OFFSET 0x100U
#define DAVINCI_AV1_RELAY_WRAPPER_TABLE_OFFSET 0x180U

typedef void *(*DavinciAv1AllocateNodeNoexcept)(void *context, size_t size);
typedef void (*DavinciAv1FreeNodeNoexcept)(void *context, void *node,
                                            size_t size);
typedef void (*DavinciAv1RebalanceNode)(void *context, void *root,
                                        void *new_node);

typedef struct {
    void *context;
    uintptr_t allocator_target;
    uintptr_t sized_delete_target;
    uintptr_t rebalancer_target;
    uintptr_t callback_entry;
    uintptr_t callback_target;
    uintptr_t wrapper_targets[DAVINCI_AV1_WRAPPER_COUNT];
} DavinciAv1TransactionBindingsV1;

enum DavinciAv1ExecutionState {
    DAVINCI_AV1_EXECUTION_EMPTY = 0U,
    DAVINCI_AV1_EXECUTION_PREPARING = 1U,
    DAVINCI_AV1_EXECUTION_COMMITTED = 2U,
    DAVINCI_AV1_EXECUTION_ROLLED_BACK = 3U,
    DAVINCI_AV1_EXECUTION_FAIL_STOP = 4U
};

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t struct_size;
    uint32_t state;
    uint32_t call_patch_mask;
    uint32_t code_page_writable_mask;
    uint32_t map_node_allocated;
    uint32_t map_node_linked;
    uint32_t callback_published;
    uint32_t reserved;
    uintptr_t relay_page;
    uintptr_t inserted_node;
} DavinciAv1TransactionExecutionV1;

enum DavinciAv1ExecutionResult {
    DAVINCI_AV1_EXECUTION_OK = 0,
    DAVINCI_AV1_EXECUTION_INVALID_ARGUMENT = -1,
    DAVINCI_AV1_EXECUTION_SNAPSHOT_INVALID = -2,
    DAVINCI_AV1_EXECUTION_TARGET_MISMATCH = -3,
    DAVINCI_AV1_EXECUTION_RELAY_FAILED = -4,
    DAVINCI_AV1_EXECUTION_ALLOCATOR_FAILED = -5,
    DAVINCI_AV1_EXECUTION_PROTECTION_FAILED = -6,
    DAVINCI_AV1_EXECUTION_PATCH_FAILED = -7,
    DAVINCI_AV1_EXECUTION_MAP_FAILED = -8,
    DAVINCI_AV1_EXECUTION_SYNC_FAILED = -9,
    DAVINCI_AV1_EXECUTION_VERIFY_FAILED = -10,
    DAVINCI_AV1_EXECUTION_ROLLBACK_FAILED = -11,
    DAVINCI_AV1_EXECUTION_NOT_COMMITTED = -12,
    DAVINCI_AV1_EXECUTION_TEST_INJECTED = -13
};

/* Same-process, all-threads-stopped executor. The allocator callback must be
 * non-throwing and return NULL after catching any C++ allocation exception.
 * Wrapper/provider DSOs remain owned by the session controller and must stay
 * mapped for as long as this execution remains committed. */
int davinci_av1_execute_prepared_transaction(
    const DavinciResolveTransactionSnapshotV1 *snapshot,
    const DavinciAv1TransactionBindingsV1 *bindings,
    DavinciAv1TransactionExecutionV1 *execution);

/* Rollback is valid only while the target remains all-stop. On any failure the
 * caller must fail-stop and must not resume the inferior. */
int davinci_av1_rollback_prepared_transaction(
    const DavinciResolveTransactionSnapshotV1 *snapshot,
    const DavinciAv1TransactionBindingsV1 *bindings,
    DavinciAv1TransactionExecutionV1 *execution);

#ifdef DAVINCI_AV1_EXECUTOR_TESTING
void davinci_av1_executor_test_fail_after(unsigned stage);
#endif

#endif
