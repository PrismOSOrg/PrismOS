#include "util/sha256.h"

static const uint32_t round_constants[64] = {
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U,
    0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U,
    0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U,
    0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U,
    0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU,
    0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
    0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
    0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U,
    0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U,
    0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U,
    0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U,
    0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
    0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U,
    0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
    0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U,
    0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U
};

static uint32_t rotate_right(uint32_t value, uint32_t count) {
    return (value >> count) | (value << (32U - count));
}

static uint32_t read_be32(const uint8_t* bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16)
        | ((uint32_t)bytes[2] << 8) | bytes[3];
}

static void write_be64(uint8_t* bytes, uint64_t value) {
    for (uint32_t index = 0U; index < 8U; index++) {
        bytes[index] = (uint8_t)(value >> (56U - index * 8U));
    }
}

static void transform(sha256_context_t* context, const uint8_t block[64]) {
    uint32_t words[64];
    uint32_t a, b, c, d, e, f, g, h;
    for (uint32_t index = 0U; index < 16U; index++) words[index] = read_be32(&block[index * 4U]);
    for (uint32_t index = 16U; index < 64U; index++) {
        uint32_t x = words[index - 15U];
        uint32_t y = words[index - 2U];
        uint32_t s0 = rotate_right(x, 7U) ^ rotate_right(x, 18U) ^ (x >> 3U);
        uint32_t s1 = rotate_right(y, 17U) ^ rotate_right(y, 19U) ^ (y >> 10U);
        words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }
    a = context->state[0]; b = context->state[1];
    c = context->state[2]; d = context->state[3];
    e = context->state[4]; f = context->state[5];
    g = context->state[6]; h = context->state[7];
    for (uint32_t index = 0U; index < 64U; index++) {
        uint32_t s1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
        uint32_t choice = (e & f) ^ (~e & g);
        uint32_t temp1 = h + s1 + choice + round_constants[index] + words[index];
        uint32_t s0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + majority;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    context->state[0] += a; context->state[1] += b;
    context->state[2] += c; context->state[3] += d;
    context->state[4] += e; context->state[5] += f;
    context->state[6] += g; context->state[7] += h;
}

void sha256_init(sha256_context_t* context) {
    static const uint32_t initial_state[8] = {
        0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
        0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U
    };
    for (uint32_t index = 0U; index < 8U; index++) context->state[index] = initial_state[index];
    context->bit_count = 0U;
    context->block_length = 0U;
}

void sha256_update(sha256_context_t* context, const uint8_t* data, uint32_t length) {
    for (uint32_t index = 0U; index < length; index++) {
        context->block[context->block_length++] = data[index];
        if (context->block_length == 64U) {
            transform(context, context->block);
            context->bit_count += 512U;
            context->block_length = 0U;
        }
    }
}

void sha256_final(sha256_context_t* context, uint8_t digest[32]) {
    uint32_t index = context->block_length;
    context->bit_count += (uint64_t)context->block_length * 8U;
    context->block[index++] = 0x80U;
    if (index > 56U) {
        while (index < 64U) context->block[index++] = 0U;
        transform(context, context->block);
        index = 0U;
    }
    while (index < 56U) context->block[index++] = 0U;
    write_be64(&context->block[56U], context->bit_count);
    transform(context, context->block);
    for (uint32_t word = 0U; word < 8U; word++) {
        digest[word * 4U] = (uint8_t)(context->state[word] >> 24);
        digest[word * 4U + 1U] = (uint8_t)(context->state[word] >> 16);
        digest[word * 4U + 2U] = (uint8_t)(context->state[word] >> 8);
        digest[word * 4U + 3U] = (uint8_t)context->state[word];
    }
}