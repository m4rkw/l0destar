/*
 * Crypto glue: Zephyr CSPRNG + ChaCha20-Poly1305 self-test on boot.
 * Replaces the STM32L4 HSI48/RNG init from the original crypto.ino.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include "app.h"
#include "chacha20_poly1305.h"

LOG_MODULE_REGISTER(crypto, CONFIG_APP_LOG_LEVEL);

int crypto_init(void)
{
    if (!cp_self_test()) {
        LOG_ERR("AEAD self-test FAILED");
        return -EIO;
    }
    LOG_INF("ready");
    return 0;
}

int crypto_random(uint8_t *out, size_t len)
{
    /* sys_csrand_get pulls from CONFIG_ENTROPY_GENERATOR (CryptoCell on
     * nRF9151).  Returns 0 on success, negative errno otherwise. */
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
