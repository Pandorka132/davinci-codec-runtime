#include "session_controller_api.h"

#include <cstdlib>
#include <cstddef>

namespace {

void require(bool condition) {
    if (!condition) std::abort();
}

}

int main() {
    static_assert(sizeof(DavinciAv1SessionControllerConfigV1) == 856U);
    static_assert(sizeof(DavinciAv1SessionControllerStatusV1) == 48U);
    static_assert(sizeof(DavinciAv1SessionControllerPlanV1) == 192U);
    static_assert(sizeof(DavinciAv1TransactionBindingsV1) == 112U);
    static_assert(sizeof(DavinciAv1CallbackBindingsV1) == 56U);

    require(davinci_av1_session_controller_abi_version() ==
            DAVINCI_AV1_SESSION_CONTROLLER_ABI_VERSION);

    DavinciAv1SessionControllerStatusV1 status{};
    require(davinci_av1_session_controller_get_status(&status) ==
            DAVINCI_AV1_SESSION_CONTROLLER_OK);
    require(status.magic == DAVINCI_AV1_SESSION_CONTROLLER_STATUS_MAGIC);
    require(status.version == DAVINCI_AV1_SESSION_CONTROLLER_ABI_VERSION);
    require(status.struct_size == sizeof(status));
    require(status.state == DAVINCI_AV1_SESSION_CONTROLLER_EMPTY);
    require(status.result == DAVINCI_AV1_SESSION_CONTROLLER_NOT_READY);
    require(status.provider_ready == 0U);
    require(status.callback_configured == 0U);

    require(davinci_av1_session_controller_initialize(nullptr) ==
            DAVINCI_AV1_SESSION_CONTROLLER_INVALID_ARGUMENT);

    status = {};
    require(davinci_av1_session_controller_get_status(&status) ==
            DAVINCI_AV1_SESSION_CONTROLLER_OK);
    require(status.state == DAVINCI_AV1_SESSION_CONTROLLER_FAILED);
    require(status.result ==
            DAVINCI_AV1_SESSION_CONTROLLER_INVALID_ARGUMENT);
    require(status.provider_ready == 0U);

    DavinciAv1SessionControllerConfigV1 config{};
    config.magic = DAVINCI_AV1_SESSION_CONTROLLER_CONFIG_MAGIC;
    config.version = DAVINCI_AV1_SESSION_CONTROLLER_ABI_VERSION;
    config.struct_size = sizeof(config);
    require(davinci_av1_session_controller_initialize(&config) ==
            DAVINCI_AV1_SESSION_CONTROLLER_ALREADY_USED);

    require(davinci_av1_session_controller_get_plan(nullptr, nullptr) ==
            DAVINCI_AV1_SESSION_CONTROLLER_BUFFER_INVALID);
    require(davinci_av1_session_controller_prepare(nullptr, nullptr) ==
            DAVINCI_AV1_SESSION_CONTROLLER_BUFFER_INVALID);

    DavinciAv1TransactionExecutionV1 execution{};
    require(davinci_av1_session_controller_execute(
                "not authorized", 13U, &execution) ==
            DAVINCI_AV1_SESSION_CONTROLLER_AUTHORIZATION_REQUIRED);
    require(davinci_av1_session_controller_rollback(
                "not authorized", 13U, &execution) ==
            DAVINCI_AV1_SESSION_CONTROLLER_AUTHORIZATION_REQUIRED);
    require(davinci_av1_session_controller_execute(
                reinterpret_cast<const char *>(1U),
                sizeof(DAVINCI_AV1_INSTALL_AUTHORIZATION_TEXT) - 1U,
                &execution) ==
            DAVINCI_AV1_SESSION_CONTROLLER_AUTHORIZATION_REQUIRED);
    require(davinci_av1_session_controller_rollback(
                DAVINCI_AV1_INSTALL_AUTHORIZATION_TEXT,
                sizeof(DAVINCI_AV1_INSTALL_AUTHORIZATION_TEXT) - 1U,
                &execution) == DAVINCI_AV1_SESSION_CONTROLLER_NOT_READY);
    require(execution.state == DAVINCI_AV1_EXECUTION_EMPTY);
    return 0;
}
