/*
 * Alert queue + dispatch.  Alerts piggyback on telemetry sends and are also
 * sent standalone after movement wakes — though "movement wake" is moot here
 * since the accelerometer is stubbed.
 */

#include <string.h>
#include <stdio.h>
#include <zephyr/logging/log.h>

#include "app.h"

LOG_MODULE_REGISTER(alert, CONFIG_APP_LOG_LEVEL);

#define ALERT_QUEUE_SIZE 5
#define ALERT_MSG_SIZE   120

static char alert_queue[ALERT_QUEUE_SIZE][ALERT_MSG_SIZE];
static int  alert_prio[ALERT_QUEUE_SIZE];
int         alert_count;

void alert_enqueue(const char *msg, int priority)
{
    if (alert_count >= ALERT_QUEUE_SIZE) {
        LOG_WRN("queue full, dropping");
        return;
    }
    strncpy(alert_queue[alert_count], msg, ALERT_MSG_SIZE - 1);
    alert_queue[alert_count][ALERT_MSG_SIZE - 1] = '\0';
    alert_prio[alert_count] = priority;
    alert_count++;
    LOG_INF("queued: %s", msg);
}

int alert_send(void)
{
    if (alert_count == 0) return 1;

    for (int i = 0; i < alert_count; i++) {
        char line[ALERT_MSG_SIZE + 16];
        int n = snprintf(line, sizeof(line), "A,%d,%s",
                         alert_prio[i], alert_queue[i]);
        if (n <= 0) continue;

        int rc = transport_send((const uint8_t *)line, (size_t)n);
        if (rc != 0) {
            /* Leave remaining alerts queued for the next attempt. */
            if (i > 0) {
                int remaining = alert_count - i;
                memmove(alert_queue[0], alert_queue[i],
                        remaining * ALERT_MSG_SIZE);
                memmove(alert_prio, alert_prio + i,
                        remaining * sizeof(alert_prio[0]));
                alert_count = remaining;
            }
            return 0;
        }
    }
    alert_count = 0;
    return 1;
}

int alert_send_standalone(void)
{
    /* In the original firmware this also handled modem wake-up; here the
     * transport layer brings the socket up on demand. */
    return alert_send();
}
