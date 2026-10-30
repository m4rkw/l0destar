/*
 * u-blox AssistNow for the external receiver.
 *
 * A receiver that has sat unpowered for hours holds no usable ephemerides and
 * must read them from the satellites: 25 to 30 s with a clear sky, minutes
 * with a poor one.  AssistNow Predictive Orbits (free from u-blox) tells it
 * where the satellites will be for the coming days, so it need only hear a few
 * of them.
 *
 * This does not talk to u-blox.  The l0destar server does: it registers each
 * receiver with the service, downloads the orbits once a day, keeps them, and
 * serves this unit the part it can use (server/tracker/assistnow.py).  Between
 * them a reboot or a firmware update loses nothing, u-blox's request limit per
 * device is never what runs out, and the credential for the service stays on
 * the server and out of the firmware image.
 *
 * The receiver driver (gnss_ext.c) asks for assistance when the receiver comes
 * up with fewer than a handful of ephemerides and no fix.  A thread of its own
 * then:
 *
 *   1. waits for the network and for the receiver's chip id and version
 *      replies (UBX-SEC-UNIQID, UBX-MON-VER), which the driver has asked for;
 *   2. GETs /assist/data.bin from the update server, over the same TLS as the
 *      firmware manifest.  A 404 means the server has not met this receiver:
 *      it is registered with GET /assist/register, which carries those two
 *      replies, and the data is asked for again;
 *   3. hands the receiver the time from the modem's clock, then the data as
 *      the server sent it (UBX-MGA messages and nothing else gets through,
 *      gnss_ext_assist_inject), and logs how many it took.
 *
 * It runs on a thread so that a slow name lookup or a weak link, which can
 * hold a request for its whole timeout, cannot hold the main loop past the
 * watchdog.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/tls_credentials.h>
#include <date_time.h>
#include <net/rest_client.h>

#include "app.h"
#include "assistnow.h"
#include "gnss_ubx.h"

LOG_MODULE_REGISTER(assistnow, CONFIG_APP_LOG_LEVEL);

/* The same endpoint as the firmware manifest: the update server's TLS listener
 * and the CA at the same sec_tag. */
#define ASSIST_HOST                                                            \
    (sizeof(CONFIG_APP_FOTA_HOST) > 1                                          \
         ? CONFIG_APP_FOTA_HOST                                                \
         : (sizeof(CONFIG_APP_SERVER_HOST) > 1 ? CONFIG_APP_SERVER_HOST        \
                                               : HOSTNAME))
#define ASSIST_SEC_TAG      CONFIG_APP_FOTA_SEC_TAG
#define ASSIST_USE_TLS      (ASSIST_SEC_TAG >= 0)

#define STACK_SIZE          3072
#define THREAD_PRIO         K_PRIO_PREEMPT(10)
#define NETWORK_WAIT_MS     90000       /* for LTE, before the attempt is given up */
#define IDS_WAIT_MS         10000       /* for the receiver's chip id and version replies */
#define CLOCK_WAIT_MS       30000       /* for the modem's clock */
#define RETRY_PAUSE_MS      20000       /* between the two attempts */
#define HEADERS_ROOM        512         /* of the buffer, for the response's headers */

static K_SEM_DEFINE(s_req, 0, 1);

static char s_buf[CONFIG_APP_ASSISTNOW_BUF_SIZE];
/* Request line; the registration one carries the version reply as hex, about
 * 480 characters (the buffer takes the longest the driver keeps, 400 bytes). */
static char s_url[1024];
static char s_status[72] = "idle";

const char *assistnow_status(void)
{
    return s_status;
}

static void status(const char *fmt, ...) __printf_like(1, 2);

static void status(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(s_status, sizeof(s_status), fmt, ap);
    va_end(ap);
}

void assistnow_request(void)
{
    k_sem_give(&s_req);
}

/* One GET to the update server.  0 with *code set if the server answered at
 * all, whatever it said; the body, if any, is in s_buf at *body. */
static int http_get(const char *url, unsigned *code, const uint8_t **body, size_t *blen)
{
    struct rest_client_req_context req = {0};
    struct rest_client_resp_context resp = {0};

    rest_client_request_defaults_set(&req);
    req.http_method   = HTTP_GET;
    req.host          = ASSIST_HOST;
    req.port          = CONFIG_APP_FOTA_PORT;
    req.url           = url;
    req.sec_tag       = ASSIST_USE_TLS ? ASSIST_SEC_TAG : SEC_TAG_TLS_INVALID;
    req.timeout_ms    = CONFIG_APP_ASSISTNOW_TIMEOUT_S * MSEC_PER_SEC;
    req.resp_buff     = s_buf;
    req.resp_buff_len = sizeof(s_buf);

    int err = rest_client_request(&req, &resp);

    if (err) {
        return err;
    }
    *code = resp.http_status_code;
    *body = (const uint8_t *)resp.response;
    *blen = resp.response ? resp.response_len : 0;
    return 0;
}

/* Tell the server this receiver exists.  Its answer is the HTTP status:
 * 200 registered; 403 the server will not (this IMEI is registered with
 * another receiver, or assistance is off for it); 502/503 the service itself
 * would not. */
static int register_unit(const char *imei)
{
    char uid[48];
    int err = -EAGAIN;
    size_t p = (size_t)snprintf(s_url, sizeof(s_url), "/assist/register?imei=%s&ver=", imei);

    for (int64_t until = k_uptime_get() + IDS_WAIT_MS; k_uptime_get() < until; k_msleep(500)) {
        /* The version reply goes straight into the request line. */
        err = gnss_ext_ids(uid, sizeof(uid), s_url + p, sizeof(s_url) - p - 80);
        if (err != -EAGAIN || !gnss_ext_assist_useful()) {
            break;
        }
    }
    if (err) {
        LOG_WRN("receiver ids not available: %d", err);
        return err == -EAGAIN ? -ETIMEDOUT : err;
    }
    p += strlen(s_url + p);
    snprintf(s_url + p, sizeof(s_url) - p, "&uid=%s", uid);

    unsigned code;
    const uint8_t *body;
    size_t blen;

    status("registering the receiver");
    err = http_get(s_url, &code, &body, &blen);
    if (err) {
        LOG_WRN("registration request failed: %d", err);
        return err;
    }
    if (code == 200) {
        LOG_INF("receiver registered with the server");
        return 0;
    }
    LOG_WRN("registration refused: HTTP %u", code);
    return code == 403 ? -EACCES : -EIO;
}

/* The modem's clock as a UBX time message.  The clock comes from the network
 * a few seconds after registration, so it is waited for. */
static int time_frame(uint8_t *f, size_t cap)
{
    int64_t ms = 0;
    int err = -ENODATA;

    for (int64_t until = k_uptime_get() + CLOCK_WAIT_MS; k_uptime_get() < until; k_msleep(500)) {
        err = date_time_now(&ms);
        if (err == 0 || !gnss_ext_assist_useful()) {
            break;
        }
    }
    if (err) {
        return 0;
    }

    struct ubx_utc t;

    if (!ubx_utc_from_unix_ms(ms, &t)) {
        return 0;
    }
    t.tacc_s = CONFIG_APP_ASSISTNOW_TIME_ACC_S;
    t.tacc_ns = 0;
    return (int)ubx_build_mga_ini_time_utc(f, cap, &t);
}

/* The time, then what the server sent, into the receiver. */
static int deliver(const uint8_t *data, size_t len)
{
    struct gnss_assist_result rt = {0}, rb = {0};
    uint8_t f[40];
    int n = time_frame(f, sizeof(f));
    int err = 0;

    if (!gnss_ext_assist_useful()) {
        return -ECANCELED;              /* a fix, or the rail dropped, while it was fetched */
    }
    status("giving the receiver %u bytes", (unsigned)len);
    if (n > 0) {
        err = gnss_ext_assist_inject(f, (size_t)n, &rt);
    } else {
        LOG_WRN("no valid clock: the receiver is given the orbits without the time");
    }
    if (!err) {
        err = gnss_ext_assist_inject(data, len, &rb);
    }
    gnss_ext_assist_done();

    LOG_INF("assistance given: time %s, %u of %u messages taken, %u refused%s",
            rt.accepted ? "taken" : (n > 0 ? "not acknowledged" : "not sent"),
            rb.accepted, rb.sent, rb.refused,
            rb.first_info == 1 ? " (it did not know the time)" : "");
    status("given: %u of %u taken", rb.accepted, rb.sent);
    return err;
}

/* One try: ask the server for the data, registering first if it does not
 * know this receiver. */
static int fetch_and_inject(void)
{
    if (!g_settings.imei[0]) {
        return -ENODATA;
    }

    for (int round = 0; round < 2; round++) {
        unsigned code;
        const uint8_t *body;
        size_t blen;

        snprintf(s_url, sizeof(s_url), "/assist/data.bin?imei=%s&gnss=%s&max=%u",
                 g_settings.imei, CONFIG_APP_ASSISTNOW_GNSS,
                 (unsigned)(sizeof(s_buf) - HEADERS_ROOM));
        status("asking the server for orbits");

        int err = http_get(s_url, &code, &body, &blen);

        if (err) {
            LOG_WRN("request failed: %d", err);
            return err;
        }
        switch (code) {
        case 200:
            if (blen == 0) {
                return -EAGAIN;
            }
            return deliver(body, blen);
        case 404:                       /* not registered */
            if (round > 0) {
                return -ENOENT;
            }
            err = register_unit(g_settings.imei);
            if (err) {
                return err;
            }
            break;                      /* and ask again */
        case 204:                       /* registered; the server has none yet */
            LOG_INF("the server has no orbits for this receiver yet");
            return -EAGAIN;
        default:
            LOG_WRN("data request: HTTP %u", code);
            return code == 501 ? -ENOTSUP : -EIO;
        }
    }
    return -EIO;
}

static int attempt(void)
{
    /* LTE first.  It is normally up by the time the receiver has been
     * configured, but a unit that woke on movement may still be attaching. */
    for (int64_t until = k_uptime_get() + NETWORK_WAIT_MS;
         !modem_is_registered() || modem_lte_is_cut(); k_msleep(1000)) {
        if (!gnss_ext_assist_useful() || k_uptime_get() > until) {
            return -ENETDOWN;
        }
        status("waiting for the network");
    }

    for (int tries = 0; tries < 2; tries++) {
        if (!gnss_ext_assist_useful()) {
            return -ECANCELED;
        }

        int err = fetch_and_inject();

        if (err == 0 || err == -ECANCELED || err == -EACCES || err == -ENOTSUP) {
            return err;                 /* done, pointless, or refused for good */
        }
        LOG_INF("assistance attempt %d failed: %d", tries + 1, err);
        if (tries == 0) {
            status("retrying after %d", err);
            k_msleep(RETRY_PAUSE_MS);
        } else {
            return err;
        }
    }
    return -EIO;
}

static void worker(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    for (;;) {
        k_sem_take(&s_req, K_FOREVER);

        int err = attempt();

        if (err) {
            /* Not retried this power-up: the receiver finds its own way, as it
             * does without any of this, and the next power-up asks again. */
            status("not given: %d", err);
            if (err != -ECANCELED) {
                LOG_WRN("no assistance this power-up: %d", err);
            }
            gnss_ext_assist_done();
        }
    }
}

K_THREAD_DEFINE(assistnow_thread, STACK_SIZE, worker, NULL, NULL, NULL, THREAD_PRIO, 0, 0);
