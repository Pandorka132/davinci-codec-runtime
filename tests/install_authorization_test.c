#include "common/install_authorization.h"

#include <stddef.h>
#include <string.h>

int main(void) {
    static const char accepted[] = DAVINCI_AV1_INSTALL_AUTHORIZATION_TEXT;
    static const char wrong[] = "I AUTHORIZE SOME OTHER ACTION";
    if (davinci_av1_install_authorization_is_valid(
            accepted, sizeof(accepted) - 1U) == 0)
        return 1;
    if (davinci_av1_install_authorization_is_valid(
            accepted, sizeof(accepted)) != 0)
        return 2;
    if (davinci_av1_install_authorization_is_valid(
            wrong, sizeof(wrong) - 1U) != 0)
        return 3;
    if (davinci_av1_install_authorization_is_valid(NULL, 0U) != 0)
        return 4;
    if (davinci_av1_install_authorization_is_valid(
            accepted, sizeof(accepted) - 2U) != 0)
        return 5;
    return 0;
}
