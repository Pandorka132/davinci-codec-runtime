#include "sha256.h"

#include <string.h>

static const uint32_t round_constants[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static uint32_t rotate_right(uint32_t value, unsigned count) {
    return (value >> count) | (value << (32U - count));
}

static uint32_t load_be32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24U) |
           ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) |
           (uint32_t)bytes[3];
}

static void store_be32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

static void store_be64(uint8_t *bytes, uint64_t value) {
    for (unsigned index = 0; index < 8U; ++index)
        bytes[index] = (uint8_t)(value >> (56U - index * 8U));
}

static void transform(DavinciSha256 *context, const uint8_t block[64]) {
    uint32_t words[64];
    for (unsigned index = 0; index < 16U; ++index)
        words[index] = load_be32(block + index * 4U);

    for (unsigned index = 16U; index < 64U; ++index) {
        uint32_t x = words[index - 15U];
        uint32_t y = words[index - 2U];
        uint32_t small_sigma0 =
            rotate_right(x, 7U) ^ rotate_right(x, 18U) ^ (x >> 3U);
        uint32_t small_sigma1 =
            rotate_right(y, 17U) ^ rotate_right(y, 19U) ^ (y >> 10U);
        words[index] = words[index - 16U] + small_sigma0 +
                       words[index - 7U] + small_sigma1;
    }

    uint32_t a = context->state[0];
    uint32_t b = context->state[1];
    uint32_t c = context->state[2];
    uint32_t d = context->state[3];
    uint32_t e = context->state[4];
    uint32_t f = context->state[5];
    uint32_t g = context->state[6];
    uint32_t h = context->state[7];

    for (unsigned index = 0; index < 64U; ++index) {
        uint32_t big_sigma1 =
            rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
        uint32_t choose = (e & f) ^ (~e & g);
        uint32_t temporary1 = h + big_sigma1 + choose +
                              round_constants[index] + words[index];
        uint32_t big_sigma0 =
            rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temporary2 = big_sigma0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }

    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

void davinci_sha256_init(DavinciSha256 *context) {
    static const uint32_t initial_state[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };
    memcpy(context->state, initial_state, sizeof(initial_state));
    context->bit_count = 0;
    context->block_length = 0;
}

void davinci_sha256_update(DavinciSha256 *context,
                           const void *data, size_t length) {
    const uint8_t *bytes = data;
    while (length != 0U) {
        size_t available = sizeof(context->block) - context->block_length;
        size_t amount = length < available ? length : available;
        memcpy(context->block + context->block_length, bytes, amount);
        context->block_length += amount;
        context->bit_count += (uint64_t)amount * 8U;
        bytes += amount;
        length -= amount;

        if (context->block_length == sizeof(context->block)) {
            transform(context, context->block);
            context->block_length = 0;
        }
    }
}

void davinci_sha256_final(DavinciSha256 *context, uint8_t digest[32]) {
    uint64_t original_bit_count = context->bit_count;
    context->block[context->block_length++] = 0x80U;

    if (context->block_length > 56U) {
        memset(context->block + context->block_length, 0,
               sizeof(context->block) - context->block_length);
        transform(context, context->block);
        context->block_length = 0;
    }

    memset(context->block + context->block_length, 0,
           56U - context->block_length);
    store_be64(context->block + 56U, original_bit_count);
    transform(context, context->block);

    for (unsigned index = 0; index < 8U; ++index)
        store_be32(digest + index * 4U, context->state[index]);

    memset(context, 0, sizeof(*context));
}
