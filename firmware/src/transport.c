/*
 * Plain UDP transport with ChaCha20-Poly1305 AEAD encryption.
 *
 * Each send is a single UDP datagram — no handshake, no session state.  The
 * server answers every one, and its answer is the only evidence the datagram
 * arrived: a send that returns 0 means the modem queued it, nothing more.  A
 * modem that loses its registration discards whatever it had queued, and on
 * 2026-09-27 a tracking-area update the network rejected (EMM cause 9) took
 * three records at 10:57 and the ignition-off record at 11:50 that way, each
 * one "sent" as far as this side knew.  So every send gets an id, the socket
 * is kept between sends, and each reply is matched to the send it answers —
 * however late it comes — which is what lets databuf.c hold a datagram's
 * records until they are known to have arrived.  The socket is closed when
 * the unit goes to sleep, and dropped on errors.
 *
 * Keeping it costs the radio nothing.  The server replies whether or not
 * anything is listening, and it is the release hint (SO_RAI), not the socket,
 * that lets the modem leave connected mode.
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
#define IMEI_MAX     20

/* How many recent sends a reply can still be credited to.  More than one
 * telemetry cycle puts out (the record, two of backlog, an alert), so a reply
 * that turns up a cycle late — or several, behind a stall — still counts. */
#define SENT_TRACK   8

/* The server's address is looked up once per boot and kept: a lookup is a
 * round trip of its own through the modem, and on a marginal link it can take
 * tens of seconds.  But a unit can run for weeks between boots, and kept for
 * good, the address of a server that has moved would be sent to until the
 * next one — and the update notice that could have rebooted it rides on the
 * replies that no longer come.  So it is looked up again when the server
 * itself has gone quiet: RECHECK_SENDS sends in a row unanswered, each made
 * with the modem registered, the first of them RECHECK_AGE_MS ago.  That is
 * well past the longest stall seen on a registered link (38 s on a weak cell,
 * 2026-09-27), so a slow link mid-drive is not held up by a lookup, and sends
 * made with the registration gone do not count at all.  A lookup that finds
 * the same address, or fails, keeps the one in hand and doubles the count
 * before the next, to RECHECK_MAX. */
#define RECHECK_SENDS    3
#define RECHECK_MAX      48
#define RECHECK_AGE_MS   (5 * 60 * 1000)

static int s_sock = -1;
static struct sockaddr_in s_server;
static bool s_resolved;
static bool s_recheck;
static int  s_recheck_after = RECHECK_SENDS;
static int  s_silent_sends;      /* registered sends since the last reply */
static int64_t s_silent_since_ms;
/* Track mode: hold the RRC connection between sends. */
static bool s_streaming;

/* The server binds each reply to the nonce of the request it answers (the
 * response AAD is IMEI || request nonce), so a reply authenticates against
 * that send's nonce and no other.  Keeping the last few by id is therefore
 * all it takes to say which datagram a reply is for. */
static struct {
    uint32_t id;                  /* 0: empty, or already answered */
    uint8_t  nonce[NONCE_LEN];
} s_sent[SENT_TRACK];
static uint32_t s_last_id;       /* the latest send's, 0 before the first */

/* Static rather than on the stack: the main thread's 8 KB also carries the
 * record builder and the OBD poll, and a reply read happens inside a send. */
static uint8_t s_rx[UDP_PACKET_SIZE];
static char    s_reply[256];

#if CONFIG_APP_DEBUG_DROP_DATAGRAMS > 0
static unsigned s_drop_count;
#endif

void transport_set_streaming(bool on)
{
    s_streaming = on;
}

bool transport_is_streaming(void)
{
    return s_streaming;
}

uint32_t transport_sent_id(void)
{
    return s_last_id;
}

/* The id of the send the reply now in s_rx answers, with its plaintext in
 * s_reply, or 0 when it answers none of the sends still remembered. */
static uint32_t match_reply(int n)
{
    size_t imei_len = strlen(g_settings.imei);

    if (n < NONCE_LEN + TAG_LEN || imei_len == 0 || imei_len > IMEI_MAX) {
        LOG_DBG("unusable reply: %d bytes", n);
        return 0;
    }

    uint8_t aad[IMEI_MAX + NONCE_LEN];

    memcpy(aad, g_settings.imei, imei_len);

    /* Newest first: nearly every reply answers one of the last sends.  A
     * tag mismatch is the expected answer for every other one, and crypto.c
     * keeps it quiet. */
    for (int k = 0; k < SENT_TRACK; k++) {
        uint32_t id = s_last_id - (uint32_t)k;
        int slot = (int)(id % SENT_TRACK);
        size_t pt_len;

        if (id == 0 || s_sent[slot].id != id) {
            continue;
        }
        memcpy(aad + imei_len, s_sent[slot].nonce, NONCE_LEN);
        if (crypto_decrypt(s_rx + NONCE_LEN, (size_t)n - NONCE_LEN,
                           aad, imei_len + NONCE_LEN, s_rx,
                           (uint8_t *)s_reply, sizeof(s_reply) - 1,
                           &pt_len) == 0) {
            s_reply[pt_len] = '\0';
            s_sent[slot].id = 0;        /* a second copy would be a replay */
            s_silent_sends = 0;
            s_recheck_after = RECHECK_SENDS;
            return id;
        }
    }
    return 0;
}

int transport_poll(void)
{
    int matched = 0;

    if (s_sock < 0) {
        return -ENOTCONN;
    }
    /* Bounded, so a socket that keeps returning something unusable cannot
     * hold the loop; whatever is left is read on the next look. */
    for (int i = 0; i < 2 * SENT_TRACK; i++) {
        int n = zsock_recv(s_sock, s_rx, sizeof(s_rx), ZSOCK_MSG_DONTWAIT);

        if (n < 0) {
            if (errno != EAGAIN) {
                /* ENETDOWN after a re-attach: the PDN this socket belonged
                 * to is gone, and so is anything still on its way to it. */
                LOG_WRN("recv: %d", errno);
                transport_teardown();
            }
            break;
        }

        uint32_t id = match_reply(n);

        if (id != 0) {
            data_reply(id, s_reply);
            matched++;
        } else {
            LOG_DBG("reply to no recent send ignored (%d bytes)", n);
        }
    }
    return matched;
}

static int transport_resolve(void)
{
    struct zsock_addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_DGRAM,
    };
    struct zsock_addrinfo *res = NULL;
    struct sockaddr_in found;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", SERVER_PORT);

    /* The modem's resolver, and it can sit on a query for tens of seconds
     * when the link is marginal.  Nothing to slice, so bracket it with
     * kicks: the lookup gets a full watchdog window to itself. */
    watchdog_kick();
    int err = zsock_getaddrinfo(SERVER_HOST, port_str, &hints, &res);
    watchdog_kick();

    bool recheck = s_resolved;

    s_recheck = false;
    if (err) {
        if (recheck) {
            s_recheck_after = MIN(2 * s_recheck_after, RECHECK_MAX);
            LOG_WRN("getaddrinfo(%s): %d — keeping the address in hand",
                    SERVER_HOST, err);
            return 0;
        }
        LOG_ERR("getaddrinfo(%s): %d", SERVER_HOST, err);
        return -EIO;
    }
    memcpy(&found, res->ai_addr, sizeof(found));
    zsock_freeaddrinfo(res);

    if (recheck) {
        char was[NET_IPV4_ADDR_LEN], now[NET_IPV4_ADDR_LEN];

        zsock_inet_ntop(AF_INET, &s_server.sin_addr, was, sizeof(was));
        zsock_inet_ntop(AF_INET, &found.sin_addr, now, sizeof(now));
        if (found.sin_addr.s_addr == s_server.sin_addr.s_addr) {
            s_recheck_after = MIN(2 * s_recheck_after, RECHECK_MAX);
            LOG_WRN("%s is still %s — next look after %d unanswered sends",
                    SERVER_HOST, now, s_recheck_after);
        } else {
            s_recheck_after = RECHECK_SENDS;
            LOG_WRN("%s has moved: %s, was %s", SERVER_HOST, now, was);
        }
    }
    memcpy(&s_server, &found, sizeof(s_server));
    s_resolved = true;
    return 0;
}

int transport_open(void)
{
    if (s_sock >= 0) return 0;

    if (!s_resolved || s_recheck) {
        int err = transport_resolve();
        if (err) return err;
    }

    s_sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        LOG_ERR("socket: %d", errno);
        return -errno;
    }

    int err = zsock_connect(s_sock, (struct sockaddr *)&s_server,
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
    /* A last look first: a reply that has already landed is a datagram
     * whose records need not go again. */
    (void)transport_poll();

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
    /* Replies that came in since the last send: each is a datagram that
     * arrived.  It can also drop a socket that has gone bad. */
    (void)transport_poll();

    if (s_silent_sends >= s_recheck_after && modem_is_registered() &&
        k_uptime_get() - s_silent_since_ms >= RECHECK_AGE_MS) {
        LOG_WRN("%d sends unanswered over %lld s with the network up — "
                "looking %s up again", s_silent_sends,
                (k_uptime_get() - s_silent_since_ms) / 1000, SERVER_HOST);
        s_silent_sends = 0;
        s_recheck = true;
        transport_teardown();     /* the new socket connects to the answer */
    }

    if (s_sock < 0) {
        int err = transport_open();
        if (err) return err;
    }

    size_t imei_len = strlen(g_settings.imei);
    if (imei_len == 0 || imei_len > IMEI_MAX) {
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
    uint8_t nonce[NONCE_LEN];

    buf[0] = (uint8_t)imei_len;
    memcpy(buf + 1, g_settings.imei, imei_len);

    if (!crypto_random(nonce, NONCE_LEN)) {
        LOG_ERR("nonce generation failed");
        return -EIO;
    }
    memcpy(buf + 1 + imei_len, nonce, NONCE_LEN);

    size_t ct_len;
    int err = crypto_encrypt(plaintext, pt_len,
                             (const uint8_t *)g_settings.imei, imei_len,
                             nonce,
                             buf + hdr_len, UDP_PACKET_SIZE - hdr_len,
                             &ct_len);
    if (err) {
        LOG_ERR("encrypt: %d", err);
        return err;
    }

    size_t total = hdr_len + ct_len;

    /* Release-assistance hint.  The reply is always wanted now — it is the
     * receipt — so the radio may go once that one packet is in, and GNSS
     * gets the antenna back.  Streaming keeps the connection up for the
     * next send instead. */
    int rai = s_streaming ? RAI_ONGOING : RAI_ONE_RESP;
    zsock_setsockopt(s_sock, SOL_SOCKET, SO_RAI, &rai, sizeof(rai));

    bool dropped = false;

#if CONFIG_APP_DEBUG_DROP_DATAGRAMS > 0
    if (++s_drop_count % CONFIG_APP_DEBUG_DROP_DATAGRAMS == 0) {
        LOG_INF("bench: %u bytes discarded on purpose "
                "(APP_DEBUG_DROP_DATAGRAMS)", (unsigned)total);
        dropped = true;
    }
#endif

    if (!dropped && zsock_send(s_sock, buf, total, 0) < 0) {
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

    /* Filed under a fresh id with the nonce its reply will be bound to. */
    if (++s_last_id == 0) {
        s_last_id = 1;
    }
    int slot = (int)(s_last_id % SENT_TRACK);

    s_sent[slot].id = s_last_id;
    memcpy(s_sent[slot].nonce, nonce, NONCE_LEN);

    if (modem_is_registered() && s_silent_sends++ == 0) {
        s_silent_since_ms = k_uptime_get();
    }

    LOG_INF("sent %u bytes (#%u)", (unsigned)total, s_last_id);

    return 0;
}

int transport_recv_response(char *out_plaintext, size_t out_len, int timeout_ms)
{
    if (s_sock < 0) return -ENOTCONN;

    /* A reply to an earlier send can arrive first — one that was not
     * waited for, or a late one — and is credited to that send on the way:
     * the reply wanted may be right behind it.  Nothing is trusted that does
     * not authenticate against one of our own nonces; the wait only decides
     * how long to listen. */
    const uint32_t want = s_last_id;
    const int64_t deadline = k_uptime_get() + timeout_ms;

    for (;;) {
        int64_t left = deadline - k_uptime_get();

        if (left <= 0) {
            return -EAGAIN;
        }

        struct zsock_timeval tv = {
            .tv_sec  = left / 1000,
            .tv_usec = (left % 1000) * 1000,
        };
        zsock_setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        int n = zsock_recv(s_sock, s_rx, sizeof(s_rx), 0);

        if (n < 0) {
            int err = errno;

            /* Late is routine, and the socket stays for it: the reply can
             * still be credited when it does turn up. */
            if (err != EAGAIN) {
                LOG_WRN("recv: %d", err);
                transport_teardown();
            }
            return -err;
        }

        uint32_t id = match_reply(n);

        if (id != 0 && id == want) {
            databuf_ack(id);
            size_t len = strlen(s_reply);

            if (len >= out_len) {
                len = out_len - 1;
            }
            memcpy(out_plaintext, s_reply, len);
            out_plaintext[len] = '\0';
            return (int)len;
        }
        if (id != 0) {
            data_reply(id, s_reply);
        } else {
            LOG_DBG("reply to no recent send ignored (%d bytes)", n);
        }
    }
}
