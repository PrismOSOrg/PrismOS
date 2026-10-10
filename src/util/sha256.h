#ifndef PRISMOS_UTIL_SHA256_H
#define PRISMOS_UTIL_SHA256_H

#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t block[64];
    uint32_t block_length;
} sha256_context_t;

void sha256_init(sha256_context_t* context);
void sha256_update(sha256_context_t* context, const uint8_t* data, uint32_t length);
void sha256_final(sha256_context_t* context, uint8_t digest[32]);

#endif