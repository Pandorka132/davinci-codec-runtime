#define _GNU_SOURCE
#include "../bootstrap_api.h"
#include "../common/target_identity.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fail(const char *message) {
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

int main(void) {
    if (davinci_av1_bootstrap_abi_version() != DAVINCI_AV1_BOOTSTRAP_ABI_VERSION)
        return fail("bootstrap ABI version");

    PreflightAttestation attestation;
    memset(&attestation, 0, sizeof(attestation));
    attestation.magic = PREFLIGHT_MAGIC;
    attestation.version = PREFLIGHT_VERSION;
    attestation.struct_size = sizeof(attestation);
    attestation.total_checks = PREFLIGHT_EXPECTED_CHECKS;
    attestation.passed_checks = PREFLIGHT_EXPECTED_CHECKS;
    attestation.flags = PREFLIGHT_REQUIRED_FLAGS;
    attestation.session.process_id = (uint64_t)getpid();
    attestation.session.stopped_thread_count = 1U;
    for (size_t index = 0; index < sizeof(attestation.session.session_nonce);
         ++index)
        attestation.session.session_nonce[index] = (uint8_t)(index + 1U);
    attestation.session.generic_map = UINT64_C(0x10000000);
    attestation.session.av1_callback = UINT64_C(0x20000000);
    attestation.session.operator_new = UINT64_C(0x30000000);
    attestation.session.sized_delete = UINT64_C(0x30000010);
    attestation.session.scalable_malloc = UINT64_C(0x40000000);
    attestation.session.safe_free = UINT64_C(0x40000010);

    if (davinci_read_self_start_ticks(
            &attestation.session.process_start_ticks) != 0)
        return fail("read process start time");
    uint32_t loaded_dso_count = 0U;
    if (davinci_compute_process_file_identity(
            attestation.session.executable_sha256,
            attestation.session.loaded_dso_set_sha256,
            &loaded_dso_count) != 0 || loaded_dso_count == 0U)
        return fail("compute mapped image identity");

    if (davinci_av1_bootstrap_validate_session(&attestation) !=
        DAVINCI_AV1_BOOTSTRAP_OK)
        return fail("valid same-process attestation rejected");

    if (davinci_av1_bootstrap_validate_resolve_state(&attestation) !=
        DAVINCI_AV1_BOOTSTRAP_RESOLVE_STATE_MISMATCH)
        return fail("non-Resolve executable accepted as the pinned Resolve target");

    PreflightAttestation stale = attestation;
    ++stale.session.process_id;
    if (davinci_av1_bootstrap_validate_session(&stale) !=
        DAVINCI_AV1_BOOTSTRAP_STALE_PROCESS)
        return fail("stale PID accepted");

    PreflightAttestation wrong_start = attestation;
    ++wrong_start.session.process_start_ticks;
    if (davinci_av1_bootstrap_validate_session(&wrong_start) !=
        DAVINCI_AV1_BOOTSTRAP_STALE_PROCESS)
        return fail("stale process start time accepted");

    PreflightAttestation wrong_threads = attestation;
    ++wrong_threads.session.stopped_thread_count;
    if (davinci_av1_bootstrap_validate_session(&wrong_threads) !=
        DAVINCI_AV1_BOOTSTRAP_THREAD_MISMATCH)
        return fail("thread-count mismatch accepted");

    PreflightAttestation wrong_image = attestation;
    wrong_image.session.loaded_dso_set_sha256[0] ^= 0x80U;
    if (davinci_av1_bootstrap_validate_session(&wrong_image) !=
        DAVINCI_AV1_BOOTSTRAP_IMAGE_MISMATCH)
        return fail("mapped image mismatch accepted");

    PreflightAttestation incomplete = attestation;
    incomplete.flags &= ~PREFLIGHT_LOADER_LOCKS;
    if (davinci_av1_bootstrap_validate_session(&incomplete) !=
        DAVINCI_AV1_BOOTSTRAP_INVALID_ATTESTATION)
        return fail("incomplete evidence accepted");

    if (davinci_av1_bootstrap_validate_session(NULL) !=
        DAVINCI_AV1_BOOTSTRAP_INVALID_ARGUMENT)
        return fail("null attestation accepted");

    if (davinci_av1_bootstrap_execute_prepared_transaction(
            "not the authorization phrase", 29U,
            (const DavinciAv1TransactionBindingsV1 *)(uintptr_t)1U,
            (DavinciAv1TransactionExecutionV1 *)(uintptr_t)1U) !=
        DAVINCI_AV1_BOOTSTRAP_EXECUTION_NOT_AUTHORIZED)
        return fail("execute entrypoint rejects missing authorization before pointer dereference");
    if (davinci_av1_bootstrap_rollback_prepared_transaction(
            "not the authorization phrase", 29U,
            (const DavinciAv1TransactionBindingsV1 *)(uintptr_t)1U,
            (DavinciAv1TransactionExecutionV1 *)(uintptr_t)1U) !=
        DAVINCI_AV1_BOOTSTRAP_EXECUTION_NOT_AUTHORIZED)
        return fail("rollback entrypoint rejects missing authorization before pointer dereference");
    if (davinci_av1_bootstrap_execute_prepared_transaction(
            DAVINCI_AV1_INSTALL_AUTHORIZATION_TEXT,
            sizeof(DAVINCI_AV1_INSTALL_AUTHORIZATION_TEXT) - 1U,
            NULL, NULL) != DAVINCI_AV1_BOOTSTRAP_INVALID_ARGUMENT)
        return fail("authorized execute entrypoint validates bindings without attempting execution");

    DavinciAv1TransactionStatusV1 transaction_status;
    if (davinci_av1_bootstrap_prepare_transaction(NULL,
                                                  &transaction_status) !=
        DAVINCI_AV1_BOOTSTRAP_INVALID_ARGUMENT ||
        transaction_status.state != DAVINCI_AV1_TRANSACTION_AVAILABLE)
        return fail("null transaction request rejected without consuming the one-shot gate");

    if (davinci_av1_bootstrap_prepare_transaction(
            (const DavinciAv1TransactionRequestV1 *)(uintptr_t)1U,
            &transaction_status) !=
            DAVINCI_AV1_BOOTSTRAP_TRANSACTION_REQUEST_INVALID ||
        transaction_status.state != DAVINCI_AV1_TRANSACTION_AVAILABLE)
        return fail("unmapped request pointer is rejected without dereference");

    if (davinci_av1_bootstrap_prepare_transaction(
            NULL, (DavinciAv1TransactionStatusV1 *)(uintptr_t)1U) !=
        DAVINCI_AV1_BOOTSTRAP_INVALID_ARGUMENT)
        return fail("unmapped status pointer is rejected without dereference");

    DavinciAv1TransactionRequestV1 transaction_request;
    memset(&transaction_request, 0, sizeof(transaction_request));
    transaction_request.magic = DAVINCI_AV1_TRANSACTION_REQUEST_MAGIC;
    transaction_request.version = DAVINCI_AV1_TRANSACTION_API_VERSION;
    transaction_request.struct_size = sizeof(transaction_request);
    transaction_request.attestation = &attestation;
    memcpy(transaction_request.session_nonce,
           attestation.session.session_nonce,
           sizeof(transaction_request.session_nonce));

    if (davinci_av1_bootstrap_prepare_transaction(
            (const DavinciAv1TransactionRequestV1 *)&transaction_status,
            &transaction_status) != DAVINCI_AV1_BOOTSTRAP_INVALID_ARGUMENT)
        return fail("overlapping request/status buffers are rejected");

    DavinciAv1TransactionRequestV1 malformed_request = transaction_request;
    malformed_request.attestation =
        (const PreflightAttestation *)(uintptr_t)1U;
    if (davinci_av1_bootstrap_prepare_transaction(&malformed_request,
                                                  &transaction_status) !=
        DAVINCI_AV1_BOOTSTRAP_INVALID_ARGUMENT)
        return fail("unmapped attestation pointer is rejected without dereference");

    malformed_request = transaction_request;
    malformed_request.magic ^= UINT64_C(1);
    if (davinci_av1_bootstrap_prepare_transaction(&malformed_request,
                                                  &transaction_status) !=
            DAVINCI_AV1_BOOTSTRAP_TRANSACTION_REQUEST_INVALID ||
        transaction_status.state != DAVINCI_AV1_TRANSACTION_AVAILABLE)
        return fail("bad transaction request magic rejected without consuming the gate");

    malformed_request = transaction_request;
    malformed_request.reserved_tail = 1U;
    if (davinci_av1_bootstrap_prepare_transaction(&malformed_request,
                                                  &transaction_status) !=
            DAVINCI_AV1_BOOTSTRAP_TRANSACTION_REQUEST_INVALID ||
        transaction_status.state != DAVINCI_AV1_TRANSACTION_AVAILABLE)
        return fail("nonzero transaction reserved field rejected without consuming the gate");

    if (davinci_av1_bootstrap_prepare_transaction(&transaction_request,
                                                  &transaction_status) !=
        DAVINCI_AV1_BOOTSTRAP_RESOLVE_STATE_MISMATCH)
        return fail("valid request reaches read-only Resolve-state validation");

    if (transaction_status.magic != DAVINCI_AV1_TRANSACTION_STATUS_MAGIC ||
        transaction_status.version != DAVINCI_AV1_TRANSACTION_API_VERSION ||
        transaction_status.struct_size != sizeof(transaction_status) ||
        transaction_status.result !=
            DAVINCI_AV1_BOOTSTRAP_RESOLVE_STATE_MISMATCH ||
        transaction_status.state != DAVINCI_AV1_TRANSACTION_REJECTED ||
        transaction_status.completed_stage !=
            DAVINCI_AV1_TRANSACTION_STAGE_NONE ||
        transaction_status.reserved != 0U)
        return fail("transaction status reports the terminal validation failure with exact ABI metadata");

    if (memcmp(transaction_status.session_nonce,
               transaction_request.session_nonce,
               sizeof(transaction_status.session_nonce)) != 0)
        return fail("transaction status echoes the claimed session nonce");

    if (davinci_av1_bootstrap_prepare_transaction(&transaction_request,
                                                  &transaction_status) !=
            DAVINCI_AV1_BOOTSTRAP_TRANSACTION_ALREADY_CLAIMED ||
        transaction_status.state != DAVINCI_AV1_TRANSACTION_REJECTED ||
        memcmp(transaction_status.session_nonce,
               transaction_request.session_nonce,
               sizeof(transaction_status.session_nonce)) != 0)
        return fail("one-shot gate rejects replay after a failed live-state validation");

    if (davinci_av1_bootstrap_prepare_transaction(&transaction_request, NULL) !=
        DAVINCI_AV1_BOOTSTRAP_INVALID_ARGUMENT)
        return fail("null transaction status pointer rejected");

    puts("bootstrap transaction request/status, one-shot gate and write-authorization tests: 15/15 passed");
    puts("bootstrap same-process handoff tests: 8/8 passed");
    return 0;
}
