/*
 * Encrypted UDP transport.  Envelope format (unchanged from the legacy
 * firmware):
 *
 *   imei_len(1) | IMEI | nonce(12) | ciphertext | tag(16)
 *
 * IMEI is bound as AAD so envelopes can't be replayed across devices.
 * Sockets are offloaded to the modem (CONFIG_NET_SOCKETS_OFFLOAD=y), so the
 * regular POSIX-shaped API talks directly to the modem stack.
 */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>

#include "app.h"
#include "chacha20_poly1305.h"

LOG_MODULE_REGISTER(transport, CONFIG_APP_LOG_LEVEL);

#define SERVER_HOST \
    (sizeof(CONFIG_APP_SERVER_HOST) > 1 ? CONFIG_APP_SERVER_HOST : HOSTNAME)
#define SERVER_PORT \
    (CONFIG_APP_SERVER_PORT > 0 ? CONFIG_APP_SERVER_PORT : UDP_PORT)

static int s_sock = -1;
static struct sockaddr_in s_server;

int transport_open(void)
{
    if (s_sock >= 0) return 0;

    struct zsock_addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_DGRAM,
        .ai_protocol = IPPROTO_UDP,
    };
    struct zsock_addrinfo *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", SERVER_PORT);

    int err = zsock_getaddrinfo(SERVER_HOST, port_str, &hints, &res);
    if (err) {
        LOG_ERR("getaddrinfo(%s): %d", SERVER_HOST, err);
        return -EIO;
    }
    memcpy(&s_server, res->ai_addr, sizeof(s_server));
    zsock_freeaddrinfo(res);

    s_sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        LOG_ERR("socket: %d", errno);
        return -errno;
    }

    /* recv timeout — used by transport_recv_response */
    struct zsock_timeval tv = { .tv_sec = 0, .tv_usec = 0 };
    zsock_setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    LOG_INF("socket ready");
    return 0;
}

void transport_close(void)
{
    if (s_sock >= 0) {
        zsock_close(s_sock);
        s_sock = -1;
    }
}

int transport_send(const uint8_t *plaintext, size_t pt_len)
{
    if (s_sock < 0) {
        int err = transport_open();
        if (err) return err;
    }

    /* Refuse to send under an all-zero key — that would leak plaintext under
     * a known/static key.  See settings.c. */
    uint8_t zero[32] = {0};
    if (memcmp(g_settings.psk, zero, sizeof(zero)) == 0) {
        LOG_WRN("PSK unset, dropping packet");
        return -EACCES;
    }

    size_t imei_len = strlen(g_settings.imei);
    if (imei_len == 0 || imei_len > 20) {
        LOG_WRN("IMEI not set, dropping packet");
        return -EACCES;
    }

    /* Envelope build */
    uint8_t pkt[UDP_PACKET_SIZE];
    size_t off = 0;

    if (1 + imei_len + 12 + pt_len + 16 > sizeof(pkt)) {
        LOG_ERR("packet too large: %u", (unsigned)pt_len);
        return -EMSGSIZE;
    }

    pkt[off++] = (uint8_t)imei_len;
    memcpy(pkt + off, g_settings.imei, imei_len);
    off += imei_len;

    uint8_t *nonce = pkt + off;
    if (!crypto_random(nonce, 12)) {
        LOG_ERR("nonce gen failed");
        return -EIO;
    }
    off += 12;

    uint8_t *ct  = pkt + off;
    uint8_t *tag = pkt + off + pt_len;
    cp_seal(g_settings.psk, nonce,
            (const uint8_t *)g_settings.imei, imei_len,
            plaintext, pt_len, ct, tag);
    off += pt_len + 16;

    int sent = zsock_sendto(s_sock, pkt, off, 0,
                            (struct sockaddr *)&s_server, sizeof(s_server));
    if (sent < 0) {
        LOG_ERR("sendto: %d", errno);
        return -errno;
    }
    LOG_INF("sent %d bytes", sent);
    return 0;
}

int transport_recv_response(char *out_plaintext, size_t out_len, int timeout_ms)
{
    if (s_sock < 0) return -ENOTCONN;

    struct zsock_timeval tv = {
        .tv_sec  = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    zsock_setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[UDP_PACKET_SIZE];
    int n = zsock_recv(s_sock, buf, sizeof(buf), 0);
    if (n < 0) {
        if (errno != EAGAIN) LOG_WRN("recv: %d", errno);
        return -errno;
    }
    /* Reply envelope shape matches the request: imei_len|IMEI|nonce|ct|tag */
    if (n < 1) return -EBADMSG;
    int imei_len = buf[0];
    if (imei_len < 1 || imei_len > 20) return -EBADMSG;
    if (n < 1 + imei_len + 12 + 16) return -EBADMSG;

    const uint8_t *imei  = buf + 1;
    const uint8_t *nonce = buf + 1 + imei_len;
    int ct_len = n - 1 - imei_len - 12 - 16;
    if (ct_len < 0 || (size_t)ct_len >= out_len) return -EBADMSG;
    const uint8_t *ct  = nonce + 12;
    const uint8_t *tag = ct + ct_len;

    if (!cp_open(g_settings.psk, nonce, imei, imei_len,
                 ct, ct_len, tag, (uint8_t *)out_plaintext)) {
        LOG_WRN("recv: tag mismatch");
        return -EBADMSG;
    }
    out_plaintext[ct_len] = '\0';
    return ct_len;
}
