#ifndef DAVINCI_AV1_INSTALL_AUTHORIZATION_H
#define DAVINCI_AV1_INSTALL_AUTHORIZATION_H

#include <stddef.h>
#include <string.h>

/* This phrase must be supplied explicitly for each transaction operation.
 * Merely loading/preparing the controller does not authorize target writes. */
#define DAVINCI_AV1_INSTALL_AUTHORIZATION_TEXT \
    "I AUTHORIZE AV1 PATCH IN THIS RESOLVE PROCESS"

static inline int davinci_av1_install_authorization_is_valid(
    const char *token, size_t token_size) {
    static const char expected[] = DAVINCI_AV1_INSTALL_AUTHORIZATION_TEXT;
    return token != NULL && token_size == sizeof(expected) - 1U &&
           memcmp(token, expected, sizeof(expected) - 1U) == 0;
}

#endif
