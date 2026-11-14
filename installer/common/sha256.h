#ifndef DAVINCI_SHA256_H
#define DAVINCI_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t block[64];
    size_t block_length;
} DavinciSha256;

void davinci_sha256_init(DavinciSha256 *context);
void davinci_sha256_update(DavinciSha256 *context,
                           const void *data, size_t length);
void davinci_sha256_final(DavinciSha256 *context, uint8_t digest[32]);

#endif
