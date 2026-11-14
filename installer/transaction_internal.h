#ifndef DAVINCI_AV1_TRANSACTION_INTERNAL_H
#define DAVINCI_AV1_TRANSACTION_INTERNAL_H

#include <stdint.h>

#include "common/preflight_attestation_v3.h"
#include "resolve_snapshot_v1.h"
#include "transaction_executor.h"

/* Internal installer-to-transaction handoff. This symbol is hidden from the
 * public DSO ABI and succeeds only after the one-shot read-only preflight gate. */
int davinci_av1_installer_copy_prepared_state(
    PreflightAttestation *attestation,
    DavinciResolveTransactionSnapshotV1 *snapshot,
    size_t snapshot_size, uint8_t session_nonce[16]);

/* Read-only pre-commit gate: verifies the copied snapshot and recaptures the
 * live Resolve state; succeeds only when both byte-identically match the
 * original one-shot snapshot. */
int davinci_av1_installer_revalidate_prepared_state(
    const PreflightAttestation *attestation,
    const DavinciResolveTransactionSnapshotV1 *snapshot,
    size_t snapshot_size, const uint8_t session_nonce[16]);

/* Production controller bridge. Execute always revalidates the prepared
 * snapshot immediately before entering the executor. Rollback reuses the
 * immutable private snapshot after an execution has already changed target
 * state; it never attempts to revalidate the pre-commit image first. */
int davinci_av1_installer_execute_prepared_state(
    const DavinciAv1TransactionBindingsV1 *bindings,
    DavinciAv1TransactionExecutionV1 *execution);
int davinci_av1_installer_rollback_prepared_state(
    const DavinciAv1TransactionBindingsV1 *bindings,
    DavinciAv1TransactionExecutionV1 *execution);

#endif
