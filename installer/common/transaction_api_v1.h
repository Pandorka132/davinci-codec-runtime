#ifndef DAVINCI_AV1_TRANSACTION_API_V1_H
#define DAVINCI_AV1_TRANSACTION_API_V1_H

#include <stddef.h>
#include <stdint.h>

#include "preflight_attestation_v3.h"

#define DAVINCI_AV1_TRANSACTION_API_VERSION 1U
#define DAVINCI_AV1_TRANSACTION_REQUEST_MAGIC UINT64_C(0x4156315458525131)
#define DAVINCI_AV1_TRANSACTION_STATUS_MAGIC UINT64_C(0x4156315458535331)

/* The request is a same-process ABI; attestation storage must remain readable
 * for the duration of the call. All reserved fields and flags must be zero. */
typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t struct_size;
    uint32_t flags;
    uint32_t reserved;
    const PreflightAttestation *attestation;
    uint8_t session_nonce[16];
    uint64_t reserved_tail;
} DavinciAv1TransactionRequestV1;

enum DavinciAv1TransactionState {
    DAVINCI_AV1_TRANSACTION_AVAILABLE = 0U,
    DAVINCI_AV1_TRANSACTION_VALIDATING = 1U,
    DAVINCI_AV1_TRANSACTION_PREFLIGHT_READY = 2U,
    DAVINCI_AV1_TRANSACTION_REJECTED = 3U,
    DAVINCI_AV1_TRANSACTION_EXECUTING = 4U,
    DAVINCI_AV1_TRANSACTION_COMMITTED = 5U,
    DAVINCI_AV1_TRANSACTION_ROLLED_BACK = 6U,
    DAVINCI_AV1_TRANSACTION_FAILED = 7U,
    DAVINCI_AV1_TRANSACTION_FAIL_STOP = 8U
};

enum DavinciAv1TransactionStage {
    DAVINCI_AV1_TRANSACTION_STAGE_NONE = 0U,
    DAVINCI_AV1_TRANSACTION_STAGE_PREFLIGHT_VALIDATED = 1U,
    DAVINCI_AV1_TRANSACTION_STAGE_SNAPSHOT_CAPTURED = 2U
};

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t struct_size;
    int32_t result;
    uint32_t state;
    uint32_t completed_stage;
    uint32_t reserved;
    uint8_t session_nonce[16];
} DavinciAv1TransactionStatusV1;

#ifdef __cplusplus
#define DAVINCI_AV1_TRANSACTION_STATIC_ASSERT static_assert
#else
#define DAVINCI_AV1_TRANSACTION_STATIC_ASSERT _Static_assert
#endif

DAVINCI_AV1_TRANSACTION_STATIC_ASSERT(sizeof(uintptr_t) == 8U,
                                      "transaction ABI requires 64-bit pointers");
DAVINCI_AV1_TRANSACTION_STATIC_ASSERT(
    sizeof(DavinciAv1TransactionRequestV1) == 56U,
    "transaction request ABI size changed");
DAVINCI_AV1_TRANSACTION_STATIC_ASSERT(
    offsetof(DavinciAv1TransactionRequestV1, attestation) == 24U,
    "transaction request attestation offset changed");
DAVINCI_AV1_TRANSACTION_STATIC_ASSERT(
    offsetof(DavinciAv1TransactionRequestV1, session_nonce) == 32U,
    "transaction request nonce offset changed");
DAVINCI_AV1_TRANSACTION_STATIC_ASSERT(
    sizeof(DavinciAv1TransactionStatusV1) == 48U,
    "transaction status ABI size changed");
DAVINCI_AV1_TRANSACTION_STATIC_ASSERT(
    offsetof(DavinciAv1TransactionStatusV1, session_nonce) == 32U,
    "transaction status nonce offset changed");

#undef DAVINCI_AV1_TRANSACTION_STATIC_ASSERT

#endif
