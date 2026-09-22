/*
 * Traccar transport: telemetry as OsmAnd-protocol HTTP requests.
 *
 * Built in place of transport.c when CONFIG_APP_TRACCAR is set and
 * implements the same transport_* interface, so the rest of the firmware —
 * the record builder, batching, the backlog, the alert queue — is unchanged.
 * It still hands over newline-separated lines in the l0destar server's
 * format, and this module turns each into one request:
 *
 *   record    GET /?id=<imei>&timestamp=..&lat=..&lon=..&speed=..&...
 *   A,p,msg   GET /?id=<imei>&alarm=<type>&alert=<msg>&priority=<p>
 *             (no position: Traccar attaches the device's last one)
 *   D,codes   GET /?id=<imei>&dtcs=<codes>
 *   L,.. F,.. dropped — captured log lines and the update verdict are for
 *             the l0destar server's device log
 *
 * Field names follow Traccar's own attribute keys where one exists (power,
 * ignition, sat, versionFw, rpm, coolantTemp ...) and the record's key
 * otherwise, so everything a record carries ends up on the position.  The
 * mapping is in TRACCAR.md.
 *
 * One TCP connection is kept across sends (HTTP/1.1 keep-alive) rather
 * than opened and closed around each.  With release assistance the modem
 * drops the RRC connection straight after each response, and a TCP close
 * after that would need a fresh one just to carry the FIN.  So
 * transport_close() only hints the release and keeps the socket, and
 * transport_teardown() — before the modem is powered off, and on any
 * error — is what actually closes it.  A request on a connection the
 * server has meanwhile dropped fails and is retried once on a new one.
 *
 * Traccar answers every request, so a 2xx is proof of delivery, which the
 * UDP transport only gets from a reply.  transport_recv_response() reports
 * it in the l0destar server's response shape — "1,-1,-1", an ack carrying
 * no settings, so data.c needs no second parser — followed by the data of
 * any command Traccar had queued for the device (its "custom" command),
 * which comes back in the response body.  The firmware's own command
 * vocabulary applies: int=, movealarm=, locate, fota, reboot ...
 *
 * A 4xx answer is a configuration problem (400: Traccar does not know the
 * id), not a transport failure: the request is dropped with an error
 * logged, rather than held in the backlog and retried forever.
 */

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/sys/timeutil.h>
#include <zephyr/app_version.h>

#include "app.h"

LOG_MODULE_REGISTER(traccar, CONFIG_APP_LOG_LEVEL);

BUILD_ASSERT(sizeof(CONFIG_APP_TRACCAR_HOST) > 1,
             "CONFIG_APP_TRACCAR_HOST must name the Traccar server");

#define TRACCAR_HOST     CONFIG_APP_TRACCAR_HOST
#define TRACCAR_PORT     CONFIG_APP_TRACCAR_PORT
#define TRACCAR_SEC_TAG  CONFIG_APP_TRACCAR_SEC_TAG
#define TRACCAR_USE_TLS  (TRACCAR_SEC_TAG >= 0)
/* Traccar's device identifier: the IMEI unless the build names another. */
#define TRACCAR_ID \
    (sizeof(CONFIG_APP_TRACCAR_ID) > 1 ? CONFIG_APP_TRACCAR_ID : g_settings.imei)

/* Connect (DNS excluded) and per-request bounds.  Both inside the watchdog
 * window, which is fed around every blocking step anyway. */
#define CONNECT_TIMEOUT_MS  15000
#define REQUEST_TIMEOUT_MS  10000

/* Netty, which Traccar's HTTP protocols sit on, refuses a request line over
 * 4096 bytes.  A record with every OBD field is ~700 as a query string; the
 * IMU burst is left out (see s_dropped). */
#define QUERY_MAX   1536
#define REQ_MAX     (QUERY_MAX + 128)   /* + request line and headers */
#define CMD_MAX     256                 /* commands taken from response bodies */
#define LINE_MAX    UDP_PACKET_SIZE

static int s_sock = -1;
static struct sockaddr_in s_server;
static bool s_resolved;
/* Track mode: hold the RRC connection between sends. */
static bool s_streaming;
/* Set when a batch got through; cleared when data.c reads the ack. */
static bool s_acked;
/* The server asked for the connection to be closed after this response. */
static bool s_close_after;
/* Commands from response bodies, waiting for data.c to read them. */
static char s_cmd[CMD_MAX];

/* Static rather than on the stack: the main thread's 8 KB has the record
 * builder and the OBD poll to carry as well. */
static char    s_line[LINE_MAX];
static char    s_query[QUERY_MAX];
static char    s_req[REQ_MAX];
static char    s_body[CMD_MAX];
static uint8_t s_rx[512];

void transport_set_streaming(bool on)
{
    s_streaming = on;
}

/* -- query string ---------------------------------------------------------- */

struct query {
    char   *buf;
    size_t  cap;
    size_t  len;
    bool    full;     /* a parameter did not fit and was left out */
};

static bool q_raw(struct query *q, const char *s)
{
    size_t n = strlen(s);

    if (q->len + n >= q->cap) return false;
    memcpy(q->buf + q->len, s, n);
    q->len += n;
    q->buf[q->len] = '\0';
    return true;
}

/* RFC 3986 unreserved characters as they are, everything else as %XX. */
static bool q_enc(struct query *q, const char *s, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";

    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        bool plain = isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';

        if (q->len + (plain ? 1 : 3) >= q->cap) return false;
        if (plain) {
            q->buf[q->len++] = (char)c;
        } else {
            q->buf[q->len++] = '%';
            q->buf[q->len++] = hex[c >> 4];
            q->buf[q->len++] = hex[c & 15];
        }
    }
    q->buf[q->len] = '\0';
    return true;
}

/* Append &key=value.  A parameter that does not fit is left out whole rather
 * than cut short, and the query is flagged. */
static void q_param(struct query *q, const char *key, const char *val,
                    size_t val_len)
{
    size_t mark = q->len;

    if (!q_raw(q, "&") || !q_raw(q, key) || !q_raw(q, "=") ||
        !q_enc(q, val, val_len)) {
        q->len = mark;
        q->buf[mark] = '\0';
        q->full = true;
    }
}

static void q_str(struct query *q, const char *key, const char *val)
{
    q_param(q, key, val, strlen(val));
}

static void q_fmt(struct query *q, const char *key, const char *fmt, ...)
{
    char tmp[32];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) {
        q_param(q, key, tmp, MIN((size_t)n, sizeof(tmp) - 1));
    }
}

/* -- line to query --------------------------------------------------------- */

enum line_kind { LINE_RECORD, LINE_ALERT, LINE_DTC, LINE_SKIP };

static enum line_kind line_kind(const char *p, size_t len)
{
    if (len >= 2 && p[1] == ',') {
        switch (p[0]) {
        case 'A': return LINE_ALERT;
        case 'D': return LINE_DTC;
        default:  return LINE_SKIP;     /* L, F */
        }
    }
    return isdigit((unsigned char)p[0]) ? LINE_RECORD : LINE_SKIP;
}

/* Split in place on `sep`, at most `max` pieces: the last keeps whatever
 * is left, separators and all.  Returns the count. */
static int split(char *s, char sep, char **out, int max)
{
    int n = 0;

    while (s && n < max) {
        out[n++] = s;
        if (n == max) break;
        s = strchr(s, sep);
        if (s) *s++ = '\0';
    }
    return n;
}

/* "DD/MM/YY" and "HH:MM:SS.uuuuuu+00" to Unix time.  0 for a record built
 * before the clock was set, which data.c stamps 01/01/00: Traccar then
 * takes the time of receipt, as the l0destar server does. */
static int64_t record_time(const char *date, const char *clock)
{
    struct tm tm = {0};

    if (sscanf(date, "%2d/%2d/%2d", &tm.tm_mday, &tm.tm_mon, &tm.tm_year) != 3 ||
        sscanf(clock, "%2d:%2d:%2d", &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 3 ||
        tm.tm_year == 0) {
        return 0;
    }
    tm.tm_year += 100;      /* two digits of 20xx, as years since 1900 */
    tm.tm_mon  -= 1;
    return timeutil_timegm64(&tm);
}

/* Extras with a Traccar attribute of their own, and the scale the record
 * carries them at (the l0destar server unscales them too).  Anything not
 * listed here or in s_dropped goes out under its own key. */
static const struct {
    const char *csv;
    const char *key;
    int         div;
} s_extras[] = {
    { "fw",    "versionFw",     1   },
    { "mt",    "deviceTemp",    1   },
    { "it",    "imuTemp",       1   },
    { "vs",    "battery",       1   },   /* the SiP's own supply, not the car's */
    { "rst",   "resetCause",    1   },
    { "tm",    "trackMode",     1   },
    { "rid",   "recordId",      1   },
    { "orpm",  "rpm",           1   },
    { "ormin", "rpmMin",        1   },
    { "ormax", "rpmMax",        1   },
    { "oravg", "rpmAvg",        1   },
    { "ospd",  "obdSpeed",      1   },
    { "ocl",   "coolantTemp",   1   },
    { "oit",   "intakeTemp",    1   },
    { "old",   "engineLoad",    10  },
    { "oth",   "throttle",      10  },
    { "omaf",  "maf",           100 },
    { "otim",  "timingAdvance", 10  },
    { "ostft", "shortFuelTrim", 10  },
    { "oltft", "longFuelTrim",  10  },
    { "ofs",   "fuelStatus",    1   },
    { "omil",  "mil",           1   },
    { "odtc",  "dtcCount",      1   },
};

/* Extras that mean nothing to Traccar: the settings sync and its request
 * flag, the duplicate of the uptime field, the track-mode IMU burst (a
 * kilobyte of raw samples, past the request-line limit and no use as an
 * attribute) and the compound debug counters. */
static const char *const s_dropped[] = { "int", "ma", "ri", "up", "acc", "dbg" };

static void encode_extra(struct query *q, const char *key, const char *val,
                         bool *stale, const char *cell[4])
{
    static const char *const cell_keys[4] = { "mcc", "mnc", "lac", "cid" };

    for (int i = 0; i < 4; i++) {
        if (strcmp(key, cell_keys[i]) == 0) {
            cell[i] = val;
            return;
        }
    }
    if (strcmp(key, "cl") == 0) {
        *stale = atoi(val) != 0;
        return;
    }
    /* wt=<rid>:<ms> is the duration of the wake that sent record <rid>, an
     * earlier one.  Traccar has no way to amend a position it has already
     * stored, so the pair goes out as two attributes of the record carrying
     * them and the reader joins them on recordId; filing the duration as an
     * attribute of this record would say it took that long to send this one,
     * which is the one thing it does not mean. */
    if (strcmp(key, "wt") == 0) {
        const char *colon = strchr(val, ':');

        if (colon && colon != val && colon[1] != '\0') {
            char rec[12];
            size_t len = MIN((size_t)(colon - val), sizeof(rec) - 1);

            memcpy(rec, val, len);
            rec[len] = '\0';
            q_str(q, "wakeRecordId", rec);
            q_str(q, "wakeMs", colon + 1);
        }
        return;
    }
    for (size_t i = 0; i < ARRAY_SIZE(s_dropped); i++) {
        if (strcmp(key, s_dropped[i]) == 0) return;
    }
    for (size_t i = 0; i < ARRAY_SIZE(s_extras); i++) {
        if (strcmp(key, s_extras[i].csv) != 0) continue;
        if (s_extras[i].div == 1) {
            q_str(q, s_extras[i].key, val);
        } else {
            long v = atol(val);
            long a = labs(v);

            if (s_extras[i].div == 10) {
                q_fmt(q, s_extras[i].key, "%s%ld.%ld",
                      v < 0 ? "-" : "", a / 10, a % 10);
            } else {
                q_fmt(q, s_extras[i].key, "%s%ld.%02ld",
                      v < 0 ? "-" : "", a / 100, a % 100);
            }
        }
        return;
    }
    q_str(q, key, val);
}

/* ts,lat,lon,spd,alt,hdg,hdop,sat,bat,ign,up,pon[,extras] — thirteen
 * tokens, the timestamp having a comma of its own.  Modifies the line. */
static int encode_record(struct query *q, char *line)
{
    char *f[14];
    int n = split(line, ',', f, ARRAY_SIZE(f));

    if (n < 13) {
        return -EINVAL;
    }

    int64_t ts = record_time(f[0], f[1]);

    if (ts > 0) q_fmt(q, "timestamp", "%lld", (long long)ts);

    /* A position of exactly 0,0 is a track-mode record on a unit that has
     * never had a fix.  Left out, Traccar attaches the last one it holds. */
    if (strtod(f[2], NULL) != 0.0 || strtod(f[3], NULL) != 0.0) {
        q_str(q, "lat", f[2]);
        q_str(q, "lon", f[3]);
    }
    q_fmt(q, "speed", "%.2f", strtod(f[4], NULL) / 1.852);   /* km/h -> knots */
    q_str(q, "altitude", f[5]);
    q_str(q, "bearing", f[6]);

    long hdop = atol(f[7]);
    if (hdop > 0) q_fmt(q, "hdop", "%ld.%ld", hdop / 10, hdop % 10);
    if (atol(f[8]) > 0) q_str(q, "sat", f[8]);

    /* Traccar's "power" is the supply the tracker is wired to, which is
     * this record's battery: the vehicle's. */
    q_str(q, "power", f[9]);
    q_str(q, "ignition", atoi(f[10]) ? "true" : "false");
    q_str(q, "uptime", f[11]);
    /* f[12], the powered-on flag: resetCause carries the same news. */

    bool stale = false;
    const char *cell[4] = { NULL, NULL, NULL, NULL };

    if (n > 13) {
        char *groups[24];
        int ng = split(f[13], ',', groups, ARRAY_SIZE(groups));

        for (int g = 0; g < ng; g++) {
            char *pairs[20];
            int np = split(groups[g], ';', pairs, ARRAY_SIZE(pairs));

            for (int i = 0; i < np; i++) {
                char *eq = strchr(pairs[i], '=');

                if (!eq) continue;
                *eq = '\0';
                encode_extra(q, pairs[i], eq + 1, &stale, cell);
            }
        }
    }
    if (cell[0] && cell[1] && cell[2] && cell[3]) {
        char c[48];

        snprintf(c, sizeof(c), "%s,%s,%s,%s", cell[0], cell[1], cell[2], cell[3]);
        q_str(q, "cell", c);
    }
    /* cl=1: built from the stored position because there was no fix. */
    q_str(q, "valid", stale ? "false" : "true");
    return 0;
}

/* Traccar's alarm for an alert, from how the firmware words it.  Anything
 * urgent it does not recognise is a general alarm; the informational
 * replies (locate, config, fota progress) are plain attributes. */
static const char *alarm_for(const char *msg, int priority)
{
    static const struct {
        const char *prefix;
        const char *alarm;
    } map[] = {
        { "movement",           "movement"      },
        { "impact",             "accident"      },   /* driving, past the crash threshold */
        { "parked impact",      "vibration"     },
        { "tilt",               "tow"           },
        { "tamper",             "tampering"     },
        { "low battery",        "lowPower"      },
        { "backup power",       "powerCut"      },
        { "car power restored", "powerRestored" },
        { "SELFTEST",           "fault"         },
        { "RAIL",               "fault"         },
        /* OBD threshold alerts lead with the field name (obd_alert.c). */
        { "coolant",            "temperature"   },
        { "intake",             "temperature"   },
    };

    for (size_t i = 0; i < ARRAY_SIZE(map); i++) {
        if (strncmp(msg, map[i].prefix, strlen(map[i].prefix)) == 0) {
            return map[i].alarm;
        }
    }
    return priority > 0 ? "general" : NULL;
}

/* "<priority>,<message>" — the message keeps its commas. */
static void encode_alert(struct query *q, char *rest)
{
    char *f[2];

    if (split(rest, ',', f, ARRAY_SIZE(f)) < 2) {
        q_str(q, "alert", rest);
        return;
    }

    const char *alarm = alarm_for(f[1], atoi(f[0]));

    if (alarm) q_str(q, "alarm", alarm);
    q_str(q, "alert", f[1]);
    q_str(q, "priority", f[0]);
}

/* -- connection ------------------------------------------------------------ */

static int resolve(void)
{
    struct zsock_addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct zsock_addrinfo *res = NULL;
    char port_str[8];

    snprintf(port_str, sizeof(port_str), "%d", TRACCAR_PORT);

    /* The modem's resolver, which can sit on a query for tens of seconds
     * when the link is marginal: a full watchdog window to itself. */
    watchdog_kick();
    int err = zsock_getaddrinfo(TRACCAR_HOST, port_str, &hints, &res);
    watchdog_kick();
    if (err) {
        LOG_ERR("getaddrinfo(%s): %d", TRACCAR_HOST, err);
        return -EIO;
    }
    memcpy(&s_server, res->ai_addr, sizeof(s_server));
    zsock_freeaddrinfo(res);
    s_resolved = true;
    return 0;
}

static int tls_setup(int sock)
{
    int verify = TLS_PEER_VERIFY_REQUIRED;
    const sec_tag_t tags[] = { TRACCAR_SEC_TAG };
    /* Same server every time: a resumed session skips the full handshake
     * when the connection has to be made again. */
    uint8_t cache = TLS_SESSION_CACHE_ENABLED;

    if (zsock_setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY, &verify, sizeof(verify)) ||
        zsock_setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST, tags, sizeof(tags)) ||
        zsock_setsockopt(sock, SOL_TLS, TLS_SESSION_CACHE, &cache, sizeof(cache)) ||
        zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME, TRACCAR_HOST,
                         strlen(TRACCAR_HOST))) {
        LOG_ERR("TLS setup: %d", errno);
        return -errno;
    }
    return 0;
}

static int set_timeouts(int sock, int ms)
{
    struct zsock_timeval tv = {
        .tv_sec  = ms / 1000,
        .tv_usec = (ms % 1000) * 1000,
    };

    /* The send timeout also bounds connect(). */
    if (zsock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) ||
        zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv))) {
        return -errno;
    }
    return 0;
}

static int connect_server(void)
{
    if (s_sock >= 0) return 0;

    if (!s_resolved) {
        int err = resolve();
        if (err) return err;
    }

    int sock = zsock_socket(AF_INET, SOCK_STREAM,
                            TRACCAR_USE_TLS ? IPPROTO_TLS_1_2 : IPPROTO_TCP);
    if (sock < 0) {
        LOG_ERR("socket: %d", errno);
        return -errno;
    }

    int err = TRACCAR_USE_TLS ? tls_setup(sock) : 0;
    if (!err) err = set_timeouts(sock, CONNECT_TIMEOUT_MS);
    if (err) {
        zsock_close(sock);
        return err;
    }

    watchdog_kick();
    if (zsock_connect(sock, (struct sockaddr *)&s_server, sizeof(s_server))) {
        err = -errno;
        LOG_ERR("connect %s:%d: %d", TRACCAR_HOST, TRACCAR_PORT, errno);
        zsock_close(sock);
        /* The address may have moved: look it up again next time. */
        s_resolved = false;
        return err;
    }
    watchdog_kick();
    set_timeouts(sock, REQUEST_TIMEOUT_MS);

    s_sock = sock;
    s_close_after = false;
    LOG_INF("connected to %s:%d%s", TRACCAR_HOST, TRACCAR_PORT,
            TRACCAR_USE_TLS ? " (TLS)" : "");
    return 0;
}

int transport_open(void)
{
    /* Look the server up now, while the radio is up for something else;
     * connect on the first send.  The boot-time call is followed by a
     * teardown, and a TCP handshake made only to be closed again would be
     * wasted radio time. */
    if (s_resolved) return 0;
    return resolve();
}

void transport_close(void)
{
    /* The exchange is over: let the radio go, keep the connection. */
    if (s_sock >= 0) {
        int rai = RAI_NO_DATA;
        zsock_setsockopt(s_sock, SOL_SOCKET, SO_RAI, &rai, sizeof(rai));
    }
}

void transport_teardown(void)
{
    if (s_sock >= 0) {
        zsock_close(s_sock);
        s_sock = -1;
    }
}

/* -- one request ----------------------------------------------------------- */

struct reader {
    size_t len;     /* bytes in s_rx */
    size_t pos;     /* consumed */
};

/* At least one unread byte in s_rx, or why not. */
static int rd_fill(struct reader *r)
{
    if (r->pos < r->len) return 0;
    r->pos = r->len = 0;

    ssize_t n = zsock_recv(s_sock, s_rx, sizeof(s_rx), 0);
    if (n < 0) return errno == EAGAIN ? -ETIMEDOUT : -errno;
    if (n == 0) return -ECONNRESET;      /* closed under us */
    r->len = (size_t)n;
    return 0;
}

/* One CRLF-terminated line, the CRLF dropped.  Longer than the buffer is
 * kept only as far as it fits: nothing looked for here is long. */
static int rd_line(struct reader *r, char *out, size_t cap)
{
    size_t n = 0, total = 0;

    for (;;) {
        int err = rd_fill(r);
        if (err) return err;

        char c = (char)s_rx[r->pos++];
        if (c == '\n') break;
        if (++total > 4096) return -EMSGSIZE;   /* not HTTP */
        if (c != '\r' && n < cap - 1) out[n++] = c;
    }
    out[n] = '\0';
    return 0;
}

/* Consume n body bytes, keeping what fits of them in `keep` if wanted. */
static int rd_body(struct reader *r, size_t n, char *keep, size_t cap,
                   size_t *kept)
{
    while (n > 0) {
        int err = rd_fill(r);
        if (err) return err;

        size_t take = MIN(r->len - r->pos, n);

        if (keep) {
            size_t k = MIN(take, cap - 1 - *kept);

            memcpy(keep + *kept, s_rx + r->pos, k);
            *kept += k;
            keep[*kept] = '\0';
        }
        r->pos += take;
        n -= take;
    }
    return 0;
}

/* The body of a 2xx is a command Traccar queued for the device.  Kept for
 * data.c to collect at its next response read, joined to any already
 * waiting the way the l0destar server joins commands: cmd_run() finds
 * each by name. */
static void command_received(char *body)
{
    size_t n = strlen(body);

    while (n > 0 && isspace((unsigned char)body[n - 1])) body[--n] = '\0';
    while (*body && isspace((unsigned char)*body)) body++;
    if (*body == '\0') return;

    /* A page rather than a command: the port is not Traccar's. */
    for (const char *p = body; *p; p++) {
        if (*p == '<' || iscntrl((unsigned char)*p)) {
            LOG_WRN("ignoring a response body that is not a command");
            return;
        }
    }

    size_t have = strlen(s_cmd);

    if (have + 1 + strlen(body) >= sizeof(s_cmd)) {
        LOG_WRN("command buffer full — dropping: %s", body);
        return;
    }
    if (have) s_cmd[have++] = ',';
    strcpy(s_cmd + have, body);
    LOG_INF("command from server: %s", body);
}

/* Read one response.  Returns its status code, or a negative errno when the
 * connection failed or what came back was not HTTP. */
static int read_response(void)
{
    struct reader r = { .len = 0, .pos = 0 };
    char line[128];
    int err;

    err = rd_line(&r, line, sizeof(line));
    if (err) return err;
    if (strncmp(line, "HTTP/1.", 7) != 0 || strlen(line) < 12) {
        LOG_ERR("not an HTTP reply: %.32s", line);
        return -EPROTO;
    }

    int status = atoi(line + 9);
    if (status < 100) return -EPROTO;

    size_t content_length = 0;
    bool chunked = false;

    for (;;) {
        err = rd_line(&r, line, sizeof(line));
        if (err) return err;
        if (line[0] == '\0') break;         /* end of the headers */

        char *v = strchr(line, ':');
        if (!v) continue;
        *v++ = '\0';
        while (*v == ' ') v++;

        if (strcasecmp(line, "content-length") == 0) {
            content_length = strtoul(v, NULL, 10);
        } else if (strcasecmp(line, "transfer-encoding") == 0 &&
                   strcasecmp(v, "chunked") == 0) {
            chunked = true;
        } else if (strcasecmp(line, "connection") == 0 &&
                   strcasecmp(v, "close") == 0) {
            s_close_after = true;
        }
    }

    /* Only a 2xx body is a command; an error page is drained and ignored. */
    char *keep = (status / 100 == 2) ? s_body : NULL;
    size_t kept = 0;

    s_body[0] = '\0';
    if (chunked) {
        for (;;) {
            err = rd_line(&r, line, sizeof(line));
            if (err) return err;

            size_t n = strtoul(line, NULL, 16);
            if (n == 0) {
                /* Trailers, if any, then the empty line. */
                do {
                    err = rd_line(&r, line, sizeof(line));
                    if (err) return err;
                } while (line[0] != '\0');
                break;
            }
            err = rd_body(&r, n, keep, sizeof(s_body), &kept);
            if (!err) err = rd_line(&r, line, sizeof(line));   /* the CRLF after it */
            if (err) return err;
        }
    } else if (content_length > 0) {
        err = rd_body(&r, content_length, keep, sizeof(s_body), &kept);
        if (err) return err;
    }
    if (kept) command_received(s_body);
    return status;
}

static int send_all(const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = zsock_send(s_sock, buf, len, 0);
        if (n < 0) return -errno;
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* GET s_query.  `last` is the final request of this send, after which the
 * radio may be let go.  Returns the HTTP status, or a negative errno. */
static int request(bool last)
{
    /* Release assistance.  The whole request is one send, so the hint
     * covers it: the modem waits for the response and then drops the RRC
     * connection — unless more requests follow, or streaming (track mode)
     * keeps it up for the next record. */
    int rai = (s_streaming || !last) ? RAI_ONGOING : RAI_ONE_RESP;
    zsock_setsockopt(s_sock, SOL_SOCKET, SO_RAI, &rai, sizeof(rai));

    int n = snprintf(s_req, sizeof(s_req),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s:%d\r\n"
                     "User-Agent: l0destar/%s\r\n"
                     "\r\n",
                     s_query, TRACCAR_HOST, TRACCAR_PORT, APP_VERSION_STRING);
    if (n < 0 || n >= (int)sizeof(s_req)) {
        return -EMSGSIZE;
    }

    watchdog_kick();
    int err = send_all(s_req, (size_t)n);
    if (!err) err = read_response();
    watchdog_kick();

    if (err > 0 && s_close_after) {
        transport_teardown();
    }
    return err;
}

/* -- the transport interface ------------------------------------------------ */

/* The next line Traccar gets a request for, at or after p; NULL when none. */
static const char *next_line(const char *p, const char *end, size_t *len)
{
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);

        if (n > 0 && line_kind(p, n) != LINE_SKIP) {
            *len = n;
            return p;
        }
        p += n + 1;
    }
    return NULL;
}

/* One line as one request.  Returns 1 when Traccar accepted it, 0 when it
 * was dropped (malformed, or refused by the server for a reason a retry
 * would only repeat), or a negative errno when the server could not be
 * reached and the caller should keep the line for later. */
static int send_line(const char *id, const char *p, size_t len, bool last)
{
    size_t n = MIN(len, sizeof(s_line) - 1);

    memcpy(s_line, p, n);
    s_line[n] = '\0';

    struct query q = { .buf = s_query, .cap = sizeof(s_query) };

    q_raw(&q, "/?id=");
    q_enc(&q, id, strlen(id));

    switch (line_kind(p, len)) {
    case LINE_RECORD:
        if (encode_record(&q, s_line)) {
            LOG_WRN("malformed record dropped: %.17s...", p);
            return 0;
        }
        break;
    case LINE_ALERT:
        encode_alert(&q, s_line + 2);
        break;
    case LINE_DTC:
        q_str(&q, "dtcs", s_line + 2);
        break;
    default:
        return 0;
    }
    if (q.full) {
        LOG_WRN("request full at %u bytes — some fields left out",
                (unsigned)q.len);
    }

    int status = request(last);

    if (status < 0) {
        /* The connection rather than the server: most likely dropped while
         * the unit was asleep or the modem was cycled.  Once more, on a
         * new one. */
        LOG_WRN("request failed (%d) — reconnecting", status);
        transport_teardown();
        int err = connect_server();
        if (err) return err;
        status = request(last);
        if (status < 0) {
            LOG_ERR("request failed again: %d", status);
            transport_teardown();
            return status;
        }
    }
    if (status >= 500) {
        /* The server's problem: keep the lines and try later. */
        LOG_WRN("HTTP %d from %s — will retry", status, TRACCAR_HOST);
        return -EIO;
    }
    if (status >= 300) {
        /* Ours: retrying would only repeat it.  400 is Traccar not knowing
         * the id; a redirect means the port is not the OsmAnd listener. */
        if (status == 400) {
            LOG_ERR("HTTP 400 — Traccar rejected id \"%s\" (device not "
                    "registered?) — dropped", id);
        } else {
            LOG_ERR("HTTP %d from %s:%d — dropped", status, TRACCAR_HOST,
                    TRACCAR_PORT);
        }
        return 0;
    }
    return 1;
}

int transport_send(const uint8_t *plaintext, size_t pt_len)
{
    const char *id = TRACCAR_ID;

    if (id[0] == '\0') {
        LOG_WRN("device id not known yet (IMEI unread) — dropping");
        return -EACCES;
    }

    const char *end = (const char *)plaintext + pt_len;
    size_t len = 0;
    const char *p = next_line((const char *)plaintext, end, &len);

    if (p == NULL) {
        return 0;               /* nothing Traccar wants: log lines alone */
    }

    int err = connect_server();
    if (err) return err;

    int sent = 0;

    while (p != NULL) {
        size_t next_len = 0;
        const char *after = p + len + 1;
        const char *next = after < end ? next_line(after, end, &next_len) : NULL;

        int rc = send_line(id, p, len, next == NULL);
        if (rc < 0) return rc;
        sent += rc;

        p = next;
        len = next_len;
    }

    if (sent > 0) {
        s_acked = true;
        LOG_INF("sent %d request%s", sent, sent == 1 ? "" : "s");
    }
    return 0;
}

int transport_recv_response(char *out_plaintext, size_t out_len, int timeout_ms)
{
    ARG_UNUSED(timeout_ms);     /* nothing to wait for: it came with the request */

    if (!s_acked && s_cmd[0] == '\0') {
        return -ENODATA;
    }

    /* In the l0destar server's shape, "1,<interval>,<movement_alarm>[,cmd]":
     * -1 leaves both settings as they are (data.c applies a value only when
     * it is >= 0), and cmd is whatever Traccar queued for the device. */
    int n = s_cmd[0] ? snprintf(out_plaintext, out_len, "1,-1,-1,%s", s_cmd)
                     : snprintf(out_plaintext, out_len, "1,-1,-1");

    s_acked = false;
    s_cmd[0] = '\0';
    if (n < 0) return -EIO;
    if ((size_t)n >= out_len) n = (int)out_len - 1;
    return n;
}
