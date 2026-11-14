#ifndef AV1_PREFLIGHT_ATTESTATION_H
#define AV1_PREFLIGHT_ATTESTATION_H

#include <stddef.h>
#include <stdint.h>

enum {
    PREFLIGHT_IMAGE_IDENTITIES = UINT64_C(1) << 0,
    PREFLIGHT_THREAD_STATE = UINT64_C(1) << 1,
    PREFLIGHT_ALLOCATOR_CHAIN = UINT64_C(1) << 2,
    PREFLIGHT_MAP_INVARIANTS = UINT64_C(1) << 3,
    PREFLIGHT_CALLBACK = UINT64_C(1) << 4,
    PREFLIGHT_CALL_SITES = UINT64_C(1) << 5,
    PREFLIGHT_REBALANCER_ABI = UINT64_C(1) << 6,
    PREFLIGHT_LOADER_LOCKS = UINT64_C(1) << 7
};

#define PREFLIGHT_MAGIC UINT64_C(0x4156315052465633)
#define PREFLIGHT_VERSION 3U
#define PREFLIGHT_EXPECTED_CHECKS 47U
#define PREFLIGHT_REQUIRED_FLAGS \
    (PREFLIGHT_IMAGE_IDENTITIES | PREFLIGHT_THREAD_STATE | \
     PREFLIGHT_ALLOCATOR_CHAIN | PREFLIGHT_MAP_INVARIANTS | \
     PREFLIGHT_CALLBACK | PREFLIGHT_CALL_SITES | \
     PREFLIGHT_REBALANCER_ABI | PREFLIGHT_LOADER_LOCKS)

typedef struct {
    uint64_t process_id;
    uint64_t process_start_ticks;
    uint32_t stopped_thread_count;
    uint32_t reserved;
    uint8_t session_nonce[16];
    uintptr_t generic_map;
    uintptr_t av1_callback;
    uint8_t executable_sha256[32];
    uint8_t loaded_dso_set_sha256[32];
    uintptr_t operator_new;
    uintptr_t sized_delete;
    uintptr_t scalable_malloc;
    uintptr_t safe_free;
} TargetSessionIdentity;

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t struct_size;
    uint32_t total_checks;
    uint32_t passed_checks;
    uint32_t failures;
    uint32_t reserved;
    uint64_t flags;
    TargetSessionIdentity session;
} PreflightAttestation;

#ifdef __cplusplus
#define AV1_PREFLIGHT_STATIC_ASSERT static_assert
#else
#define AV1_PREFLIGHT_STATIC_ASSERT _Static_assert
#endif

AV1_PREFLIGHT_STATIC_ASSERT(sizeof(uintptr_t) == 8U,
                            "preflight ABI requires 64-bit pointers");
AV1_PREFLIGHT_STATIC_ASSERT(sizeof(TargetSessionIdentity) == 152U,
                            "target session identity ABI size changed");
AV1_PREFLIGHT_STATIC_ASSERT(sizeof(PreflightAttestation) == 192U,
                            "preflight attestation ABI size changed");
AV1_PREFLIGHT_STATIC_ASSERT(offsetof(PreflightAttestation, flags) == 32U,
                            "preflight flags ABI offset changed");
AV1_PREFLIGHT_STATIC_ASSERT(offsetof(PreflightAttestation, session) == 40U,
                            "preflight session ABI offset changed");
AV1_PREFLIGHT_STATIC_ASSERT(
    offsetof(PreflightAttestation, session.operator_new) == 160U,
    "preflight allocator ABI offset changed");

#undef AV1_PREFLIGHT_STATIC_ASSERT

#endif
