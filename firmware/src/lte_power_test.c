/*
 * LTE TX power / brown-out test rig (CONFIG_APP_LTE_POWER_TEST).
 *
 * Runs instead of the tracker and never returns.  Brings the modem up once,
 * then idles with LED1 on.  An ignition OFF->ON edge — or ENTER on the
 * console, when one is attached — starts a ~10 s uplink burst: LED2 joins
 * LED1, and UDP datagrams are pushed at the modem as fast as it will queue
 * them, so the radio spends the whole window transmitting — the same
 * sustained-TX draw a unit in a low-signal area produces, where every PUSCH
 * transmission goes out at full power with repetitions.
 *
 * The ignition trigger exists because the measurement setup precludes the
 * console one: USB takes over the power input, so a PSU-only run (the whole
 * point — watching the current draw) has no serial console.  Flick the
 * ignition wire to trigger; the line must drop back to OFF (debounced)
 * before it can trigger again, so a wire left ON fires exactly one burst.
 * Needs APP_DEBUG_IGNITION=-1 (the live GPIO read).
 *
 * If the supply survives the burst, all three LEDs light and stay lit until
 * ignition is turned OFF, which drops the rig back to LED1 alone and re-arms
 * the trigger.  If the board browns out it resets instead: the operator sees
 * the LEDs go dark and come back as LED1 only, with LED3 never having lit.
 *
 * The watchdog is never armed in this build (watchdog_init() is bypassed
 * along with the rest of the tracker), so nothing here needs to kick it.
 */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>

#include "app.h"
#include "hw_common.h"
#include "pins.h"

LOG_MODULE_REGISTER(lte_power_test, CONFIG_APP_LOG_LEVEL);

#define BURST_MS        10000   /* uplink burst length */
#define PASS_HOLD_MS    5000    /* all-LEDs hold when there is no ignition
                                 * line to acknowledge with (ENTER trigger
                                 * with ignition already off) */
#define PAYLOAD_LEN     500     /* per-datagram payload (NB-IoT-safe size) */
#define SEND_TIMEOUT_MS 1000    /* bound on one send while modem buffers drain */

#define SERVER_HOST \
    (sizeof(CONFIG_APP_SERVER_HOST) > 1 ? CONFIG_APP_SERVER_HOST : HOSTNAME)

/* -- console ---------------------------------------------------------------- */

#if DT_HAS_CHOSEN(zephyr_console)
static const struct device *const s_console =
    DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_console));
#else
static const struct device *const s_console = NULL;
#endif

static void wait_trigger(void)
{
    unsigned char c;
    bool have_console = s_console != NULL && device_is_ready(s_console);

    if (have_console) {
        while (uart_poll_in(s_console, &c) == 0) {
            /* drain stale input so only a fresh keypress triggers */
        }
    }

    /* Ignition path: fire on a debounced OFF->ON edge, and only re-arm once
     * the line has read OFF for the same debounce — a wire left on battery
     * fires exactly one burst, not one per loop.  Debounce is 5 consecutive
     * identical 20 ms samples, as in board_test.c's ign_stable(). */
    bool armed = false;
    int  run = 0;
    int  last = ignition_read();   /* 0 = ON (active low) */

    for (;;) {
        if (have_console && uart_poll_in(s_console, &c) == 0 &&
            (c == '\r' || c == '\n')) {
            return;
        }

        int v = ignition_read();
        run = (v == last) ? run + 1 : 0;
        last = v;
        if (run >= 5) {
            if (v != 0) {
                armed = true;       /* debounced OFF — ready for the edge */
            } else if (armed) {
                return;             /* debounced OFF->ON edge */
            }
        }
        k_msleep(20);
    }
}

/* Hold the pass indication until the operator acknowledges it by turning
 * ignition OFF (debounced like the trigger).  When ignition is already off
 * — the ENTER-triggered console workflow — fall back to a fixed hold so the
 * LEDs are still visibly lit; ENTER also dismisses the wait early. */
static void wait_pass_ack(void)
{
    unsigned char c;
    bool have_console = s_console != NULL && device_is_ready(s_console);

    if (have_console) {
        while (uart_poll_in(s_console, &c) == 0) {
            /* drain */
        }
    }

    if (ignition_read() != 0) {
        k_msleep(PASS_HOLD_MS);
        return;
    }

    int run = 0;
    for (;;) {
        if (have_console && uart_poll_in(s_console, &c) == 0 &&
            (c == '\r' || c == '\n')) {
            return;
        }
        run = (ignition_read() != 0) ? run + 1 : 0;
        if (run >= 5) {
            return;
        }
        k_msleep(20);
    }
}

/* -- destination ------------------------------------------------------------ */
/* The telemetry server's UDP port.  The payload is garbage, so the server
 * fails the AEAD check and drops it — nothing downstream reacts. */

static struct sockaddr_in s_server;
static bool s_resolved;

static int resolve_server(void)
{
    struct zsock_addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_DGRAM,
    };
    struct zsock_addrinfo *res = NULL;
    char port_str[8];

    snprintf(port_str, sizeof(port_str), "%u", UDP_PORT);
    int err = zsock_getaddrinfo(SERVER_HOST, port_str, &hints, &res);
    if (err || !res) {
        printk("DNS failed for %s: %d\n", SERVER_HOST, err);
        return -EIO;
    }
    memcpy(&s_server, res->ai_addr, sizeof(s_server));
    zsock_freeaddrinfo(res);
    s_resolved = true;
    return 0;
}

/* -- burst ------------------------------------------------------------------ */

static int burst_run(void)
{
    int reg = modem_get_network_status();
    if (reg != 1 && reg != 5) {
        printk("registration lost — reconnecting...\n");
        if (modem_connect()) {
            printk("reconnect failed\n");
            return -1;
        }
    }
    if (!s_resolved && resolve_server()) {
        return -1;
    }

    int sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        printk("socket: %d\n", errno);
        return -1;
    }
    if (zsock_connect(sock, (struct sockaddr *)&s_server,
                      sizeof(s_server))) {
        printk("connect: %d\n", errno);
        zsock_close(sock);
        return -1;
    }

    /* Bound each send so the loop keeps the modem's TX buffers topped up
     * without ever wedging on them: on NB-IoT in poor signal one datagram
     * can take seconds of air time, and a full buffer means the radio
     * already has all the work it can do. */
    struct zsock_timeval tv = {
        .tv_sec  = SEND_TIMEOUT_MS / 1000,
        .tv_usec = (SEND_TIMEOUT_MS % 1000) * 1000,
    };
    zsock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    static uint8_t payload[PAYLOAD_LEN];
    for (int i = 0; i < PAYLOAD_LEN; i++) {
        payload[i] = (uint8_t)i;
    }

    int pkts = 0, errs = 0;
    int64_t t0 = k_uptime_get();
    int64_t last_report = t0;

    printk("burst: %d s of uplink to %s:%u (%s)\n",
           BURST_MS / 1000, SERVER_HOST, UDP_PORT, modem_rat());

    for (;;) {
        int64_t now = k_uptime_get();
        if (now - t0 >= BURST_MS) {
            break;
        }

        if (zsock_send(sock, payload, sizeof(payload), 0) >= 0) {
            pkts++;
        } else {
            errs++;
            k_msleep(20);   /* buffers full — let the radio drain them */
        }

        if (now - last_report >= 2000) {
            last_report = now;
            int mv = 0;
            if (modem_read_vbat(&mv) == 0) {
                printk("  %llds: %d pkts queued, VDD %d mV under load\n",
                       (now - t0) / 1000, pkts, mv);
            } else {
                printk("  %llds: %d pkts queued\n", (now - t0) / 1000, pkts);
            }
        }
    }

    /* Ask for a prompt RRC release so idle current between bursts settles,
     * mirroring transport_close(). */
    int rai = RAI_NO_DATA;
    zsock_setsockopt(sock, SOL_SOCKET, SO_RAI, &rai, sizeof(rai));
    zsock_close(sock);

    int64_t took = k_uptime_get() - t0;
    printk("burst done: %d datagrams (%d bytes) queued in %lld ms, "
           "%d send stalls\n",
           pkts, pkts * PAYLOAD_LEN, took, errs);

    if (pkts == 0) {
        printk("nothing was accepted for transmit — burst not valid\n");
        return -1;
    }
    return 0;
}

/* -- driver ----------------------------------------------------------------- */

void lte_power_test_run(void)
{
    printk("\n");
    printk("==============================================\n");
    printk("  l0destar LTE POWER TEST -- board %s, fw %s\n",
           fota_board_id(), fota_version());
    printk("==============================================\n");

    /* The ignition sense input is normally configured by hw_power_init(),
     * which this build bypasses along with the rest of the INA228 bring-up.
     * Left unconfigured, the pin's input buffer is disconnected and reads a
     * constant, so the trigger edge never arrives.  Same pull policy as
     * hw_power_init(): the PCBs have an external 56K pull-up. */
    if (IS_ENABLED(CONFIG_APP_BOARD_IGN_EXT_PULLUP)) {
        gpio_pin_configure(hw_gpio0, PIN_IGN_SENSE, GPIO_INPUT);
    } else {
        gpio_pin_configure(hw_gpio0, PIN_IGN_SENSE,
                           GPIO_INPUT | GPIO_PULL_UP);
    }

    led_mask(0);

    if (modem_init()) {
        printk("modem init FAILED — cannot run\n");
        for (;;) {
            led_toggle();
            k_msleep(200);
        }
    }

    printk("connecting (APN \"%s\", can take 30 s+)...\n", g_settings.apn);
    while (modem_connect()) {
        printk("connect failed — retrying in 10 s\n");
        k_msleep(10000);
    }
    printk("registered (%s)\n", modem_rat());
    if (resolve_server()) {
        printk("will retry DNS at burst time\n");
    }

    for (;;) {
        led_mask(LED_MASK_1);
        printk("\nREADY — flick ignition ON (or press ENTER) for a %d s "
               "TX burst\n", BURST_MS / 1000);
        wait_trigger();

        led_mask(LED_MASK_1 | LED_MASK_2);
        if (burst_run() == 0) {
            /* Reaching this line is the pass: the supply carried the burst
             * without a reset.  LED3 lighting is the operator's proof, and
             * it stays lit until they turn ignition OFF — which is also
             * what re-arms the trigger for the next run. */
            led_mask(LED_MASK_1 | LED_MASK_2 | LED_MASK_3);
            printk("SURVIVED — no brown-out; ignition OFF to re-arm\n");
            wait_pass_ack();
        } else {
            printk("burst did not run — check network and retry\n");
        }
    }
}
