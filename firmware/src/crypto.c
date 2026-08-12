/*
 * Crypto: ChaCha20-Poly1305 AEAD via PSA Crypto, CSPRNG, PSK hex parsing.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <psa/crypto.h>

#include "app.h"

LOG_MODULE_REGISTER(crypto, CONFIG_APP_LOG_LEVEL);

int crypto_init(void)
{
    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        LOG_ERR("psa_crypto_init: %d", (int)st);
        return -EIO;
    }
    LOG_INF("ready");
    return 0;
}

int crypto_random(uint8_t *out, size_t len)
{
    return sys_csrand_get(out, len) == 0;
}

int crypto_psk_from_hex(const char *hex, uint8_t out[32])
{
    for (int i = 0; i < 32; i++) {
        int hi = -1, lo = -1;
        char a = hex[2 * i], b = hex[2 * i + 1];
        if      (a >= '0' && a <= '9') hi = a - '0';
        else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
        else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;
        if      (b >= '0' && b <= '9') lo = b - '0';
        else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
        else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;
        if (hi < 0 || lo < 0) return 0;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

int crypto_encrypt(const uint8_t *pt, size_t pt_len,
                   const uint8_t *aad, size_t aad_len,
                   const uint8_t nonce[12],
                   uint8_t *out, size_t out_size, size_t *out_len)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_CHACHA20);
    psa_set_key_algorithm(&attr, PSA_ALG_CHACHA20_POLY1305);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);

    psa_key_id_t kid;
    psa_status_t st = psa_import_key(&attr, g_settings.psk, 32, &kid);
    if (st != PSA_SUCCESS) {
        LOG_ERR("psa_import_key: %d", (int)st);
        return -EIO;
    }

    st = psa_aead_encrypt(kid, PSA_ALG_CHACHA20_POLY1305,
                          nonce, 12, aad, aad_len,
                          pt, pt_len, out, out_size, out_len);
    psa_destroy_key(kid);
    if (st != PSA_SUCCESS) {
        LOG_ERR("psa_aead_encrypt: %d", (int)st);
        return -EIO;
    }
    return 0;
}

int crypto_decrypt(const uint8_t *ct, size_t ct_len,
                   const uint8_t *aad, size_t aad_len,
                   const uint8_t nonce[12],
                   uint8_t *out, size_t out_size, size_t *out_len)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_CHACHA20);
    psa_set_key_algorithm(&attr, PSA_ALG_CHACHA20_POLY1305);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DECRYPT);

    psa_key_id_t kid;
    psa_status_t st = psa_import_key(&attr, g_settings.psk, 32, &kid);
    if (st != PSA_SUCCESS) {
        LOG_ERR("psa_import_key: %d", (int)st);
        return -EIO;
    }

    st = psa_aead_decrypt(kid, PSA_ALG_CHACHA20_POLY1305,
                          nonce, 12, aad, aad_len,
                          ct, ct_len, out, out_size, out_len);
    psa_destroy_key(kid);
    if (st != PSA_SUCCESS) {
        LOG_WRN("psa_aead_decrypt: %d", (int)st);
        return -EACCES;
    }
    return 0;
}
