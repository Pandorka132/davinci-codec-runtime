#ifndef DAVINCI_AV1_SESSION_CONTROLLER_API_H
#define DAVINCI_AV1_SESSION_CONTROLLER_API_H

#include <stddef.h>
#include <stdint.h>

#include "common/preflight_attestation_v3.h"
#include "common/transaction_api_v1.h"
#include "common/install_authorization.h"
#include "transaction_executor.h"
#include <davinci_codec_runtime/api.hpp>

#if defined(__GNUC__)
#define DAVINCI_AV1_SESSION_CONTROLLER_API __attribute__((visibility("default")))
#else
#define DAVINCI_AV1_SESSION_CONTROLLER_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define DAVINCI_AV1_SESSION_CONTROLLER_ABI_VERSION 1U
#define DAVINCI_AV1_SESSION_CONTROLLER_CONFIG_MAGIC UINT64_C(0x4156315343434631)
#define DAVINCI_AV1_SESSION_CONTROLLER_STATUS_MAGIC UINT64_C(0x4156315343535431)
#define DAVINCI_AV1_SESSION_CONTROLLER_PLAN_MAGIC UINT64_C(0x4156315343504c31)

enum DavinciAv1SessionControllerState {
    DAVINCI_AV1_SESSION_CONTROLLER_EMPTY = 0U,
    DAVINCI_AV1_SESSION_CONTROLLER_LOADING = 1U,
    DAVINCI_AV1_SESSION_CONTROLLER_BOUND = 2U,
    DAVINCI_AV1_SESSION_CONTROLLER_PREFLIGHT_READY = 3U,
    DAVINCI_AV1_SESSION_CONTROLLER_EXECUTING = 4U,
    DAVINCI_AV1_SESSION_CONTROLLER_COMMITTED = 5U,
    DAVINCI_AV1_SESSION_CONTROLLER_ROLLED_BACK = 6U,
    DAVINCI_AV1_SESSION_CONTROLLER_FAIL_STOP = 7U,
    DAVINCI_AV1_SESSION_CONTROLLER_FAILED = 8U
};

enum DavinciAv1SessionControllerResult {
    DAVINCI_AV1_SESSION_CONTROLLER_OK = 0,
    DAVINCI_AV1_SESSION_CONTROLLER_INVALID_ARGUMENT = -1,
    DAVINCI_AV1_SESSION_CONTROLLER_ALREADY_USED = -2,
    DAVINCI_AV1_SESSION_CONTROLLER_PATH_INVALID = -3,
    DAVINCI_AV1_SESSION_CONTROLLER_ARTIFACT_MISMATCH = -4,
    DAVINCI_AV1_SESSION_CONTROLLER_DLOPEN_FAILED = -5,
    DAVINCI_AV1_SESSION_CONTROLLER_SYMBOL_MISSING = -6,
    DAVINCI_AV1_SESSION_CONTROLLER_TARGET_MISMATCH = -7,
    DAVINCI_AV1_SESSION_CONTROLLER_PROVIDER_FAILED = -8,
    DAVINCI_AV1_SESSION_CONTROLLER_CALLBACK_FAILED = -9,
    DAVINCI_AV1_SESSION_CONTROLLER_ATTESTATION_INVALID = -10,
    DAVINCI_AV1_SESSION_CONTROLLER_BOOTSTRAP_REJECTED = -11,
    DAVINCI_AV1_SESSION_CONTROLLER_NOT_READY = -12,
    DAVINCI_AV1_SESSION_CONTROLLER_BUFFER_INVALID = -13,
    DAVINCI_AV1_SESSION_CONTROLLER_AUTHORIZATION_REQUIRED = -14,
    DAVINCI_AV1_SESSION_CONTROLLER_EXECUTION_FAILED = -15,
    DAVINCI_AV1_SESSION_CONTROLLER_ROLLBACK_FAILED = -16,
    DAVINCI_AV1_SESSION_CONTROLLER_THREAD_MISMATCH = -17
};

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t struct_size;
    uint32_t flags;
    uint32_t reserved;
    char project_root[512];
    char resolve_executable[256];
    uint8_t reserved_tail[64];
} DavinciAv1SessionControllerConfigV1;

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t struct_size;
    int32_t result;
    uint32_t state;
    uint32_t provider_ready;
    uint32_t callback_configured;
    uint32_t reserved;
    uint32_t private_avcodec_version;
    uint32_t reserved2;
} DavinciAv1SessionControllerStatusV1;

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t struct_size;
    uint32_t state;
    uint32_t reserved;
    DavinciAv1TransactionBindingsV1 transaction_bindings;
    DavinciAv1CallbackBindingsV1 callback_bindings;
} DavinciAv1SessionControllerPlanV1;

DAVINCI_AV1_SESSION_CONTROLLER_API uint32_t
davinci_av1_session_controller_abi_version(void);

DAVINCI_AV1_SESSION_CONTROLLER_API int
davinci_av1_session_controller_initialize(
    const DavinciAv1SessionControllerConfigV1 *config);

DAVINCI_AV1_SESSION_CONTROLLER_API int
davinci_av1_session_controller_get_status(
    DavinciAv1SessionControllerStatusV1 *status);

/* Build immutable transaction bindings from the final same-process ABI-v3
 * attestation. This operation is read-only and does not invoke the executor. */
DAVINCI_AV1_SESSION_CONTROLLER_API int
davinci_av1_session_controller_get_plan(
    const PreflightAttestation *attestation,
    DavinciAv1SessionControllerPlanV1 *plan);

/* Revalidates the final image set and Resolve structures, then asks the
 * bootstrap to capture its one-shot immutable transaction snapshot. No
 * Resolve target memory, code page, codec-map or callback bytes are changed. */
DAVINCI_AV1_SESSION_CONTROLLER_API int
davinci_av1_session_controller_prepare(
    const PreflightAttestation *attestation,
    DavinciAv1TransactionStatusV1 *status);

/* Write-capable API. Both execute and rollback require the exact authorization
 * phrase from common/install_authorization.h. They also require a one-thread
 * inferior and the accepted immutable PREFLIGHT_READY snapshot. A FAIL_STOP
 * state MUST NOT be resumed; the outer GDB/controller must keep it stopped. */
DAVINCI_AV1_SESSION_CONTROLLER_API int
davinci_av1_session_controller_execute(
    const char *authorization, size_t authorization_size,
    DavinciAv1TransactionExecutionV1 *execution);

DAVINCI_AV1_SESSION_CONTROLLER_API int
davinci_av1_session_controller_rollback(
    const char *authorization, size_t authorization_size,
    DavinciAv1TransactionExecutionV1 *execution);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
static_assert(sizeof(DavinciAv1SessionControllerConfigV1) == 856U);
static_assert(sizeof(DavinciAv1SessionControllerStatusV1) == 48U);
static_assert(offsetof(DavinciAv1SessionControllerPlanV1,
                       transaction_bindings) == 24U);
#endif

#endif
