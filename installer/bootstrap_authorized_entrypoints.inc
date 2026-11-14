

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
