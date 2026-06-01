// ChaCha20-Poly1305 AEAD, RFC 8439.
// Single-purpose implementation for the tracker UDP envelope. No streaming,
// no incremental API, no key derivation — one-shot seal/open over a buffer.
//
// Constant-time tag comparison; ChaCha and Poly1305 themselves are naturally
// CT under the operations used. Not hardened against power/EM side channels.

#ifndef CHACHA20_POLY1305_H
#define CHACHA20_POLY1305_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CP_KEY_BYTES   32
#define CP_NONCE_BYTES 12
#define CP_TAG_BYTES   16

// Encrypt and authenticate.
//   key:  32 bytes
//   nonce: 12 bytes (must be unique per key)
//   aad / aad_len: associated data, authenticated but not encrypted
//   pt / pt_len: plaintext (in)
//   ct: ciphertext (out, pt_len bytes; may alias pt)
//   tag: 16 bytes (out)
void cp_seal(const uint8_t key[CP_KEY_BYTES],
             const uint8_t nonce[CP_NONCE_BYTES],
             const uint8_t *aad, size_t aad_len,
             const uint8_t *pt, size_t pt_len,
             uint8_t *ct, uint8_t tag[CP_TAG_BYTES]);

// Verify and decrypt.
// Returns 1 on success, 0 if the tag is invalid (in which case pt is not
// touched). Same parameter rules as cp_seal.
int cp_open(const uint8_t key[CP_KEY_BYTES],
            const uint8_t nonce[CP_NONCE_BYTES],
            const uint8_t *aad, size_t aad_len,
            const uint8_t *ct, size_t ct_len,
            const uint8_t tag[CP_TAG_BYTES],
            uint8_t *pt);

// RFC 8439 §2.8.2 self-test. Returns 1 on success, 0 on any mismatch.
// Cheap (one ~114-byte seal + one open). Run once at boot.
int cp_self_test(void);

#ifdef __cplusplus
}
#endif

#endif
