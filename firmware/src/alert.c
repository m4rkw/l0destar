/*
 * Alert queue + dispatch.  Alerts piggyback on telemetry sends and are also
 * sent standalone after movement wakes.
 *
 * Held until answered.  transport_send() returning 0 means the modem took the
 * datagram, nothing more; the server's reply is the receipt (see databuf.c).
 * An alert used to be forgotten the moment the modem had it, so one sent just
 * before a network reject was simply lost: on 2026-09-30 the bench's
 * "backup power: 9.19V" went out at 19:55:19, the network rejected the
 * registration two seconds later (EMM cause 9, TAC 12296), and the server
 * never saw it.  Now an alert keeps its slot until the reply to the datagram
 * that carried it comes in.  One still unanswered after APP_ACK_TIMEOUT_S goes
 * out again with the next send, as does one still unanswered when the socket
 * is about to close (databuf_settle, which waits for these replies too).
 *
 * A reply can be lost when the alert was not, so an alert can arrive twice.
 * Each carries an id, ",aid=<n>" at the end of the line, and the server drops
 * a copy of one it has already relayed rather than notify again.  The ids are
 * consecutive from a random start, like rid=, so a reboot does not reuse the
 * previous boot's.  Only where there is a reply to wait for: Traccar's HTTP
 * 2xx is its own receipt, and its builds hold nothing (APP_DATABUF_UNACKED_
 * SLOTS = 0), so their alert lines are exactly what they were.
 */

#include <string.h>
#include <stdio.h>
#include <zephyr/logging/log.h>

#include "app.h"
#include "config.h"

LOG_MODULE_REGISTER(alert, CONFIG_APP_LOG_LEVEL);

#define ALERT_QUEUE_SIZE 8
#define ALERT_MSG_SIZE   120

#define ALERT_HOLD       (CONFIG_APP_DATABUF_UNACKED_SLOTS > 0)
#define ACK_TIMEOUT_MS   ((int64_t)CONFIG_APP_ACK_TIMEOUT_S * 1000)

enum alert_state {
    ALERT_FREE,
    ALERT_QUEUED,       /* waiting for a send */
    ALERT_SENT,         /* with the modem, waiting for the reply */
};

static struct {
    uint8_t  state;
    int8_t   prio;
    uint32_t aid;
    uint32_t sent_id;   /* the transport's id for the datagram that carried it */
    int64_t  sent_ms;
    char     msg[ALERT_MSG_SIZE];
} s_alert[ALERT_QUEUE_SIZE];

/* Alerts waiting for a send.  What callers test before bringing the radio up
 * for alert_send(): one already with the modem is not counted. */
int alert_count;

static uint32_t s_aid;

static uint32_t next_aid(void)
{
    if (s_aid == 0) {
        uint16_t seed = 0;

        if (!crypto_random((uint8_t *)&seed, sizeof(seed))) {
            seed = (uint16_t)k_uptime_get();
        }
        s_aid = seed ? seed : 1u;
    }
    return ++s_aid;
}

void alert_enqueue(const char *msg, int priority)
{
    int slot = -1;

    for (int i = 0; i < ALERT_QUEUE_SIZE; i++) {
        if (s_alert[i].state == ALERT_FREE) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        LOG_WRN("queue full, dropping");
        return;
    }
    strncpy(s_alert[slot].msg, msg, ALERT_MSG_SIZE - 1);
    s_alert[slot].msg[ALERT_MSG_SIZE - 1] = '\0';
    s_alert[slot].prio = (int8_t)priority;
    s_alert[slot].aid = next_aid();
    s_alert[slot].state = ALERT_QUEUED;
    alert_count++;
#ifdef CONFIG_APP_DEMO_MODE
    /* The locate/tomtom replies carry the position in their text. */
    char masked[ALERT_MSG_SIZE];
    LOG_INF("queued: %s", demo_mask_coords(s_alert[slot].msg,
                                           strlen(s_alert[slot].msg),
                                           masked, sizeof(masked)));
#else
    LOG_INF("queued: %s", msg);
#endif
}

/* The queued alert raised first, or -1: sent in the order they were raised. */
static int next_queued(void)
{
    int best = -1;

    for (int i = 0; i < ALERT_QUEUE_SIZE; i++) {
        if (s_alert[i].state == ALERT_QUEUED &&
            (best < 0 || (int32_t)(s_alert[i].aid - s_alert[best].aid) < 0)) {
            best = i;
        }
    }
    return best;
}

/* Back in the queue for the next send. */
static void requeue(int i, const char *why)
{
    LOG_WRN("alert #%u unanswered %s - sending it again", s_alert[i].aid, why);
    s_alert[i].state = ALERT_QUEUED;
    alert_count++;
}

int alert_send(void)
{
    alert_expire();

    int i;

    while ((i = next_queued()) >= 0) {
        char line[ALERT_MSG_SIZE + 32];
        int n = ALERT_HOLD
            ? snprintf(line, sizeof(line), "A,%d,%s,aid=%u",
                       s_alert[i].prio, s_alert[i].msg, s_alert[i].aid)
            : snprintf(line, sizeof(line), "A,%d,%s",
                       s_alert[i].prio, s_alert[i].msg);

        if (n <= 0) {
            s_alert[i].state = ALERT_FREE;      /* cannot happen */
            alert_count--;
            continue;
        }
        if (transport_send((const uint8_t *)line, (size_t)n) != 0) {
            /* The rest stay queued for the next attempt. */
            return 0;
        }
        alert_count--;

        uint32_t id = transport_sent_id();

        if (ALERT_HOLD && id != 0) {
            s_alert[i].state = ALERT_SENT;
            s_alert[i].sent_id = id;
            s_alert[i].sent_ms = k_uptime_get();
        } else {
            /* No reply to wait for: the send was its own receipt. */
            s_alert[i].state = ALERT_FREE;
        }
    }
    return 1;
}

int alert_send_standalone(void)
{
    /* In the original firmware this also handled modem wake-up; here the
     * transport layer brings the socket up on demand.  Nothing else is going
     * out, and the caller takes the radio down straight after, so wait here
     * for the replies that say the alerts arrived — normally a second or so.
     * Any still unanswered go again with a later send. */
    int ok = alert_send();
    int64_t deadline = k_uptime_get() + RESPONSE_TIMEOUT_MS;

    while (alert_unanswered() > 0 && k_uptime_get() < deadline) {
        if (transport_poll() < 0) {
            break;                              /* no socket to hear it on */
        }
        if (alert_unanswered() > 0) {
            k_msleep(100);
        }
    }
    return ok;
}

void alert_ack(uint32_t id)
{
    for (int i = 0; i < ALERT_QUEUE_SIZE; i++) {
        if (s_alert[i].state == ALERT_SENT && s_alert[i].sent_id == id) {
            LOG_DBG("alert #%u answered", s_alert[i].aid);
            s_alert[i].state = ALERT_FREE;
        }
    }
}

int alert_unanswered(void)
{
    int n = 0;

    for (int i = 0; i < ALERT_QUEUE_SIZE; i++) {
        if (s_alert[i].state == ALERT_SENT) {
            n++;
        }
    }
    return n;
}

void alert_expire(void)
{
    int64_t now = k_uptime_get();

    for (int i = 0; i < ALERT_QUEUE_SIZE; i++) {
        if (s_alert[i].state == ALERT_SENT &&
            now - s_alert[i].sent_ms >= ACK_TIMEOUT_MS) {
            requeue(i, "past the timeout");
        }
    }
}

void alert_settle(void)
{
    for (int i = 0; i < ALERT_QUEUE_SIZE; i++) {
        if (s_alert[i].state == ALERT_SENT) {
            requeue(i, "as the socket closes");
        }
    }
}
