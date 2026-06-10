/*
 * DTLS transport over UDP, offloaded to the nRF modem.
 *
 * The modem handles the DTLS handshake, encryption, and certificate
 * verification.  The application sends/receives plaintext.
 *
 * Wire format (single DTLS datagram per send):
 *   request:  [2] payload_len (big-endian)  [N] plaintext
 *   response: plaintext (single datagram)
 *
 * The IMEI is sent as the first line of the payload so the server can
 * identify the device.
 *
 * After each exchange, RAI_NO_DATA hints the network to release the
 * radio so GNSS can use it.  The socket is closed immediately so the
 * modem can complete the DTLS close_notify while the server session is
 * still alive.  Session caching lets the modem abbreviate subsequent
 * handshakes.
 */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <modem/modem_key_mgmt.h>

#include "app.h"

LOG_MODULE_REGISTER(transport, CONFIG_APP_LOG_LEVEL);

#define SERVER_HOST \
    (sizeof(CONFIG_APP_SERVER_HOST) > 1 ? CONFIG_APP_SERVER_HOST : HOSTNAME)
#define SERVER_PORT DTLS_PORT

static int s_sock = -1;
static struct sockaddr_in s_server;

int transport_open(void)
{
    if (s_sock >= 0) return 0;

    struct zsock_addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_DGRAM,
        .ai_protocol = IPPROTO_DTLS_1_2,
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

    s_sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_DTLS_1_2);
    if (s_sock < 0) {
        LOG_ERR("socket: %d", errno);
        return -errno;
    }

    sec_tag_t sec_tags[] = { TLS_SEC_TAG };
    zsock_setsockopt(s_sock, SOL_TLS, TLS_SEC_TAG_LIST,
                     sec_tags, sizeof(sec_tags));

    int verify = TLS_PEER_VERIFY_REQUIRED;
    zsock_setsockopt(s_sock, SOL_TLS, TLS_PEER_VERIFY,
                     &verify, sizeof(verify));

    zsock_setsockopt(s_sock, SOL_TLS, TLS_HOSTNAME,
                     SERVER_HOST, strlen(SERVER_HOST));

    int cid = TLS_DTLS_CID_ENABLED;
    zsock_setsockopt(s_sock, SOL_TLS, TLS_DTLS_CID,
                     &cid, sizeof(cid));

    int cache = TLS_SESSION_CACHE_ENABLED;
    zsock_setsockopt(s_sock, SOL_TLS, TLS_SESSION_CACHE,
                     &cache, sizeof(cache));

    err = zsock_connect(s_sock, (struct sockaddr *)&s_server,
                        sizeof(s_server));
    if (err) {
        LOG_ERR("connect: %d", errno);
        zsock_close(s_sock);
        s_sock = -1;
        return -errno;
    }

    LOG_INF("DTLS connected");
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

    size_t payload_len = imei_len + 1 + pt_len;
    if (payload_len > UINT16_MAX || payload_len + 2 > UDP_PACKET_SIZE) {
        LOG_ERR("payload too large: %u", (unsigned)payload_len);
        return -EMSGSIZE;
    }

    /* Single datagram: [2-byte len][IMEI\n][csv_data] */
    uint8_t buf[UDP_PACKET_SIZE];
    buf[0] = (uint8_t)(payload_len >> 8);
    buf[1] = (uint8_t)(payload_len & 0xFF);
    memcpy(buf + 2, g_settings.imei, imei_len);
    buf[2 + imei_len] = '\n';
    memcpy(buf + 2 + imei_len + 1, plaintext, pt_len);
    size_t total = 2 + payload_len;

    int rai = read_udp_response ? RAI_ONE_RESP : RAI_LAST;
    zsock_setsockopt(s_sock, SOL_SOCKET, SO_RAI, &rai, sizeof(rai));

    if (zsock_send(s_sock, buf, total, 0) < 0) {
        LOG_ERR("send: %d", errno);
        transport_teardown();
        return -errno;
    }

    LOG_INF("sent %u bytes", (unsigned)total);
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

    int n = zsock_recv(s_sock, out_plaintext, out_len - 1, 0);
    if (n < 0) {
        if (errno != EAGAIN) LOG_WRN("recv: %d", errno);
        return -errno;
    }
    out_plaintext[n] = '\0';

    int rai = RAI_NO_DATA;
    zsock_setsockopt(s_sock, SOL_SOCKET, SO_RAI, &rai, sizeof(rai));

    return n;
}
