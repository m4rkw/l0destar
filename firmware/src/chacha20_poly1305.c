// ChaCha20-Poly1305 AEAD per RFC 8439. See chacha20_poly1305.h for the API.
//
// Implementation notes:
//   - ChaCha20 follows §2.3; quarter-round inlined.
//   - Poly1305 uses 5 26-bit limbs (radix 2^26) — fits 32x32->64 multiplies
//     well on Cortex-M4 with UMULL. See §2.5.
//   - AEAD construction is §2.8: Poly1305 key derived from ChaCha20 block 0,
//     then ciphertext from block 1+, then MAC over aad||pad||ct||pad||lens.

#include "chacha20_poly1305.h"
#include <string.h>

// -- little-endian load/store ------------------------------------------------

static uint32_t ld32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void st32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;       p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void st64le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

// -- ChaCha20 ----------------------------------------------------------------

#define ROL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define QR(a,b,c,d) do { \
    a += b; d ^= a; d = ROL32(d, 16); \
    c += d; b ^= c; b = ROL32(b, 12); \
    a += b; d ^= a; d = ROL32(d,  8); \
    c += d; b ^= c; b = ROL32(b,  7); \
} while (0)

static void chacha20_block(const uint32_t in[16], uint8_t out[64]) {
    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = in[i];
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[ 8], x[12]);
        QR(x[1], x[5], x[ 9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[ 8], x[13]);
        QR(x[3], x[4], x[ 9], x[14]);
    }
    for (int i = 0; i < 16; i++) st32(out + 4 * i, x[i] + in[i]);
}

static void chacha20_init(uint32_t state[16],
                          const uint8_t key[32], uint32_t counter,
                          const uint8_t nonce[12]) {
    state[ 0] = 0x61707865; state[ 1] = 0x3320646e;
    state[ 2] = 0x79622d32; state[ 3] = 0x6b206574;
    for (int i = 0; i < 8; i++) state[4 + i] = ld32(key + 4 * i);
    state[12] = counter;
    state[13] = ld32(nonce + 0);
    state[14] = ld32(nonce + 4);
    state[15] = ld32(nonce + 8);
}

static void chacha20_xor(const uint8_t key[32], uint32_t counter,
                         const uint8_t nonce[12],
                         const uint8_t *in, uint8_t *out, size_t len) {
    uint32_t state[16];
    chacha20_init(state, key, counter, nonce);
    uint8_t blk[64];
    while (len) {
        chacha20_block(state, blk);
        state[12]++;
        size_t n = len < 64 ? len : 64;
        for (size_t i = 0; i < n; i++) out[i] = in[i] ^ blk[i];
        in += n; out += n; len -= n;
    }
}

// -- Poly1305 (radix-2^26) ---------------------------------------------------

typedef struct {
    uint32_t r[5];     // clamped key, 26-bit limbs
    uint32_t h[5];     // accumulator, 26-bit limbs (may carry into h[4])
    uint32_t pad[4];   // 's' half of the one-time key, little-endian
    uint8_t  buf[16];
    size_t   buf_used;
} poly1305_ctx;

static void poly1305_init(poly1305_ctx *ctx, const uint8_t key[32]) {
    uint32_t t0 = ld32(key + 0);
    uint32_t t1 = ld32(key + 4);
    uint32_t t2 = ld32(key + 8);
    uint32_t t3 = ld32(key + 12);
    // Clamp r and split into 26-bit limbs (RFC 8439 §2.5.1).
    ctx->r[0] = (t0                     ) & 0x03ffffff;
    ctx->r[1] = ((t0 >> 26) | (t1 <<  6)) & 0x03ffff03;
    ctx->r[2] = ((t1 >> 20) | (t2 << 12)) & 0x03ffc0ff;
    ctx->r[3] = ((t2 >> 14) | (t3 << 18)) & 0x03f03fff;
    ctx->r[4] = ((t3 >>  8)             ) & 0x000fffff;

    for (int i = 0; i < 5; i++) ctx->h[i] = 0;
    for (int i = 0; i < 4; i++) ctx->pad[i] = ld32(key + 16 + 4 * i);
    ctx->buf_used = 0;
}

// Add one 16-byte block (or final partial block) to the accumulator.
// `final` non-zero means the high bit is omitted (caller has padded the
// block with the 0x01 byte and zeros, no 2^128 high bit added).
static void poly1305_block(poly1305_ctx *ctx, const uint8_t blk[16], int final) {
    uint32_t t0 = ld32(blk + 0);
    uint32_t t1 = ld32(blk + 4);
    uint32_t t2 = ld32(blk + 8);
    uint32_t t3 = ld32(blk + 12);

    // h += blk (with implied high bit)
    uint32_t h0 = ctx->h[0] + ((t0                     ) & 0x03ffffff);
    uint32_t h1 = ctx->h[1] + (((t0 >> 26) | (t1 <<  6)) & 0x03ffffff);
    uint32_t h2 = ctx->h[2] + (((t1 >> 20) | (t2 << 12)) & 0x03ffffff);
    uint32_t h3 = ctx->h[3] + (((t2 >> 14) | (t3 << 18)) & 0x03ffffff);
    uint32_t h4 = ctx->h[4] + ((t3 >>  8) | (final ? 0 : (1u << 24)));

    // h *= r, then reduce mod 2^130 - 5.
    uint32_t r0 = ctx->r[0], r1 = ctx->r[1], r2 = ctx->r[2],
             r3 = ctx->r[3], r4 = ctx->r[4];
    // Pre-multiplied by 5 for the wraparound terms.
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;

    uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3
                + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
    uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4
                + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
    uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0
                + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
    uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1
                + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
    uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2
                + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

    // Carry-propagate, with the 2^130 ≡ 5 (mod p) reduction folded back.
    uint32_t c;
    c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x03ffffff; d1 += c;
    c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x03ffffff; d2 += c;
    c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x03ffffff; d3 += c;
    c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x03ffffff; d4 += c;
    c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x03ffffff;
    h0 += c * 5;
    c = h0 >> 26; h0 &= 0x03ffffff; h1 += c;

    ctx->h[0] = h0; ctx->h[1] = h1; ctx->h[2] = h2; ctx->h[3] = h3; ctx->h[4] = h4;
}

static void poly1305_update(poly1305_ctx *ctx, const uint8_t *m, size_t len) {
    if (ctx->buf_used) {
        size_t want = 16 - ctx->buf_used;
        if (want > len) want = len;
        memcpy(ctx->buf + ctx->buf_used, m, want);
        ctx->buf_used += want; m += want; len -= want;
        if (ctx->buf_used == 16) {
            poly1305_block(ctx, ctx->buf, 0);
            ctx->buf_used = 0;
        }
    }
    while (len >= 16) {
        poly1305_block(ctx, m, 0);
        m += 16; len -= 16;
    }
    if (len) {
        memcpy(ctx->buf, m, len);
        ctx->buf_used = len;
    }
}

static void poly1305_finish(poly1305_ctx *ctx, uint8_t tag[16]) {
    if (ctx->buf_used) {
        ctx->buf[ctx->buf_used++] = 0x01;
        while (ctx->buf_used < 16) ctx->buf[ctx->buf_used++] = 0;
        poly1305_block(ctx, ctx->buf, 1);
    }

    // Final reduction: ensure h < 2^130 - 5.
    uint32_t h0 = ctx->h[0], h1 = ctx->h[1], h2 = ctx->h[2],
             h3 = ctx->h[3], h4 = ctx->h[4];
    uint32_t c;
    c = h1 >> 26; h1 &= 0x03ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x03ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x03ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x03ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x03ffffff; h1 += c;

    // g = h + 5; if g >= 2^130, h was >= p, so the canonical value is g-2^130.
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x03ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x03ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x03ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x03ffffff;
    uint32_t g4 = h4 + c - (1u << 26);   // borrows iff h+5 < 2^130

    // mask = all-ones if no borrow (take g), else 0 (take h).
    uint32_t mask = (g4 >> 31) - 1;
    h0 ^= (h0 ^ g0) & mask;
    h1 ^= (h1 ^ g1) & mask;
    h2 ^= (h2 ^ g2) & mask;
    h3 ^= (h3 ^ g3) & mask;
    h4 ^= (h4 ^ g4) & mask;

    // Pack 5x26-bit -> 4x32-bit.
    uint32_t f0 = (h0      ) | (h1 << 26);
    uint32_t f1 = (h1 >>  6) | (h2 << 20);
    uint32_t f2 = (h2 >> 12) | (h3 << 14);
    uint32_t f3 = (h3 >> 18) | (h4 <<  8);

    // tag = (h + s) mod 2^128
    uint64_t t;
    t = (uint64_t)f0 + ctx->pad[0];               st32(tag +  0, (uint32_t)t);
    t = (uint64_t)f1 + ctx->pad[1] + (t >> 32);   st32(tag +  4, (uint32_t)t);
    t = (uint64_t)f2 + ctx->pad[2] + (t >> 32);   st32(tag +  8, (uint32_t)t);
    t = (uint64_t)f3 + ctx->pad[3] + (t >> 32);   st32(tag + 12, (uint32_t)t);
}

// -- AEAD --------------------------------------------------------------------

static void poly1305_pad16(poly1305_ctx *ctx, size_t n) {
    static const uint8_t zero[16] = {0};
    if (n & 15) poly1305_update(ctx, zero, 16 - (n & 15));
}

static void aead_mac(const uint8_t poly_key[32],
                     const uint8_t *aad, size_t aad_len,
                     const uint8_t *ct,  size_t ct_len,
                     uint8_t tag[16]) {
    poly1305_ctx ctx;
    poly1305_init(&ctx, poly_key);
    poly1305_update(&ctx, aad, aad_len); poly1305_pad16(&ctx, aad_len);
    poly1305_update(&ctx, ct,  ct_len);  poly1305_pad16(&ctx, ct_len);
    uint8_t lens[16];
    st64le(lens + 0, (uint64_t)aad_len);
    st64le(lens + 8, (uint64_t)ct_len);
    poly1305_update(&ctx, lens, 16);
    poly1305_finish(&ctx, tag);
}

void cp_seal(const uint8_t key[32], const uint8_t nonce[12],
             const uint8_t *aad, size_t aad_len,
             const uint8_t *pt,  size_t pt_len,
             uint8_t *ct, uint8_t tag[16]) {
    uint8_t poly_key[64];
    static const uint8_t zero[64] = {0};
    chacha20_xor(key, 0, nonce, zero, poly_key, 32);
    chacha20_xor(key, 1, nonce, pt, ct, pt_len);
    aead_mac(poly_key, aad, aad_len, ct, pt_len, tag);
}

int cp_open(const uint8_t key[32], const uint8_t nonce[12],
            const uint8_t *aad, size_t aad_len,
            const uint8_t *ct, size_t ct_len,
            const uint8_t tag[16], uint8_t *pt) {
    uint8_t poly_key[64];
    static const uint8_t zero[64] = {0};
    chacha20_xor(key, 0, nonce, zero, poly_key, 32);
    uint8_t got[16];
    aead_mac(poly_key, aad, aad_len, ct, ct_len, got);
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= got[i] ^ tag[i];
    if (diff) return 0;
    chacha20_xor(key, 1, nonce, ct, pt, ct_len);
    return 1;
}

// -- self-test (RFC 8439 §2.8.2) ---------------------------------------------

int cp_self_test(void) {
    static const uint8_t key[32] = {
        0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,
        0x88,0x89,0x8a,0x8b,0x8c,0x8d,0x8e,0x8f,
        0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,
        0x98,0x99,0x9a,0x9b,0x9c,0x9d,0x9e,0x9f,
    };
    static const uint8_t nonce[12] = {
        0x07,0x00,0x00,0x00,0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47,
    };
    static const uint8_t aad[12] = {
        0x50,0x51,0x52,0x53,0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,
    };
    static const char pt_str[] =
        "Ladies and Gentlemen of the class of '99: If I could offer you "
        "only one tip for the future, sunscreen would be it.";
    const uint8_t *pt = (const uint8_t *)pt_str;
    const size_t pt_len = sizeof pt_str - 1;  // 114
    static const uint8_t want_ct[114] = {
        0xd3,0x1a,0x8d,0x34,0x64,0x8e,0x60,0xdb,0x7b,0x86,0xaf,0xbc,0x53,0xef,0x7e,0xc2,
        0xa4,0xad,0xed,0x51,0x29,0x6e,0x08,0xfe,0xa9,0xe2,0xb5,0xa7,0x36,0xee,0x62,0xd6,
        0x3d,0xbe,0xa4,0x5e,0x8c,0xa9,0x67,0x12,0x82,0xfa,0xfb,0x69,0xda,0x92,0x72,0x8b,
        0x1a,0x71,0xde,0x0a,0x9e,0x06,0x0b,0x29,0x05,0xd6,0xa5,0xb6,0x7e,0xcd,0x3b,0x36,
        0x92,0xdd,0xbd,0x7f,0x2d,0x77,0x8b,0x8c,0x98,0x03,0xae,0xe3,0x28,0x09,0x1b,0x58,
        0xfa,0xb3,0x24,0xe4,0xfa,0xd6,0x75,0x94,0x55,0x85,0x80,0x8b,0x48,0x31,0xd7,0xbc,
        0x3f,0xf4,0xde,0xf0,0x8e,0x4b,0x7a,0x9d,0xe5,0x76,0xd2,0x65,0x86,0xce,0xc6,0x4b,
        0x61,0x16,
    };
    static const uint8_t want_tag[16] = {
        0x1a,0xe1,0x0b,0x59,0x4f,0x09,0xe2,0x6a,0x7e,0x90,0x2e,0xcb,0xd0,0x60,0x06,0x91,
    };

    uint8_t ct[114], tag[16], pt2[114];
    cp_seal(key, nonce, aad, sizeof aad, pt, pt_len, ct, tag);
    if (memcmp(ct, want_ct, sizeof ct) != 0) return 0;
    if (memcmp(tag, want_tag, sizeof tag) != 0) return 0;
    if (!cp_open(key, nonce, aad, sizeof aad, ct, sizeof ct, tag, pt2)) return 0;
    if (memcmp(pt, pt2, pt_len) != 0) return 0;
    // Forgery: flip one ciphertext byte, expect open to fail.
    ct[0] ^= 1;
    if (cp_open(key, nonce, aad, sizeof aad, ct, sizeof ct, tag, pt2)) return 0;
    return 1;
}
