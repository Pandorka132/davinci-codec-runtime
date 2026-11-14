#ifndef DAVINCI_AV1_BOOTSTRAP_API_H
#define DAVINCI_AV1_BOOTSTRAP_API_H

#include <stddef.h>
#include <stdint.h>
#include "common/preflight_attestation_v3.h"
#include "common/transaction_api_v1.h"
#include "common/install_authorization.h"
#include "transaction_executor.h"

#if defined(__GNUC__)
#define DAVINCI_AV1_BOOTSTRAP_API __attribute__((visibility("default")))
#else
#define DAVINCI_AV1_BOOTSTRAP_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define DAVINCI_AV1_BOOTSTRAP_ABI_VERSION 1U

enum DavinciAv1BootstrapStatus {
    DAVINCI_AV1_BOOTSTRAP_OK = 0,
    DAVINCI_AV1_BOOTSTRAP_INVALID_ARGUMENT = -1,
    DAVINCI_AV1_BOOTSTRAP_INVALID_ATTESTATION = -2,
    DAVINCI_AV1_BOOTSTRAP_STALE_PROCESS = -3,
    DAVINCI_AV1_BOOTSTRAP_THREAD_MISMATCH = -4,
    DAVINCI_AV1_BOOTSTRAP_IMAGE_MISMATCH = -5,
    DAVINCI_AV1_BOOTSTRAP_IDENTITY_ERROR = -6,
    DAVINCI_AV1_BOOTSTRAP_RESOLVE_STATE_MISMATCH = -7,
    DAVINCI_AV1_BOOTSTRAP_TRANSACTION_ALREADY_CLAIMED = -8,
    DAVINCI_AV1_BOOTSTRAP_TRANSACTION_REQUEST_INVALID = -9,
    DAVINCI_AV1_BOOTSTRAP_TRANSACTION_NONCE_MISMATCH = -10,
    DAVINCI_AV1_BOOTSTRAP_TRANSACTION_NOT_READY = -11,
    DAVINCI_AV1_BOOTSTRAP_TRANSACTION_SNAPSHOT_FAILED = -12,
    DAVINCI_AV1_BOOTSTRAP_EXECUTION_NOT_AUTHORIZED = -13
};

DAVINCI_AV1_BOOTSTRAP_API uint32_t
davinci_av1_bootstrap_abi_version(void);

/*
 * Read-only same-process handoff gate. The attestation must be produced after
 * this DSO and every other DSO whose identity is covered by the final session
 * have been loaded. This function performs no target memory writes and does
 * not install hooks, change Resolve data structures, or load the codec provider.
 */
DAVINCI_AV1_BOOTSTRAP_API int davinci_av1_bootstrap_validate_session(
    const PreflightAttestation *attestation);

/*
 * Read-only target-specific structural validation for the pinned Resolve
 * executable. First revalidates the same-process ABI-v3 identity, then checks
 * the live generic FFmpeg map/tree, AV1 callback bytes, ten FFmpeg CALL sites,
 * map allocation/rebalancer transfer sites, rebalancer body hash, allocator
 * PLT stubs, and executable mapping ownership of the attested allocator
 * targets. It performs no Resolve memory writes or allocator/rebalancer calls.
 */
DAVINCI_AV1_BOOTSTRAP_API int
davinci_av1_bootstrap_validate_resolve_state(
    const PreflightAttestation *attestation);

/*
 * One-shot, read-only transaction preparation. A valid request claims this
 * bootstrap instance, revalidates the live Resolve state, and captures a
 * private snapshot of the map/header/all original nodes, map-page identities
 * and protections, all ten CALL sites, three allocator/rebalancer transfer
 * sites, three RX code-page identities/protections, allocator GOT slots and
 * owning mappings, callback bytes, and a free rel32 relay-page candidate.
 * PREFLIGHT_READY means the complete snapshot was captured and digested only:
 * no CALL, map, callback, code-page or Resolve data mutation has occurred.
 * The controller must keep this DSO mapped for the process lifetime so the
 * one-shot state cannot be reset by dlclose/reload.
 */
DAVINCI_AV1_BOOTSTRAP_API int davinci_av1_bootstrap_prepare_transaction(
    const DavinciAv1TransactionRequestV1 *request,
    DavinciAv1TransactionStatusV1 *status);

/*
 * Write-capable transaction boundary. Both operations require the exact
 * authorization phrase and an already accepted one-shot snapshot. The caller
 * must keep the target all-stop; a fail-stop result means the inferior MUST
 * NOT resume. The controller and bootstrap both verify the authorization.
 */
DAVINCI_AV1_BOOTSTRAP_API int
davinci_av1_bootstrap_execute_prepared_transaction(
    const char *authorization, size_t authorization_size,
    const DavinciAv1TransactionBindingsV1 *bindings,
    DavinciAv1TransactionExecutionV1 *execution);

DAVINCI_AV1_BOOTSTRAP_API int
davinci_av1_bootstrap_rollback_prepared_transaction(
    const char *authorization, size_t authorization_size,
    const DavinciAv1TransactionBindingsV1 *bindings,
    DavinciAv1TransactionExecutionV1 *execution);

#ifdef __cplusplus
}
#endif

#endif
