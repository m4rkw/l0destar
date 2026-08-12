/*
 * Plain UDP transport with ChaCha20-Poly1305 AEAD encryption.
 *
 * Each send is a single UDP datagram — no handshake, no session state.
 * The socket is closed after each send/recv cycle so the LTE radio is
 * fully released for GNSS.
 *
 * Wire format (matches the server's _decrypt_request / _encrypt_response):
 *   request:  [1] imei_len  [imei_len] IMEI  [12] nonce  [N+16] ct+tag
 *   response: [12] nonce  [N+16] ct+tag
 *
 * AEAD parameters:
 *   request:  key=PSK, nonce=random, AAD=IMEI, plaintext=CSV data
 *   response: key=PSK, nonce=random, AAD=IMEI||req_nonce, plaintext=resp
 */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>

#include "app.h"

LOG_MODULE_REGISTER(transport, CONFIG_APP_LOG_LEVEL);

#define SERVER_HOST \
    (sizeof(CONFIG_APP_SERVER_HOST) > 1 ? CONFIG_APP_SERVER_HOST : HOSTNAME)
#define SERVER_PORT  UDP_PORT

#define NONCE_LEN    12
#define TAG_LEN      16

static int s_sock = -1;
static struct sockaddr_in s_server;
static uint8_t s_req_nonce[NONCE_LEN];

int transport_open(void)
{
    if (s_sock >= 0) return 0;

    struct zsock_addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_DGRAM,
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

    err = zsock_connect(s_sock, (struct sockaddr *)&s_server,
                        sizeof(s_server));
    if (err) {
        LOG_ERR("connect: %d", errno);
        zsock_close(s_sock);
        s_sock = -1;
        return -errno;
    }

    return 0;
}

void transport_close(void)
{
    if (s_sock >= 0) {
        int rai = RAI_NO_DATA;
        zsock_setsockopt(s_sock, SOL_SOCKET, SO_RAI, &rai, sizeof(rai));
        zsock_close(s_sock);
        s_sock = -1;
    }
}

void transport_teardown(void)
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

    size_t imei_len = strlen(g_settings.imei);
    if (imei_len == 0 || imei_len > 20) {
        LOG_WRN("IMEI not set, dropping packet");
        return -EACCES;
    }

    /* Envelope: [1 imei_len][IMEI][12 nonce][ct + 16 tag] */
    size_t hdr_len = 1 + imei_len + NONCE_LEN;
    size_t needed = hdr_len + pt_len + TAG_LEN;
    if (needed > UDP_PACKET_SIZE) {
        LOG_ERR("payload too large: %u", (unsigned)needed);
        return -EMSGSIZE;
    }

    uint8_t buf[UDP_PACKET_SIZE];

    buf[0] = (uint8_t)imei_len;
    memcpy(buf + 1, g_settings.imei, imei_len);

    if (!crypto_random(s_req_nonce, NONCE_LEN)) {
        LOG_ERR("nonce generation failed");
        return -EIO;
    }
    memcpy(buf + 1 + imei_len, s_req_nonce, NONCE_LEN);

    size_t ct_len;
    int err = crypto_encrypt(plaintext, pt_len,
                             (const uint8_t *)g_settings.imei, imei_len,
                             s_req_nonce,
                             buf + hdr_len, UDP_PACKET_SIZE - hdr_len,
                             &ct_len);
    if (err) {
        LOG_ERR("encrypt: %d", err);
        return err;
    }

    size_t total = hdr_len + ct_len;

    int rai = read_udp_response ? RAI_ONE_RESP : RAI_LAST;
    zsock_setsockopt(s_sock, SOL_SOCKET, SO_RAI, &rai, sizeof(rai));

    if (zsock_send(s_sock, buf, total, 0) < 0) {
        LOG_WRN("send failed (%d), reconnecting", errno);
        transport_teardown();
        err = transport_open();
        if (err) return err;
        zsock_setsockopt(s_sock, SOL_SOCKET, SO_RAI, &rai, sizeof(rai));
        if (zsock_send(s_sock, buf, total, 0) < 0) {
            LOG_ERR("send retry failed: %d", errno);
            transport_teardown();
            return -errno;
        }
    }

    LOG_INF("sent %u bytes", (unsigned)total);

    if (!read_udp_response) {
        transport_close();
    }

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
        transport_close();
        return -errno;
    }

    transport_close();

    if (n < NONCE_LEN + TAG_LEN) {
        LOG_WRN("response too short: %d", n);
        return -EPROTO;
    }

    /* Response: [12 nonce][ct + 16 tag], AAD = IMEI || request_nonce */
    size_t imei_len = strlen(g_settings.imei);
    uint8_t aad[20 + NONCE_LEN];
    memcpy(aad, g_settings.imei, imei_len);
    memcpy(aad + imei_len, s_req_nonce, NONCE_LEN);

    size_t pt_len;
    int err = crypto_decrypt(buf + NONCE_LEN, n - NONCE_LEN,
                             aad, imei_len + NONCE_LEN,
                             buf,
                             (uint8_t *)out_plaintext, out_len - 1, &pt_len);
    if (err) {
        LOG_WRN("response decrypt failed: %d", err);
        return -EPROTO;
    }

    out_plaintext[pt_len] = '\0';
    return (int)pt_len;
}
