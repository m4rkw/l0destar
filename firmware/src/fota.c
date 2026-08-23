/*
 * Over-the-air application update.
 *
 * The tracker pulls; the server never pushes.  To keep telemetry wakes free
 * of speculative HTTP traffic, the trigger rides on the exchange the device
 * is already making: every telemetry response carries `fota=<latest>`
 * (appended by _process_telemetry in the server), the device compares that
 * against its own build, and only when the server's version is newer does it
 * GET the manifest and image.  The steady state costs zero extra requests.
 *
 * fota_check() therefore only acts when a check is pending:
 *
 *   1. power-on — the one unconditional check, so a freshly flashed or
 *      long-offline unit converges without waiting for a telemetry response
 *   2. the server response advertised a newer version (fota=X.Y.Z -> 
 *      fota_notify_available)
 *   3. the server sent a bare `fota` command (manual force, skips holdoff)
 *
 * The call sites in main.c (STATE_IDLE and the do_sleep() telemetry wake)
 * run right after response processing, so an indicated update starts within
 * the same wake.  Failed attempts set a holdoff
 * (CONFIG_APP_FOTA_RETRY_HOLDOFF_S, doubling to 8x) so a broken image or
 * endpoint doesn't burn a download attempt on every send while the server
 * keeps advertising it.
 *
 * Manifest — text/plain, one key=value per line, '#' comments and unknown
 * keys ignored:
 *
 *     version=0.5.0
 *     file=fw/l0destar-0.5.0.bin
 *
 * `file` is the MCUboot-signed image (build/firmware/zephyr/zephyr.signed.bin)
 * served from the same host.  The manifest URL carries ?imei=&v= so a dynamic
 * endpoint can stage rollouts per device; a static file server ignores it.
 *
 * The download streams straight into MCUboot's secondary slot and is marked
 * BOOT_UPGRADE_TEST, so a firmware that boots but never confirms itself is
 * rolled back automatically (see fota_confirm_image()).  Nothing touches
 * flash until the battery gate passes — an update can't flatten a weak
 * vehicle battery, and a brownout mid-download only leaves an unusable
 * secondary slot, never a bricked primary.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/app_version.h>   /* generated from the VERSION file */

#include "app.h"

LOG_MODULE_REGISTER(fota, CONFIG_APP_LOG_LEVEL);

/* Composite board identity, published in the manifest as `board=`.  Images
 * built from one source version still differ by carrier board and by which
 * OBD/AIO interfaces are populated — that combination, not the version, is
 * what makes an image wrong for a given unit.  push_fw.sh composes the same
 * string from the build's .config (see Kconfig.boards:APP_BOARD_ID). */
#if IS_ENABLED(CONFIG_APP_BOARD_HAS_CAN)
#define BOARD_ID_CAN   "+can"
#else
#define BOARD_ID_CAN   ""
#endif
#if IS_ENABLED(CONFIG_APP_BOARD_HAS_KLINE)
#define BOARD_ID_KLINE "+kline"
#else
#define BOARD_ID_KLINE ""
#endif
#if IS_ENABLED(CONFIG_APP_BOARD_HAS_AIO)
#define BOARD_ID_AIO   "+aio"
#else
#define BOARD_ID_AIO   ""
#endif
#define FOTA_BOARD_ID                                                          \
    CONFIG_APP_BOARD_ID BOARD_ID_CAN BOARD_ID_KLINE BOARD_ID_AIO

const char *fota_version(void)
{
    return APP_VERSION_STRING;
}

const char *fota_board_id(void)
{
    return FOTA_BOARD_ID;
}

#if IS_ENABLED(CONFIG_APP_FOTA)

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/dfu/mcuboot.h>
#include <zephyr/net/tls_credentials.h>

#include <modem/lte_lc.h>
#include <net/fota_download.h>
#include <net/rest_client.h>

/* Endpoint: CONFIG_APP_FOTA_HOST if set, else the telemetry host. */
#define FOTA_HOST                                                              \
    (sizeof(CONFIG_APP_FOTA_HOST) > 1                                          \
         ? CONFIG_APP_FOTA_HOST                                                \
         : (sizeof(CONFIG_APP_SERVER_HOST) > 1 ? CONFIG_APP_SERVER_HOST        \
                                               : HOSTNAME))

#define FOTA_SEC_TAG        CONFIG_APP_FOTA_SEC_TAG
#define FOTA_USE_TLS        (FOTA_SEC_TAG >= 0)
#define FOTA_MIN_BATTERY_V  (CONFIG_APP_FOTA_MIN_BATTERY_MV / 1000.0f)
#define FOTA_DL_ATTEMPTS      CONFIG_APP_FOTA_DOWNLOAD_ATTEMPTS
#define FOTA_DL_RETRY_DELAY_S CONFIG_APP_FOTA_RETRY_DELAY_S

/* Version fields are one byte each in the MCUboot image header, and the
 * VERSION file is validated against the same range at build time. */
#define VER_PACK(maj, min, pat)                                                \
    (((uint32_t)(maj) << 16) | ((uint32_t)(min) << 8) | (uint32_t)(pat))
#define VER_RUNNING VER_PACK(APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_PATCHLEVEL)

/* fota_download() keeps the pointers it is handed rather than copying, so the
 * host and file strings must outlive the whole download. */
static char s_dl_host[128];
static char s_dl_file[160];
static char s_url[192];
static char s_manifest[512];

static int64_t s_next_check_ms;   /* failure holdoff: no checks before this */
static int     s_fail_count;
static bool    s_forced;
static bool    s_boot_check = true;   /* the one unconditional power-on check */
static bool    s_dl_init_done;
static uint32_t s_alerted_ver;    /* version we've already reported stuck */

/* -- download completion --------------------------------------------------- */
static K_SEM_DEFINE(s_dl_done, 0, 1);
static bool s_dl_ok;
static int  s_dl_cause;

static void dl_handler(const struct fota_download_evt *evt)
{
    switch (evt->id) {
    case FOTA_DOWNLOAD_EVT_FINISHED:
        s_dl_ok = true;
        k_sem_give(&s_dl_done);
        break;
    case FOTA_DOWNLOAD_EVT_ERROR:
        s_dl_cause = (int)evt->cause;
        s_dl_ok = false;
        k_sem_give(&s_dl_done);
        break;
    case FOTA_DOWNLOAD_EVT_CANCELLED:
        s_dl_cause = -1;
        s_dl_ok = false;
        k_sem_give(&s_dl_done);
        break;
    default:
        break;
    }
}

/* -- manifest -------------------------------------------------------------- */

/* Copy the value of `key` from a key=value line into out.  Lines are matched
 * from their first non-blank character, so an indented or commented-out line
 * can't be mistaken for the real thing. */
static int manifest_value(const char *body, const char *key,
                          char *out, size_t out_len)
{
    size_t key_len = strlen(key);
    const char *p = body;

    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        const char *line_end = eol ? eol : p + strlen(p);

        while (p < line_end && (*p == ' ' || *p == '\t')) p++;

        if ((size_t)(line_end - p) > key_len &&
            strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *v = p + key_len + 1;
            const char *v_end = line_end;
            while (v_end > v && (v_end[-1] == '\r' || v_end[-1] == ' ' ||
                                 v_end[-1] == '\t')) {
                v_end--;
            }
            size_t len = (size_t)(v_end - v);
            if (len == 0 || len >= out_len) return -EINVAL;
            memcpy(out, v, len);
            out[len] = '\0';
            return 0;
        }

        if (!eol) break;
        p = eol + 1;
    }
    return -ENOENT;
}

static int version_parse(const char *s, uint32_t *out)
{
    unsigned int maj, min, pat;

    if (sscanf(s, "%u.%u.%u", &maj, &min, &pat) != 3) return -EINVAL;
    if (maj > 255 || min > 255 || pat > 255)          return -EINVAL;

    *out = VER_PACK(maj, min, pat);
    return 0;
}

static int manifest_fetch(void)
{
    struct rest_client_req_context req = {0};
    struct rest_client_resp_context resp = {0};

    /* IMEI and running version let a dynamic endpoint stage rollouts; a
     * static file server just ignores the query string. */
    snprintf(s_url, sizeof(s_url), "%s?imei=%s&v=%s",
             CONFIG_APP_FOTA_MANIFEST_PATH,
             g_settings.imei[0] ? g_settings.imei : "unknown",
             APP_VERSION_STRING);

    rest_client_request_defaults_set(&req);
    req.http_method   = HTTP_GET;
    req.host          = FOTA_HOST;
    req.port          = CONFIG_APP_FOTA_PORT;
    req.url           = s_url;
    req.sec_tag       = FOTA_USE_TLS ? FOTA_SEC_TAG : SEC_TAG_TLS_INVALID;
    req.timeout_ms    = CONFIG_APP_FOTA_MANIFEST_TIMEOUT_S * MSEC_PER_SEC;
    req.resp_buff     = s_manifest;
    req.resp_buff_len = sizeof(s_manifest);

    int err = rest_client_request(&req, &resp);
    if (err) {
        LOG_WRN("manifest request failed: %d", err);
        return err;
    }
    if (resp.http_status_code != REST_CLIENT_HTTP_STATUS_OK) {
        LOG_WRN("manifest HTTP %u", resp.http_status_code);
        return -EIO;
    }
    if (resp.response == NULL || resp.response_len == 0) {
        LOG_WRN("manifest empty");
        return -EPROTO;
    }

    /* rest_client points `response` into resp_buff; NUL-terminate the body so
     * the parser can treat it as a string. */
    size_t len = MIN(resp.response_len,
                     sizeof(s_manifest) -
                         (size_t)(resp.response - s_manifest) - 1);
    memmove(s_manifest, resp.response, len);
    s_manifest[len] = '\0';
    return 0;
}

/* -- battery gate ---------------------------------------------------------- */
static bool battery_permits_update(void)
{
    float v = battery_read_voltage();

    /* Below IMPLAUSIBLE_VOLTAGE means no sensor rather than a flat battery
     * (same convention as the low-battery alert in data.c). */
    if (v < IMPLAUSIBLE_VOLTAGE) {
        if (IS_ENABLED(CONFIG_APP_FOTA_REQUIRE_BATTERY_READING)) {
            LOG_WRN("no battery reading (%.2fV) — update blocked", (double)v);
            return false;
        }
        LOG_WRN("no battery reading (%.2fV) — updating anyway", (double)v);
        return true;
    }

    if (v < FOTA_MIN_BATTERY_V) {
        LOG_INF("battery %.2fV < %.2fV — deferring update",
                (double)v, (double)FOTA_MIN_BATTERY_V);
        return false;
    }

    battery_v = v;
    return true;
}

/* -- download -------------------------------------------------------------- */
static int download_image(void)
{
    int err;

    if (!s_dl_init_done) {
        err = fota_download_init(dl_handler);
        if (err) {
            LOG_ERR("fota_download_init: %d", err);
            return err;
        }
        s_dl_init_done = true;
    }

    s_dl_ok = false;
    s_dl_cause = 0;
    k_sem_reset(&s_dl_done);

    err = fota_download_start(s_dl_host, s_dl_file,
                              FOTA_USE_TLS ? FOTA_SEC_TAG : SEC_TAG_TLS_INVALID,
                              0, CONFIG_APP_FOTA_FRAGMENT_SIZE);
    if (err) {
        LOG_ERR("fota_download_start: %d", err);
        return err;
    }

    /* The download runs on the downloader thread and can take minutes over
     * LTE-M, so wait in short slices and keep the watchdog fed. */
    int64_t deadline =
        k_uptime_get() + (int64_t)CONFIG_APP_FOTA_DOWNLOAD_TIMEOUT_S * 1000;

    while (k_sem_take(&s_dl_done, K_SECONDS(5)) != 0) {
        watchdog_kick();
        if (k_uptime_get() >= deadline) {
            LOG_ERR("download timed out after %ds",
                    CONFIG_APP_FOTA_DOWNLOAD_TIMEOUT_S);
            (void)fota_download_cancel();
            (void)k_sem_take(&s_dl_done, K_SECONDS(30));
            watchdog_kick();
            return -ETIMEDOUT;
        }
    }

    if (!s_dl_ok) {
        LOG_ERR("download failed (cause %d)", s_dl_cause);
        return -EIO;
    }
    return 0;
}

/* -- public API ------------------------------------------------------------ */

void fota_request_check(void)
{
    /* Manual `fota` command: check now, even inside a failure holdoff. */
    s_forced = true;
    s_next_check_ms = 0;
}

void fota_notify_available(const char *ver)
{
    static uint32_t s_last_logged;
    uint32_t avail;

    if (version_parse(ver, &avail) != 0) {
        LOG_WRN("fota indication '%s' malformed", ver);
        return;
    }
    if (avail <= VER_RUNNING) {
        return;   /* steady state: server advertises what we already run */
    }
    if (avail != s_last_logged) {
        s_last_logged = avail;
        LOG_INF("server advertises %s (running %s)", ver, APP_VERSION_STRING);
    }
    s_forced = true;
}

bool fota_check_requested(void)
{
    return s_forced;
}

void fota_confirm_image(void)
{
    /* A swapped-in image boots as BOOT_UPGRADE_TEST: unless it confirms
     * itself, MCUboot reverts to the previous one on the next boot.  Called
     * from main() once the whole init sequence has completed, so an image
     * that hangs or faults during bring-up is rolled back instead of being
     * made permanent. */
    if (boot_is_img_confirmed()) {
        return;
    }

    int err = boot_write_img_confirmed();
    if (err) {
        LOG_ERR("image confirm failed: %d — MCUboot will revert", err);
        return;
    }

    LOG_INF("running image confirmed (%s)", APP_VERSION_STRING);
    char msg[48];
    snprintf(msg, sizeof(msg), "fota: updated to %s", APP_VERSION_STRING);
    alert_enqueue(msg, 0);
}

/* Failed attempts push the next one out (doubling to 8x) so a broken image or
 * endpoint doesn't cost a download attempt on every telemetry send while the
 * server keeps advertising the same version. */
static void fail_backoff(void)
{
    int64_t holdoff = (int64_t)CONFIG_APP_FOTA_RETRY_HOLDOFF_S * 1000;

    s_next_check_ms = k_uptime_get() + (holdoff << MIN(s_fail_count, 3));
    s_fail_count++;
}

int fota_check(enum fota_ctx ctx)
{
    bool gnss_stopped = false;

    if (!s_forced && !s_boot_check) {
        return 0;   /* nothing pending — the common case, no traffic */
    }
    if (s_next_check_ms != 0 && k_uptime_get() < s_next_check_ms) {
        return 0;   /* failure holdoff (fota_request_check overrides it) */
    }

    int reg = modem_get_network_status();
    if (reg != 1 && reg != 5) {
        /* Leave the pending flags set — the check should survive a dead
         * link and run on the next opportunity. */
        LOG_DBG("no network — skipping update check");
        return -ENETDOWN;
    }

    s_forced = false;
    s_boot_check = false;

    LOG_INF("checking for updates (running %s)", APP_VERSION_STRING);
    watchdog_kick();

    if (manifest_fetch() != 0) {
        fail_backoff();
        return -EIO;
    }

    char ver_str[16];
    if (manifest_value(s_manifest, "version", ver_str, sizeof(ver_str)) != 0) {
        LOG_WRN("manifest has no version=");
        fail_backoff();
        return -EPROTO;
    }

    uint32_t available;
    if (version_parse(ver_str, &available) != 0) {
        LOG_WRN("manifest version '%s' malformed", ver_str);
        fail_backoff();
        return -EPROTO;
    }

    /* Strictly newer only.  Equal is the steady state; older would loop
     * forever against a build whose VERSION file was never bumped.  To roll
     * a fleet back, publish the old image under a higher version. */
    if (available <= VER_RUNNING) {
        LOG_INF("up to date (server %s, running %s)", ver_str,
                APP_VERSION_STRING);
        s_fail_count = 0;
        return 0;
    }

    if (manifest_value(s_manifest, "file", s_dl_file, sizeof(s_dl_file)) != 0) {
        LOG_WRN("manifest has no file=");
        fail_backoff();
        return -EPROTO;
    }

    /* The server picks the manifest from the ?imei= in the request, so a
     * manifest naming a different board means that mapping is wrong or
     * missing (device not in remote.conf, fallen back to a bench build).
     * That is precisely the case where installing would leave a unit running
     * firmware for hardware it doesn't have — refuse rather than brick it.
     * A manifest with no board= line predates the check and is accepted. */
    char board[32];
    switch (manifest_value(s_manifest, "board", board, sizeof(board))) {
    case 0:
        if (strcmp(board, FOTA_BOARD_ID) != 0) {
            LOG_ERR("manifest targets board '%s', this unit is '%s' — refusing",
                    board, FOTA_BOARD_ID);
            fail_backoff();
            return -EPROTO;
        }
        break;
    case -ENOENT:
        LOG_WRN("manifest has no board= — installing unverified (this is '%s')",
                FOTA_BOARD_ID);
        break;
    default:
        LOG_WRN("manifest board= malformed");
        fail_backoff();
        return -EPROTO;
    }

    if (!battery_permits_update()) {
        /* Not a failure — retry on the normal cadence, no back-off. */
        return -EAGAIN;
    }

    LOG_INF("updating %s -> %s (%s)", APP_VERSION_STRING, ver_str, s_dl_file);
    snprintf(s_dl_host, sizeof(s_dl_host), "%s://%s:%d",
             FOTA_USE_TLS ? "https" : "http", FOTA_HOST,
             CONFIG_APP_FOTA_PORT);

    /* GNSS shares the antenna path with LTE; leaving it running would stretch
     * a multi-minute download further. */
    if (ctx == FOTA_CTX_AWAKE) {
        gnss_stop();
        gnss_stopped = true;
    }
    transport_close();
    led_sending();

    /* One dropped connection should not cost the whole wake.  The image comes
     * down as ~140 sequential ranged GETs on a single TLS connection with no
     * resume, so any stall long enough to trip the server's read timeout ends
     * the transfer and the next attempt restarts at byte 0.  Retrying inside
     * this wake is nearly free — the radio is already up, GNSS is already
     * stopped, the manifest is already fetched — and turns a transient radio
     * stall into a short delay instead of a wait for the next wake. */
    int err = -EIO;

    /* APP_FOTA_DOWNLOAD_TIMEOUT_S bounds a single attempt; it also bounds the
     * retry sequence, so adding attempts cannot multiply how long the unit
     * stays awake with GNSS stopped.  A fast failure — the interesting case,
     * a dropped connection — leaves nearly the whole budget for another go,
     * while an attempt that grinds through the budget is not repeated. */
    int64_t budget_end =
        k_uptime_get() + (int64_t)CONFIG_APP_FOTA_DOWNLOAD_TIMEOUT_S * 1000;

    for (int attempt = 1; attempt <= FOTA_DL_ATTEMPTS; attempt++) {
        err = download_image();
        if (err == 0) {
            break;
        }
        LOG_WRN("download attempt %d/%d failed: %d (cause %d)",
                attempt, FOTA_DL_ATTEMPTS, err, s_dl_cause);

        if (k_uptime_get() >= budget_end) {
            LOG_WRN("download budget (%ds) spent — leaving the rest to the "
                    "next check", CONFIG_APP_FOTA_DOWNLOAD_TIMEOUT_S);
            break;
        }

        if (attempt < FOTA_DL_ATTEMPTS) {
            /* Let the link settle before going again, feeding the watchdog
             * across the wait rather than sleeping through it in one block. */
            for (int waited = 0; waited < FOTA_DL_RETRY_DELAY_S; waited++) {
                k_sleep(K_SECONDS(1));
                watchdog_kick();
            }
        }
    }

    if (err) {
        s_fail_count++;
        led_idle();
        if (gnss_stopped) gnss_resume();

        /* Every attempt this wake failed, so this is not a one-off stall.
         * Report it once per advertised version — the check re-runs on every
         * wake while the server keeps advertising, and an alert an hour saying
         * the same thing is noise.  s_alerted_ver resets when the server
         * moves to a different version, so a genuinely new stuck update is
         * still reported.  Queued, not sent standalone: the radio has just
         * proved unreliable, and the next telemetry send carries it. */
        if (s_alerted_ver != available) {
            s_alerted_ver = available;
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "fota: %s -> %s failed after %d attempts (err %d, cause %d)",
                     APP_VERSION_STRING, ver_str, FOTA_DL_ATTEMPTS,
                     err, s_dl_cause);
            alert_enqueue(msg, 0);
        }
        return err;
    }

    s_alerted_ver = 0;

    s_fail_count = 0;
    LOG_INF("image staged — rebooting into %s", ver_str);

    /* Tell the server before the radio goes down; the matching "updated to"
     * alert is raised by fota_confirm_image() after the new image boots. */
    char msg[64];
    snprintf(msg, sizeof(msg), "fota: %s -> %s, rebooting",
             APP_VERSION_STRING, ver_str);
    alert_enqueue(msg, 0);
    alert_send_standalone();

    led_all_off();
    lte_lc_power_off();
    network_ready = false;
    k_msleep(200);
    reboot_now();

    return 1;   /* not reached */
}

#else  /* !CONFIG_APP_FOTA */

void fota_request_check(void)   { }
void fota_notify_available(const char *ver) { ARG_UNUSED(ver); }
bool fota_check_requested(void) { return false; }
void fota_confirm_image(void)   { }
int  fota_check(enum fota_ctx ctx) { ARG_UNUSED(ctx); return 0; }

#endif /* CONFIG_APP_FOTA */
