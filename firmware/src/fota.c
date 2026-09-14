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
#define FOTA_NBIOT_DEFERRALS  CONFIG_APP_FOTA_NBIOT_DEFERRALS
#define FOTA_RESCAN_TIMEOUT_S CONFIG_APP_FOTA_RESCAN_TIMEOUT_S

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
static uint32_t s_announced_ver;  /* version we've already said we're fetching */
static int     s_nbiot_defers;    /* consecutive checks deferred off NB-IoT */
static uint32_t s_denied_ver;     /* version the server or we refused */
static bool    s_denied_logged;
static char    s_pending_report[64]; /* "F,fota,..." waiting for a link */

/* What we last staged, kept across the reboot that applies it.  __noinit so
 * it survives a warm reset; a power-on leaves RAM undefined, which is what
 * the magic is for, and losing it there is the right answer anyway — a unit
 * that has been off long enough to lose RAM deserves a fresh attempt.
 *
 * This is what lets the device notice its own failed update.  A staged
 * version that is not the one running after the reboot means MCUboot
 * reverted it: the image booted but never confirmed itself.  Without this
 * the device has no memory of having tried, so it downloads the same broken
 * image on every wake — which with the engine off is a battery flattened by
 * a 300 KB download an hour. */
#define FOTA_ATTEMPT_MAGIC  0x10DEF07Au
#define FOTA_MAX_ATTEMPTS   2

static __noinit struct {
    uint32_t magic;
    uint32_t version;     /* packed version staged before the reboot */
    uint8_t  attempts;    /* consecutive attempts at that version */
} s_attempt;

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
static int download_image(int64_t deadline)
{
    if (k_uptime_get() >= deadline) {
        return -ETIMEDOUT;
    }

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
     * LTE-M, so wait in short slices and keep the watchdog fed, until the
     * deadline the caller shares across every attempt of this check. */

    while (k_sem_take(&s_dl_done, K_SECONDS(5)) != 0) {
        watchdog_kick();
        if (k_uptime_get() >= deadline) {
            LOG_ERR("download timed out (%ds budget)",
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

/* The bare `fota` command is the manual retry: it clears everything this
 * device has decided about a bad version, so an operator who has fixed the
 * image (or wants to try again anyway) can override both the local block
 * and the failure holdoff from the server. */
void fota_request_check(void)
{
    if (IS_ENABLED(CONFIG_APP_FOTA_INHIBIT)) {
        LOG_WRN("ignoring `fota` command — updates inhibited");
        return;
    }
    if (s_denied_ver) {
        LOG_INF("manual retry — clearing the block on the failed version");
    }
    s_denied_ver = 0;
    s_denied_logged = false;
    s_attempt.magic = 0;
    s_attempt.version = 0;
    s_attempt.attempts = 0;
    s_fail_count = 0;
    s_next_check_ms = 0;

    /* Manual `fota` command: check now, even inside a failure holdoff. */
    s_forced = true;
    s_next_check_ms = 0;
}

void fota_notify_available(const char *ver)
{
    static uint32_t s_last_logged;
    uint32_t avail;

    if (IS_ENABLED(CONFIG_APP_FOTA_INHIBIT)) {
        return;   /* server advertises what it likes; we are not listening */
    }

    if (version_parse(ver, &avail) != 0) {
        LOG_WRN("fota indication '%s' malformed", ver);
        return;
    }
    if (avail <= VER_RUNNING) {
        return;   /* steady state: server advertises what we already run */
    }
    if (avail == s_denied_ver) {
        /* Tried and reverted, or refused by the server.  Not acted on and
         * not asked about again: the advert rides on every response, so a
         * device that kept reacting to it would fetch the manifest on every
         * wake for as long as the server kept saying it. */
        if (!s_denied_logged) {
            s_denied_logged = true;
            LOG_WRN("ignoring %s — it has already failed here", ver);
        }
        return;
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

/* True while MCUboot is still waiting to be told this image works: it was
 * swapped in as BOOT_UPGRADE_TEST and any reset before fota_confirm_image()
 * runs takes it back out again. */
bool fota_image_on_probation(void)
{
    return !boot_is_img_confirmed();
}

/* Format a packed version back into "a.b.c" for the wire and the log. */
static const char *ver_str_of(uint32_t v, char *buf, size_t len)
{
    snprintf(buf, len, "%u.%u.%u", (unsigned)(v >> 16) & 0xff,
             (unsigned)(v >> 8) & 0xff, (unsigned)v & 0xff);
    return buf;
}

/* Did the update we staged before the last reboot actually take?
 *
 * Called at boot, before the confirm, and it has to run on both outcomes:
 * the image that booted and the image MCUboot put back when it didn't.
 * Reporting the failure is the whole point — the server cannot tell a
 * revert from a device that never downloaded until the device says so, and
 * "F,fota,failed,<staged>,<running>" is that statement. */
void fota_verdict_on_boot(void)
{
    if (s_attempt.magic != FOTA_ATTEMPT_MAGIC || s_attempt.version == 0) {
        s_attempt.magic = 0;      /* power-on, or nothing was staged */
        return;
    }

    char staged[16];

    ver_str_of(s_attempt.version, staged, sizeof(staged));

    if (s_attempt.version == VER_RUNNING) {
        LOG_INF("update to %s took", staged);
        s_attempt.magic = 0;
        s_attempt.version = 0;
        s_attempt.attempts = 0;
        return;
    }

    /* Staged one version, running another: MCUboot reverted it. */
    LOG_ERR("update to %s reverted — running %s (attempt %u of %u)",
            staged, APP_VERSION_STRING, s_attempt.attempts,
            FOTA_MAX_ATTEMPTS);

    char line[64];

    snprintf(line, sizeof(line), "F,fota,failed,%s,%s", staged,
             APP_VERSION_STRING);
    /* Queued as an alert too: the line goes out as its own datagram at the
     * next send, the alert is what a person sees. */
    char msg[80];
    snprintf(msg, sizeof(msg), "fota: %s failed to boot, reverted to %s",
             staged, APP_VERSION_STRING);
    alert_enqueue(msg, 0);
    s_pending_report[0] = '\0';
    strncpy(s_pending_report, line, sizeof(s_pending_report) - 1);

    if (s_attempt.attempts >= FOTA_MAX_ATTEMPTS) {
        s_denied_ver = s_attempt.version;
        s_denied_logged = false;
        LOG_ERR("%s has failed %u times — not trying it again until a newer "
                "version or a manual retry", staged, s_attempt.attempts);
    }
}

/* Send the verdict once there is a link.  Its own datagram, like the DTC
 * report, and dropped only if the send fails — the server's own timeout on
 * the staged version covers a device that never manages to report. */
void fota_report_flush(void)
{
    if (s_pending_report[0] == '\0') {
        return;
    }
    if (data_send_line(s_pending_report) == 0) {
        s_pending_report[0] = '\0';
    }
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

    /* Console only.  The "updated to" notification is the server's to
     * raise, from the version the next record reports against the version
     * it staged: this runs on exactly one boot — every later one returns
     * at boot_is_img_confirmed() above — and an alert queued here has to
     * survive both a working link and no reset before the next send.  When
     * it did not, nothing ever raised it again, so a missing notification
     * meant nothing.  See fw_check_running() in the server. */
    LOG_INF("running image confirmed (%s)", APP_VERSION_STRING);
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

    if (IS_ENABLED(CONFIG_APP_FOTA_INHIBIT)) {
        /* Bench build: the running image is the thing under test, so nothing
         * is allowed to replace it.  Logged once so it is obvious from the
         * console why a unit never updates. */
        static bool said;

        if (!said) {
            said = true;
            LOG_WRN("updates inhibited (APP_FOTA_INHIBIT) — running %s",
                    APP_VERSION_STRING);
        }
        return 0;
    }

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

    /* "status=blocked" is the server withholding this image from this
     * device — normally because it watched an earlier copy of it fail to
     * boot here.  Not an error and not retried: remember the version so
     * the advert that rides on every response stops meaning anything, and
     * wait for a newer one or for a manual `fota`. */
    char status[16];

    if (manifest_value(s_manifest, "status", status,
                       sizeof(status)) == 0 &&
        strcmp(status, "blocked") == 0) {
        if (s_denied_ver != available || !s_denied_logged) {
            char why[48] = "";

            (void)manifest_value(s_manifest, "reason", why, sizeof(why));
            LOG_WRN("server is withholding %s%s%s", ver_str,
                    why[0] ? " — " : "", why);
            s_denied_logged = true;
        }
        s_denied_ver = available;
        s_fail_count = 0;
        s_next_check_ms = 0;
        return 0;
    }

    if (available == s_denied_ver) {
        LOG_INF("%s is blocked here after a failed update — skipping",
                ver_str);
        return 0;
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

#if FOTA_NBIOT_DEFERRALS > 0
    /* NB-IoT carries telemetry perfectly well and a ~275 KB image very badly:
     * a few tens of kbps at best, and a round trip per 2 KB range over a link
     * whose latency is measured in seconds.  Attempting anyway spends minutes
     * of radio time to arrive back where it started.
     *
     * Deferring outright would be worse — a unit that only ever sees NB-IoT
     * would never update at all.  So try to get off it first, and if the unit
     * really is somewhere NB-IoT is all there is, give in and attempt anyway
     * after a few checks rather than stalling forever. */
    if (!modem_is_nbiot()) {
        s_nbiot_defers = 0;
    } else {
        LOG_WRN("on NB-IoT — re-scanning for LTE-M before downloading");
        (void)modem_rescan_plmn(FOTA_RESCAN_TIMEOUT_S);

        if (modem_is_nbiot()) {
            s_nbiot_defers++;
            if (s_nbiot_defers <= FOTA_NBIOT_DEFERRALS) {
                LOG_WRN("still NB-IoT — deferring download (%d/%d)",
                        s_nbiot_defers, FOTA_NBIOT_DEFERRALS);
                fail_backoff();
                return -EAGAIN;
            }
            LOG_WRN("still NB-IoT after %d deferrals — attempting anyway",
                    FOTA_NBIOT_DEFERRALS);
        }
    }
#endif

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

    /* Say so before the transfer starts, not only after it lands.  A
     * multi-minute download followed by a reboot is otherwise a silent gap
     * in telemetry, and one that never completes leaves only the failure
     * alert below.  Sent now rather than queued: the queue's next outing is
     * the reboot alert.  Once per advertised version, like the failure
     * alert — the check re-runs every wake while the server keeps
     * advertising, and "downloading" every hour would be noise.  The socket
     * the send brings up is closed again so the download's own connection
     * has the modem to itself. */
    if (s_announced_ver != available) {
        s_announced_ver = available;
        char msg[64];
        snprintf(msg, sizeof(msg), "fota: %s -> %s available, downloading",
                 APP_VERSION_STRING, ver_str);
        alert_enqueue(msg, 0);
        alert_send_standalone();
        transport_close();
    }

    /* One dropped connection should not cost the whole wake.  The image comes
     * down as ~140 sequential ranged GETs on a single TLS connection with no
     * resume, so any stall long enough to trip the server's read timeout ends
     * the transfer and the next attempt restarts at byte 0.  Retrying inside
     * this wake is nearly free — the radio is already up, GNSS is already
     * stopped, the manifest is already fetched — and turns a transient radio
     * stall into a short delay instead of a wait for the next wake. */
    int err = -EIO;

    /* APP_FOTA_DOWNLOAD_TIMEOUT_S bounds the whole retry sequence: every
     * attempt shares one deadline, so adding attempts cannot multiply how long
     * the unit stays awake with GNSS stopped.  A fast failure — the
     * interesting case, a dropped connection — leaves nearly the whole budget
     * for another go, while an attempt that grinds through the budget ends
     * the check. */
    int64_t budget_end =
        k_uptime_get() + (int64_t)CONFIG_APP_FOTA_DOWNLOAD_TIMEOUT_S * 1000;

    int attempts = 0;
    for (int attempt = 1; attempt <= FOTA_DL_ATTEMPTS; attempt++) {
        attempts = attempt;
        err = download_image(budget_end);
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

        /* Escalate before the last attempt.  Retrying on the same cell covers
         * a transient stall; it does nothing for the case actually seen in the
         * field, where a stationary unit camped on a marginal cell failed
         * three checks in a row and then downloaded in 97 s as soon as
         * reselection put it somewhere else. */
        if (IS_ENABLED(CONFIG_APP_FOTA_RESCAN_ON_RETRY) &&
            attempt == FOTA_DL_ATTEMPTS - 1) {
            (void)modem_rescan_plmn(FOTA_RESCAN_TIMEOUT_S);
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
        /* 10 min, doubling to 80, like every other failed check; the `fota`
         * command overrides it.  Without it the advert in the next reply
         * re-armed the check, and a failing download ran again every reply,
         * each time with GNSS stopped. */
        fail_backoff();
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
                     APP_VERSION_STRING, ver_str, attempts,
                     err, s_dl_cause);
            alert_enqueue(msg, 0);
        }
        return err;
    }

    s_alerted_ver = 0;
    s_nbiot_defers = 0;

    s_fail_count = 0;
    LOG_INF("image staged — rebooting into %s", ver_str);

    /* Remember what we staged, so the boot after this one can tell whether
     * it took: same version running means success, anything else means
     * MCUboot reverted it.  Consecutive attempts at the same version are
     * counted; a different version starts again from one. */
    if (s_attempt.magic != FOTA_ATTEMPT_MAGIC ||
        s_attempt.version != available) {
        s_attempt.attempts = 0;
    }
    s_attempt.magic = FOTA_ATTEMPT_MAGIC;
    s_attempt.version = available;
    s_attempt.attempts++;

    /* Tell the server before the radio goes down, twice over: the line is
     * for the server's state machine (it now knows this device holds a
     * staged image and can withhold the next one until it sees the version
     * running), the alert is for a person.  The matching "updated to" alert
     * is raised by fota_confirm_image() after the new image boots. */
    char line[64];
    snprintf(line, sizeof(line), "F,fota,staged,%s,%s", ver_str,
             APP_VERSION_STRING);
    data_send_line(line);

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
bool fota_image_on_probation(void) { return false; }
void fota_verdict_on_boot(void) { }
void fota_report_flush(void)    { }
int  fota_check(enum fota_ctx ctx) { ARG_UNUSED(ctx); return 0; }

#endif /* CONFIG_APP_FOTA */
