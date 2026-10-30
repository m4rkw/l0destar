/*
 * GNSS using an external u-blox MAX-M10S, in place of the nRF9151's built-in
 * receiver (gnss.c).  Selected by APP_BOARD_HAS_EXT_GNSS, which the l0destar
 * v4.0 board sets.  The interface is gnss.c's, so nothing above it changes:
 * fixes land in g_gnss as they arrive and gnss_collect() waits for the next.
 *
 * What differs from the built-in receiver:
 *
 *  - It has a radio of its own.  LTE cannot block it, the modem has no GNSS in
 *    its system mode on this board (modem.c), and a send costs the receiver
 *    nothing.  So there is no nRF Cloud A-GNSS, no priority mode and no starving, and
 *    gnss_stop()/gnss_resume() — which exist to hand the nRF's single RF
 *    front-end to LTE — leave the receiver alone.  It runs whenever its rail
 *    is up, and g_gnss keeps up with it through a send.
 *
 *  - Its rail is the switched PP3V3_GNSS (hw_domain AUX, enabled by
 *    GNSS_ENABLE), shared with the antenna bias tee.  The UART is opened when
 *    that rail comes up and closed before it goes down (hw_domain_notify_set):
 *    a UARTE left open holds the HF clock, which a sleeping unit cannot afford,
 *    and a TX pin still driven into an unpowered receiver would back-feed the
 *    dead rail.  The callers already hold the rail around a fix the way they
 *    held it for the bias tee, so none of them changed.
 *
 *  - It keeps its ephemeris and time on V_BCKP (GNSS_BCKP_EN, hw_domain.c), so
 *    a parked wake gets the data sheet's 1 s hot start and not a 25 s cold
 *    one.  Cycling VCC does not lose them.
 *
 *  - What the backup supply cannot keep is orbits for a unit that has sat for
 *    days, or a receiver that lost it.  For that there is u-blox AssistNow
 *    (assistnow.c), which this driver asks for once per power-up when the
 *    first few satellite lists show fewer than ASSIST_EPH_MIN ephemerides on
 *    hand and there is no fix yet, and through which it passes the data to the
 *    receiver (gnss_ext_assist_inject).  It also keeps the receiver's chip id
 *    and version replies as they came, which is how AssistNow registers a unit.
 *
 *  - The receiver is configured at every power-up, in RAM only.  It is found
 *    at whichever baud rate it answers on (a MAX-M10S comes up at 38400 with
 *    NMEA out), moved to APP_GNSS_EXT_BAUD, told to stop NMEA and to send
 *    UBX-NAV-PVT, -DOP and -SAT once a second.  Nothing is written to its flash
 *    or battery backed layers, so a receiver that has been reconfigured by hand
 *    still comes up the way this expects.
 *
 *  - A receiver that can be heard but never acknowledges the configuration
 *    (the nRF's TX not reaching it, UBX input switched off) is not dead: the
 *    driver carries on with the NMEA it sends by itself (gnss_nmea.c) and
 *    raises an alert that says so.  Fixes come out the same; the receiver
 *    keeps its own defaults, 1 Hz and the portable dynamic model.  See
 *    APP_GNSS_EXT_NMEA_FALLBACK.
 *
 * Bytes are received into DMA buffers by the nrfx UARTE driver and passed to a
 * thread through a ring buffer.  The thread parses the UBX frames (gnss_ubx.c),
 * publishes fixes, and while the rail is up watches the receiver: one that goes
 * quiet is found again and reconfigured.
 *
 * The UARTE hands a DMA buffer over only when it is full, so the end of each
 * burst the receiver sends sits in a part-filled one.  Reception is never
 * stopped to get it out: aborting the buffer leaves the receiver deaf for a few
 * byte times while it restarts, and a byte arriving then is lost or garbled.
 * Instead TIMER2 counts RXDRDY (one per received byte, over a DPPI channel), so
 * how far the DMA has got is known at any moment, and the thread copies that
 * much out of the buffer while it is still being filled — what Zephyr's own
 * UARTE driver does for a receive timeout.  rx_idle_flush() has the details.
 *
 * UARTE0 is the console and UARTE1 the K-line's (hw_kline.c); this takes UARTE2
 * and TIMER2.
 */

#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_timer.h>
#include <helpers/nrfx_gppi.h>
#include <nrfx_uarte.h>

#include "app.h"
#include "assistnow.h"
#include "gnss_nmea.h"
#include "gnss_ubx.h"
#include "hw_domain.h"
#include "pins.h"

LOG_MODULE_REGISTER(gnss, CONFIG_APP_LOG_LEVEL);

#define GNSS_UARTE          NRF_UARTE2
#define GNSS_UARTE_IRQ_PRIO 2
#define GNSS_COUNTER        NRF_TIMER2      /* counts the bytes the UARTE receives */
#define GNSS_BAUD           CONFIG_APP_GNSS_EXT_BAUD

/* Two DMA buffers, each 23 ms of line at 115200.  The next one is asked for
 * as the current one starts, so this is also how late the interrupt may run:
 * immediate-mode logging (the bench) masks interrupts for a whole log line,
 * about 9 ms, which 128 byte buffers (11 ms) left little room for.  The ring
 * holds what arrives while the thread is busy, for the same reason. */
#define RX_DMA_LEN          256
#define RX_RING_SIZE        2048
#define TICK_MS             10       /* receive housekeeping; also the idle-flush quantum */
#define PROBE_MS            250      /* how long one baud rate gets to answer */
#define FIND_MS             6000     /* how long a bring-up searches before giving up */
#define SILENT_MS           5000     /* no NAV-PVT for this long: find the receiver again */
#define ACK_MS              500
#define SWITCH_MS           1500     /* how long a receiver gets to take up a new baud rate */
#define DEAD_AFTER_FAILS    2        /* failed bring-ups before the receiver is called dead */
#define RETRY_PAUSE_MS      2000
#define ASSIST_EPH_MIN      5        /* ephemerides on hand below which assistance is wanted */
#define ASSIST_EPOCHS       3        /* satellite lists seen first, so a hot start has listed its own */
#define ASSIST_WINDOW       16       /* assistance messages sent between looks at the acknowledgements */
#define ASSIST_LAG          4        /* ... unacknowledged messages allowed to be in flight */
#define ASSIST_ACK_MS       1500     /* how long the last acknowledgements are waited for */

#define THREAD_STACK        3072
#define THREAD_PRIO         K_PRIO_PREEMPT(8)

struct gnss_fix g_gnss;

/* -- UART --------------------------------------------------------------------- */

static nrfx_uarte_t s_uarte = NRFX_UARTE_INSTANCE(GNSS_UARTE);

/* Serialises opening, closing and writing the UART between the thread and
 * whoever drops the rail.  The receive interrupt never takes it. */
static K_MUTEX_DEFINE(s_lock);
static bool s_open;                 /* under s_lock */
static uint32_t s_baud;             /* rate the UART is open at */

static uint8_t s_dma[2][RX_DMA_LEN] __aligned(4);
static uint8_t s_dma_idx;
static uint8_t s_tx[96] __aligned(4);
RING_BUF_DECLARE(gnss_rx_ring, RX_RING_SIZE);
static K_SEM_DEFINE(s_rx_sem, 0, 1);

/* Counted in the interrupt, reported by the thread.  They are totals since
 * boot; the reports take differences. */
static atomic_t s_rx_err;
static atomic_t s_rx_drop;
static atomic_t s_rx_late;
static atomic_t s_rx_restarts;
static atomic_t s_buf_refused;      /* buffers the driver would not take */
static atomic_t s_rx_in;            /* bytes put in the ring, by the interrupt or by a flush */
static atomic_t s_isr_bufs;         /* buffers the driver finished */
static atomic_t s_flushes;          /* part-filled buffers read out early */
static uint32_t s_rx_bytes;         /* bytes the thread has parsed */
static volatile bool s_rx_disabled;

/* Where the DMA is, for reading a buffer that is still being filled.  Bytes
 * received since the UART opened are s_deliv_total once they are in the ring:
 * s_done_total of them in buffers the driver has finished, the rest at the start
 * of s_dma[s_cur], which is the buffer being filled.  Changed by the interrupt
 * and, with interrupts locked, by rx_idle_flush(). */
static uint32_t s_done_total;
static uint32_t s_deliv_total;
static uint8_t s_cur;
static bool s_have_cur;
static bool s_count_ok;             /* the byte counter runs; under s_lock */
static nrfx_gppi_handle_t s_count_h;

static void uarte_evt(nrfx_uarte_event_t const *ev, void *ctx)
{
    ARG_UNUSED(ctx);

    switch (ev->type) {
    case NRFX_UARTE_EVT_RX_BUF_REQUEST:
        /* Two buffers taken in turn: one is received into while the other is
         * copied out, and the copy happens in RX_DONE, ahead of the request
         * that hands the buffer back.  The turn only passes when the driver
         * took the buffer: it refuses one while an abort is under way, and
         * stepping on regardless would hand it, next time, a buffer that is
         * still being received into. */
        if (nrfx_uarte_rx_buffer_set(&s_uarte, s_dma[s_dma_idx], RX_DMA_LEN) == 0) {
            if (!s_have_cur) {
                /* the first buffer after an enable is the one filled first */
                s_cur = s_dma_idx;
                s_have_cur = true;
            }
            s_dma_idx ^= 1;
        } else {
            atomic_inc(&s_buf_refused);
        }
        break;
    case NRFX_UARTE_EVT_RX_DONE: {
        /* A buffer that filled up (or was cut short by an abort).  Its first
         * `sent` bytes may be in the ring already, from rx_idle_flush(). */
        const uint32_t len = ev->data.rx.length;
        const uint32_t sent = MIN(s_deliv_total - s_done_total, len);
        const uint32_t put = len > sent ?
            ring_buf_put(&gnss_rx_ring, ev->data.rx.p_buffer + sent, len - sent) : 0;

        if (put < len - sent) {
            atomic_add(&s_rx_drop, (atomic_val_t)(len - sent - put));
        }
        s_done_total += len;
        s_deliv_total = s_done_total;
        s_cur = (ev->data.rx.p_buffer == s_dma[0]) ? 1 : 0;
        if (len) {
            atomic_add(&s_rx_in, (atomic_val_t)put);
            atomic_inc(&s_isr_bufs);
            k_sem_give(&s_rx_sem);
        }
        break;
    }
    case NRFX_UARTE_EVT_ERROR:
        atomic_inc(&s_rx_err);
        break;
    case NRFX_UARTE_EVT_RX_BUF_TOO_LATE:
        atomic_inc(&s_rx_late);
        break;
    case NRFX_UARTE_EVT_RX_DISABLED:
        s_rx_disabled = true;
        s_have_cur = false;
        break;
    default:
        break;
    }
}

static nrf_uarte_baudrate_t baud_reg(uint32_t baud)
{
    switch (baud) {
    case 9600:   return NRF_UARTE_BAUDRATE_9600;
    case 19200:  return NRF_UARTE_BAUDRATE_19200;
    case 38400:  return NRF_UARTE_BAUDRATE_38400;
    case 57600:  return NRF_UARTE_BAUDRATE_57600;
    case 230400: return NRF_UARTE_BAUDRATE_230400;
    case 460800: return NRF_UARTE_BAUDRATE_460800;
    case 921600: return NRF_UARTE_BAUDRATE_921600;
    default:     return NRF_UARTE_BAUDRATE_115200;
    }
}

static volatile bool s_rail_up;
static volatile uint32_t s_gen;     /* bumped on every rail change */

/* -- byte counter -------------------------------------------------------------- */

static uint32_t count_evt(void)
{
    return nrf_uarte_event_address_get(GNSS_UARTE, NRF_UARTE_EVENT_RXDRDY);
}

static uint32_t count_tsk(void)
{
    return nrf_timer_task_address_get(GNSS_COUNTER, NRF_TIMER_TASK_COUNT);
}

/* The UARTE raises RXDRDY for every byte it receives; routed to the timer's
 * COUNT task, the timer holds the number of bytes received since this call. */
static void count_start(void)
{
    s_count_ok = false;
    nrf_timer_task_trigger(GNSS_COUNTER, NRF_TIMER_TASK_STOP);
    nrf_timer_task_trigger(GNSS_COUNTER, NRF_TIMER_TASK_CLEAR);
    nrf_timer_mode_set(GNSS_COUNTER, NRF_TIMER_MODE_COUNTER);
    nrf_timer_bit_width_set(GNSS_COUNTER, NRF_TIMER_BIT_WIDTH_32);
    if (nrfx_gppi_conn_alloc(count_evt(), count_tsk(), &s_count_h) < 0) {
        LOG_WRN("no DPPI channel for the byte counter — the end of each burst is read "
                "out by cutting reception short instead, which can cost a byte");
        return;
    }
    nrfx_gppi_conn_enable(s_count_h);
    nrf_timer_task_trigger(GNSS_COUNTER, NRF_TIMER_TASK_START);
    s_count_ok = true;
}

static void count_stop(void)
{
    if (s_count_ok) {
        nrfx_gppi_conn_disable(s_count_h);
        nrfx_gppi_conn_free(count_evt(), count_tsk(), s_count_h);
        s_count_ok = false;
    }
    nrf_timer_task_trigger(GNSS_COUNTER, NRF_TIMER_TASK_STOP);
    nrf_timer_task_trigger(GNSS_COUNTER, NRF_TIMER_TASK_CLEAR);
}

static void uart_close_locked(void)
{
    if (s_open) {
        count_stop();
        nrfx_uarte_uninit(&s_uarte);    /* stops RX and TX, returns the pins */
        s_open = false;
    }
}

static void uart_close(void)
{
    k_mutex_lock(&s_lock, K_FOREVER);
    uart_close_locked();
    k_mutex_unlock(&s_lock);
}

/* (Re)open the UART at `baud`.  -ENODEV if the rail is down: the pins are
 * parked then and must not be driven. */
static int uart_open(uint32_t baud)
{
    nrfx_uarte_config_t cfg = NRFX_UARTE_DEFAULT_CONFIG(
        NRF_GPIO_PIN_MAP(0, PIN_GNSS_TX), NRF_GPIO_PIN_MAP(0, PIN_GNSS_RX));
    int err = -ENODEV;

    cfg.baudrate = baud_reg(baud);
    cfg.interrupt_priority = GNSS_UARTE_IRQ_PRIO;

    k_mutex_lock(&s_lock, K_FOREVER);
    uart_close_locked();
    if (!s_rail_up) {
        goto out;
    }

    s_dma_idx = 0;
    s_rx_disabled = false;
    s_done_total = 0;
    s_deliv_total = 0;
    s_have_cur = false;
    ring_buf_reset(&gnss_rx_ring);
    k_sem_reset(&s_rx_sem);

    err = nrfx_uarte_init(&s_uarte, &cfg, uarte_evt);
    if (err) {
        LOG_ERR("UARTE init at %u baud: %d", (unsigned)baud, err);
        goto out;
    }
    count_start();
    err = nrfx_uarte_rx_enable(&s_uarte, NRFX_UARTE_RX_ENABLE_CONT);
    if (err) {
        LOG_ERR("UARTE rx enable: %d", err);
        count_stop();
        nrfx_uarte_uninit(&s_uarte);
        goto out;
    }
    s_open = true;
    s_baud = baud;
out:
    k_mutex_unlock(&s_lock);
    return err;
}

static int uart_send(const uint8_t *data, size_t len)
{
    int err = -ENODEV;

    if (len > sizeof(s_tx)) {
        return -EMSGSIZE;
    }
    k_mutex_lock(&s_lock, K_FOREVER);
    if (s_open) {
        memcpy(s_tx, data, len);
        err = nrfx_uarte_tx(&s_uarte, s_tx, len, NRFX_UARTE_TX_BLOCKING);
    }
    k_mutex_unlock(&s_lock);
    return err;
}

/* The end of a burst: bytes that have arrived in the buffer the DMA is still
 * filling.  The counter says how many there are; once it has stood still from
 * one look to the next the line is idle and the DMA has caught up, so they are
 * copied out where they lie and the buffer carries on.  The interrupt takes the
 * rest of it when it fills (RX_DONE skips what is already in the ring).
 *
 * Left to the interrupt: a buffer that has just filled, where ENDRX is still
 * pending and the count may already include bytes of the next buffer, and one
 * that is about to. */
static void rx_flush_counted(void)
{
    static uint32_t last_count;
    uint32_t put = 0;
    unsigned int key = irq_lock();

    if (s_have_cur && !nrf_uarte_event_check(GNSS_UARTE, NRF_UARTE_EVENT_ENDRX)) {
        nrf_timer_task_trigger(GNSS_COUNTER, NRF_TIMER_TASK_CAPTURE0);

        const uint32_t count = nrf_timer_cc_get(GNSS_COUNTER, NRF_TIMER_CC_CHANNEL0);
        const uint32_t off = s_deliv_total - s_done_total;      /* already in the ring */
        const uint32_t n = count - s_deliv_total;               /* arrived, not yet */

        if (count == last_count && n > 0 && off < RX_DMA_LEN && n < RX_DMA_LEN - off &&
            !nrf_uarte_event_check(GNSS_UARTE, NRF_UARTE_EVENT_ENDRX)) {
            put = ring_buf_put(&gnss_rx_ring, &s_dma[s_cur][off], n);
            if (put < n) {
                atomic_add(&s_rx_drop, (atomic_val_t)(n - put));
            }
            s_deliv_total = count;
        }
        last_count = count;
    }
    irq_unlock(key);

    if (put) {
        atomic_inc(&s_flushes);
        atomic_add(&s_rx_in, (atomic_val_t)put);
        k_sem_give(&s_rx_sem);
    }
}

/* Without the counter: RXDRDY latches whenever a byte arrives, so a look with
 * none after a look with some means the burst has ended, and aborting the
 * buffer (the next one carries on) hands over what it holds.  The receiver is
 * off for a few byte times while it restarts. */
static void rx_flush_aborting(void)
{
    static bool active;

    if (nrfx_uarte_rx_new_data_check(&s_uarte)) {
        active = true;
    } else if (active) {
        active = false;
        atomic_inc(&s_flushes);
        (void)nrfx_uarte_rx_abort(&s_uarte, false, false);
    }
}

static void rx_idle_flush(void)
{
    static int64_t last_ms;
    int64_t now = k_uptime_get();

    /* Idleness is judged over a stretch of time, not between two calls: a
     * burst wakes the thread from the interrupt as well as from its tick, and
     * two looks microseconds apart can find no new byte mid-burst. */
    if (now - last_ms < TICK_MS / 2) {
        return;
    }
    last_ms = now;
    if (k_mutex_lock(&s_lock, K_NO_WAIT) != 0) {
        return;
    }
    if (s_open) {
        if (s_count_ok) {
            rx_flush_counted();
        } else {
            rx_flush_aborting();
        }
    }
    k_mutex_unlock(&s_lock);
}

/* The receiver stopped without being asked to (the driver ends reception if a
 * buffer is ever late): start it again. */
static void rx_recover(void)
{
    if (!s_rx_disabled) {
        return;
    }
    k_mutex_lock(&s_lock, K_FOREVER);
    if (s_open && s_rx_disabled) {
        s_rx_disabled = false;
        int err = nrfx_uarte_rx_enable(&s_uarte, NRFX_UARTE_RX_ENABLE_CONT);

        if (atomic_inc(&s_rx_restarts) < 3) {
            LOG_WRN("UART receive had stopped — restarted (%d)", err);
        }
    }
    k_mutex_unlock(&s_lock);
}

/* -- receiver state --------------------------------------------------------------- */

static K_SEM_DEFINE(s_fix_sem, 0, 1);
static K_SEM_DEFINE(s_wake_sem, 0, 1);

static struct ubx_parser s_parser;
static uint32_t s_events;           /* valid frames and sentences, any kind */
static int64_t  s_last_pvt_ms;

/* The latest fix, and the uptime the receiver delivered it at: the fix's epoch
 * for anything that has to line it up with something else measured on the
 * uptime clock (the dead reckoning). */
static struct gnss_fix s_fix;
static int64_t s_pvt_ms;
static volatile bool s_have_fix;
static int64_t s_start_ms;
static uint32_t s_pub_itow;
static long s_hdop_x10;
static uint32_t s_dop_itow;

/* A fix is announced to gnss_collect() once the DOP of its own epoch has been
 * folded in, so what the caller copies has the right HDOP and not the last
 * epoch's.  DOP follows PVT by a couple of milliseconds, in no guaranteed
 * order; if it never comes the fix goes out anyway after DOP_WAIT_MS. */
#define DOP_WAIT_MS 40
static bool s_give_pending;
static int64_t s_give_at_ms;
static bool s_log_fix;

static int s_tracked_sv;
static int s_best_cno;
static int s_search_epochs;

static volatile bool s_dead;        /* bring-up failed repeatedly this power-up */
static bool s_dead_alerted;
static volatile bool s_dead_alert_due;
static int s_fails;

/* The receiver took no configuration and is being read through its NMEA
 * (APP_GNSS_EXT_NMEA_FALLBACK).  Set and cleared by the thread, read by the
 * callers for the log and the alert. */
static volatile bool s_nmea_mode;
static bool s_nmea_alerted;
/* Fixes per second asked of the receiver (gnss_set_rate_hz), applied by the
 * thread: at once if it is running, and in configure() at every bring-up. */
static volatile int s_rate_hz = 1;
static volatile bool s_rate_dirty;
static struct nmea_decoder s_nmea;
static uint32_t s_nmea_epoch;       /* numbers the epochs; stands where NAV-PVT has iTOW */

static int8_t  s_ack_result;        /* 0 waiting, 1 ACK, -1 NAK */
static uint8_t s_ack_cls, s_ack_id;
static bool s_ver_logged;
static char s_ver[64];      /* module name and software version, 30 + 1 + 30 at most */

static int32_t s_last_wait_ms;

#if IS_ENABLED(CONFIG_APP_ASSISTNOW)
/* AssistNow (assistnow.c).  The receiver's replies to the chip id and version
 * polls are kept as received, header and checksum and all, because registering
 * a unit sends them verbatim.  Written by the receiver's thread, read by
 * assistnow.c's, under the mutex. */
static K_MUTEX_DEFINE(s_ids_lock);
static uint8_t s_uniqid_frame[24];
static size_t  s_uniqid_len;
static uint8_t s_monver_frame[400];
static size_t  s_monver_len;

static atomic_t s_mga_ok;                   /* UBX-MGA-ACK: taken */
static atomic_t s_mga_bad;                  /* ... not used */
static volatile uint8_t s_mga_first_info;   /* why the first one was not */

static volatile bool s_configured;          /* the UBX configuration was acknowledged this power-up */
static volatile bool s_assist_asked;        /* assistance was requested this power-up */
static volatile bool s_assisted;            /* ... and given, or given up on */
static int s_sat_epochs;                    /* satellite lists since the configuration */
#endif

static void fix_ready(void)
{
    s_give_pending = false;
    if (s_log_fix) {
        s_log_fix = false;
        LOG_INF("fix %lld ms after start: %ld SVs, hdop %ld.%ld",
                s_pvt_ms - s_start_ms, s_fix.sats, s_fix.hdop_x10 / 10,
                s_fix.hdop_x10 % 10);
    }
    k_sem_give(&s_fix_sem);
}

static void publish(const struct gnss_fix *f, int64_t now)
{
    unsigned int key = irq_lock();

    s_fix = *f;
    s_pvt_ms = now;
    g_gnss = *f;
    irq_unlock(key);

    if (!s_have_fix) {
        s_have_fix = true;
        s_log_fix = true;
    }
    if (s_dop_itow == s_pub_itow) {
        fix_ready();
    } else {
        s_give_pending = true;
        s_give_at_ms = now + DOP_WAIT_MS;
    }
}

/* A position, from NAV-PVT or from the NMEA the receiver sends by itself, as
 * the fix the rest of the firmware reads.  `epoch` numbers it for the DOP
 * pairing in publish(): NAV-PVT's iTOW, or the count of NMEA epochs. */
static void publish_pvt(const struct ubx_pvt *v, uint32_t epoch, int64_t now)
{
    struct gnss_fix f = {0};

    f.valid = true;
    f.fix_uptime_ms = now;
    f.epoch_ms = now;
    ubx_fmt_deg6(v->lat_e7, f.lat_str, sizeof(f.lat_str));
    ubx_fmt_deg6(v->lon_e7, f.lon_str, sizeof(f.lon_str));
    f.speed_kmh = v->gspeed_mms * 0.0036f;
    /* Above the ellipsoid, as the built-in receiver reports it, so altitudes
     * stay comparable across boards; hMSL would read 45 m lower in England. */
    f.altitude_m = v->height_mm * 0.001f;
    f.heading_deg = v->head_mot_e5 * 1e-5f;
    f.sats = v->num_sv;
    f.hdop_x10 = s_hdop_x10;
    ubx_fmt_time(v, f.time_iso, sizeof(f.time_iso));
    s_pub_itow = epoch;
    s_search_epochs = 0;
    publish(&f, now);
}

static void on_pvt(const uint8_t *pl, uint16_t len)
{
    struct ubx_pvt v;

    if (!ubx_decode_nav_pvt(pl, len, &v)) {
        return;
    }

    int64_t now = k_uptime_get();

    s_last_pvt_ms = now;
    if (!(v.fix_ok && v.fix_type >= 2 && v.num_sv > 0)) {
        return;
    }
    publish_pvt(&v, v.itow_ms, now);
}

/* DOP follows PVT within a couple of milliseconds and in no guaranteed order,
 * so a fix is published with the last HDOP seen and corrected when this
 * epoch's arrives. */
static void on_dop(const uint8_t *pl, uint16_t len)
{
    struct ubx_dop d;

    if (!ubx_decode_nav_dop(pl, len, &d)) {
        return;
    }
    s_hdop_x10 = (d.hdop_e2 + 5) / 10;
    s_dop_itow = d.itow_ms;
    if (s_have_fix && d.itow_ms == s_pub_itow) {
        unsigned int key = irq_lock();

        s_fix.hdop_x10 = s_hdop_x10;
        g_gnss.hdop_x10 = s_hdop_x10;
        irq_unlock(key);
        if (s_give_pending) {
            fix_ready();
        }
    }
}

static void on_sat(const uint8_t *pl, uint16_t len)
{
    struct ubx_sat_summary s;

    if (!ubx_decode_nav_sat(pl, len, &s)) {
        return;
    }
    s_tracked_sv = s.tracked;
    s_best_cno = s.best_cno;

    /* The built-in receiver logged this every epoch; the line that tells a
     * bad antenna from a bad sky is the same one, every fifth is enough. */
    if (!s_have_fix && (s_search_epochs++ % 5) == 0) {
        LOG_INF("searching: %d SVs tracked, best cn0=%d dBHz", s_tracked_sv, s_best_cno);
    }

#if IS_ENABLED(CONFIG_APP_ASSISTNOW)
    /* Asked for once per power-up, and only by a receiver that holds hardly
     * any ephemerides and has not found its position on its own.  A hot start
     * (the backup supply kept them) needs nothing, and orbit data sent to a
     * receiver that has fresher would only replace them.  A few lists are let
     * go by first: the first after the configuration may not have the
     * receiver's own database in it yet. */
    s_sat_epochs++;
    if (s_configured && !s_nmea_mode && !s_have_fix && !s_assist_asked &&
        s_sat_epochs >= ASSIST_EPOCHS && s.eph < ASSIST_EPH_MIN) {
        s_assist_asked = true;
        LOG_INF("assistance wanted: ephemerides for %u of %u satellites, almanac for %u",
                s.eph, s.listed, s.alm);
        assistnow_request();
    }
#endif
}

/* One sentence of the receiver's own output, once it has refused configuration
 * (s_nmea_mode).  RMC is the heartbeat, as NAV-PVT is on the UBX path.  A
 * position goes out when its RMC and GGA are both in, carrying that GGA's
 * HDOP, so unlike the UBX path there is no DOP still to come.  The satellite
 * summary is the GSV of the epoch before, logged as NAV-SAT's is. */
static void on_nmea(const struct ubx_parser *p)
{
    unsigned f = nmea_feed(&s_nmea, (const char *)p->payload, p->len);
    int64_t now = k_uptime_get();

    if (f & NMEA_F_RMC) {
        s_last_pvt_ms = now;
    }
    if (f & NMEA_F_SATS) {
        s_tracked_sv = s_nmea.sats.tracked;
        s_best_cno = s_nmea.sats.best_cno;
        if (!s_have_fix && (s_search_epochs++ % 5) == 0) {
            LOG_INF("searching: %d SVs tracked, best cn0=%d dBHz", s_tracked_sv, s_best_cno);
        }
    }
    if (f & NMEA_F_EPOCH) {
        struct ubx_pvt v;
        uint16_t hdop_e2;

        if (nmea_epoch_pvt(&s_nmea, &v, &hdop_e2)) {
            s_hdop_x10 = (hdop_e2 + 5) / 10;
            s_nmea_epoch++;
            s_dop_itow = s_nmea_epoch;      /* this epoch's HDOP is already in */
            publish_pvt(&v, s_nmea_epoch, now);
        }
    }
}

static void on_mon_ver(const uint8_t *pl, uint16_t len)
{
    char sw[31], hw[11], mod[31] = "";

    if (len < 40) {
        return;
    }
    memcpy(sw, pl, 30);
    sw[30] = '\0';
    memcpy(hw, pl + 30, 10);
    hw[10] = '\0';
    for (uint16_t off = 40; off + 30 <= len; off += 30) {
        if (memcmp(pl + off, "MOD=", 4) == 0) {
            memcpy(mod, pl + off + 4, 26);
            mod[26] = '\0';
        }
    }
    snprintf(s_ver, sizeof(s_ver), "%s %s", mod[0] ? mod : "u-blox", sw);
    if (!s_ver_logged) {
        s_ver_logged = true;
        LOG_INF("receiver %s, hardware %s", s_ver, hw);
    }
}

#if IS_ENABLED(CONFIG_APP_ASSISTNOW)
/* A reply kept as it came: the frame is rebuilt from its payload, which gives
 * back the same bytes.  0 length if it does not fit. */
static void keep_frame(uint8_t *dst, size_t cap, size_t *len, const struct ubx_parser *p)
{
    k_mutex_lock(&s_ids_lock, K_FOREVER);
    *len = ubx_build_frame(dst, cap, p->cls, p->id, p->payload, p->len);
    k_mutex_unlock(&s_ids_lock);
}

static void on_mga_ack(const uint8_t *pl, uint16_t len)
{
    struct ubx_mga_ack a;

    if (!ubx_decode_mga_ack(pl, len, &a)) {
        return;
    }
    if (a.accepted) {
        atomic_inc(&s_mga_ok);
    } else if (atomic_inc(&s_mga_bad) == 0) {
        s_mga_first_info = a.info;
    }
}
#endif

static void on_frame(const struct ubx_parser *p)
{
    if (p->cls == UBX_CLS_NAV) {
        switch (p->id) {
        case UBX_ID_NAV_PVT: on_pvt(p->payload, p->len); break;
        case UBX_ID_NAV_DOP: on_dop(p->payload, p->len); break;
        case UBX_ID_NAV_SAT: on_sat(p->payload, p->len); break;
        default: break;
        }
    } else if (p->cls == UBX_CLS_ACK) {
        uint8_t cls, id;

        if (ubx_decode_ack(p->payload, p->len, &cls, &id) &&
            cls == s_ack_cls && id == s_ack_id) {
            s_ack_result = (p->id == UBX_ID_ACK_ACK) ? 1 : -1;
        }
    } else if (p->cls == UBX_CLS_MON && p->id == UBX_ID_MON_VER) {
        on_mon_ver(p->payload, p->len);
#if IS_ENABLED(CONFIG_APP_ASSISTNOW)
        keep_frame(s_monver_frame, sizeof(s_monver_frame), &s_monver_len, p);
    } else if (p->cls == UBX_CLS_SEC && p->id == UBX_ID_SEC_UNIQID) {
        keep_frame(s_uniqid_frame, sizeof(s_uniqid_frame), &s_uniqid_len, p);
    } else if (p->cls == UBX_CLS_MGA && p->id == UBX_ID_MGA_ACK) {
        on_mga_ack(p->payload, p->len);
#endif
    }
}

/* Parse what the interrupt has queued. */
static void drain(void)
{
    uint8_t chunk[64];
    uint32_t n;

    while ((n = ring_buf_get(&gnss_rx_ring, chunk, sizeof(chunk))) > 0) {
        s_rx_bytes += n;
        for (uint32_t i = 0; i < n; i++) {
            enum ubx_event ev = ubx_parse_byte(&s_parser, chunk[i]);

            if (ev == UBX_EV_FRAME) {
                s_events++;
                on_frame(&s_parser);
            } else if (ev == UBX_EV_NMEA) {
                s_events++;
                if (s_nmea_mode) {
                    on_nmea(&s_parser);
                }
            }
        }
    }
}

static inline bool powered(uint32_t gen)
{
    return s_rail_up && gen == s_gen;
}

/* One tick of receive housekeeping, at most TICK_MS long. */
static void pump_once(void)
{
    (void)k_sem_take(&s_rx_sem, K_MSEC(TICK_MS));
    drain();
    rx_idle_flush();
    rx_recover();
    if (s_give_pending && k_uptime_get() >= s_give_at_ms) {
        fix_ready();
    }
}

/* -- bring-up ------------------------------------------------------------------- */

/* True once anything valid has come in, UBX or NMEA, within `ms`. */
static bool wait_alive(uint32_t gen, int ms)
{
    uint32_t before = s_events;
    int64_t end = k_uptime_get() + ms;

    while (k_uptime_get() < end && powered(gen)) {
        pump_once();
        if (s_events != before) {
            return true;
        }
    }
    return false;
}

/* 0 on ACK, -EIO on NAK, -ETIMEDOUT. */
static int wait_ack(uint32_t gen, uint8_t cls, uint8_t id, int ms)
{
    int64_t end = k_uptime_get() + ms;

    s_ack_cls = cls;
    s_ack_id = id;
    s_ack_result = 0;
    while (k_uptime_get() < end && powered(gen)) {
        pump_once();
        if (s_ack_result) {
            return s_ack_result > 0 ? 0 : -EIO;
        }
    }
    return -ETIMEDOUT;
}

static int send_poll(uint8_t cls, uint8_t id)
{
    uint8_t f[UBX_FRAME_OVERHEAD];
    size_t n = ubx_build_poll(f, sizeof(f), cls, id);

    return uart_send(f, n);
}

/* Open the UART at each rate the receiver might be using until something
 * valid comes back.  The poll gets an answer from a receiver that is idle or
 * already on UBX; the NMEA a fresh one is sending by itself does for the
 * rest.  A receiver is slow to start after its rail rises, so this keeps
 * cycling for FIND_MS.
 *
 * The poll is NAV-STATUS because its reply is 24 bytes.  MON-VER, which says
 * what the receiver is, is 228: at 9600 baud that is 237 ms on the wire, so it
 * never finished inside a PROBE_MS window, and a receiver that was answering
 * was passed over for the next rate (found on the bench's stand-in, which
 * answers it just as slowly as the part would). */
static int find_receiver(uint32_t gen)
{
    /* A MAX-M10S comes up at 38400 (as the v4.0 board's does), so that first;
     * then where this puts it, for a receiver that outlived a restart of the
     * firmware; then 9600, which older u-blox parts start at. */
    const uint32_t wanted[] = { 38400, GNSS_BAUD, 9600 };
    uint32_t rates[ARRAY_SIZE(wanted)];
    size_t nrates = 0;
    int64_t end = k_uptime_get() + FIND_MS;

    for (size_t i = 0; i < ARRAY_SIZE(wanted); i++) {
        bool seen = false;

        for (size_t k = 0; k < nrates; k++) {
            seen |= (rates[k] == wanted[i]);
        }
        if (!seen) {
            rates[nrates++] = wanted[i];
        }
    }

    while (k_uptime_get() < end && powered(gen)) {
        for (size_t i = 0; i < nrates && powered(gen); i++) {
            if (uart_open(rates[i])) {
                return -ENODEV;
            }
            ubx_parser_reset(&s_parser);
            (void)send_poll(UBX_CLS_NAV, UBX_ID_NAV_STATUS);
            if (wait_alive(gen, PROBE_MS)) {
                LOG_INF("receiver answers at %u baud", (unsigned)s_baud);
                return 0;
            }
        }
    }
    return powered(gen) ? -ETIMEDOUT : -ENODEV;
}

/* Move the receiver to the configured rate.  Not fatal if it will not go: it
 * is used at the rate it answers on.
 *
 * It does not re-clock at once.  The v4.0 board's MAX-M10S, asked 100 ms
 * earlier, still answered nothing at the new rate, and was sending at it by its
 * next NMEA burst, a second after the command.  A half-second wait gave up on
 * it, went back to the old rate and configured a receiver that was no longer
 * there.  So it is asked again every PROBE_MS for SWITCH_MS. */
static void switch_baud(uint32_t gen)
{
    uint32_t old = s_baud;
    uint8_t f[24];

    if (old == GNSS_BAUD) {
        return;
    }

    const struct ubx_cfg_item item = { UBX_KEY_UART1_BAUDRATE, GNSS_BAUD };
    size_t n = ubx_build_valset(f, sizeof(f), UBX_LAYER_RAM, &item, 1);

    if (uart_send(f, n)) {
        return;
    }
    /* Let it finish the acknowledgement at the old rate. */
    k_msleep(100);
    if (uart_open(GNSS_BAUD) == 0) {
        const int64_t asked = k_uptime_get(), end = asked + SWITCH_MS;

        ubx_parser_reset(&s_parser);
        while (powered(gen) && k_uptime_get() < end) {
            (void)send_poll(UBX_CLS_NAV, UBX_ID_NAV_STATUS);
            if (wait_alive(gen, PROBE_MS)) {
                LOG_INF("receiver moved to %u baud after %d ms", (unsigned)s_baud,
                        (int)(k_uptime_get() - asked) + 100);
                return;
            }
        }
    }
    if (powered(gen) && uart_open(old) == 0) {
        LOG_WRN("receiver stayed at %u baud", (unsigned)old);
    }
}

static int configure(uint32_t gen)
{
    /* The satellite list is a few hundred bytes: once a second only if the
     * link has the room. */
    const int hz = s_rate_hz;
    const uint32_t sat_every = ((s_baud >= 115200) ? 1 : 5) * (uint32_t)hz;
    const struct ubx_cfg_item cfg[] = {
        { UBX_KEY_UART1INPROT_UBX,      1 },
        { UBX_KEY_UART1OUTPROT_UBX,     1 },
        { UBX_KEY_UART1OUTPROT_NMEA,    0 },
        { UBX_KEY_RATE_MEAS,            (uint32_t)(1000 / hz) },
        { UBX_KEY_RATE_NAV,             1 },
        { UBX_KEY_NAVSPG_DYNMODEL,      UBX_DYNMODEL_AUTOMOTIVE },
        { UBX_KEY_MSGOUT_NAV_PVT_UART1, 1 },
        { UBX_KEY_MSGOUT_NAV_DOP_UART1, 1 },
        { UBX_KEY_MSGOUT_NAV_SAT_UART1, sat_every },
        /* An UBX-MGA-ACK for every assistance message (AssistNow), so what
         * reached the receiver is known.  It sends none for anything else. */
        { UBX_KEY_NAVSPG_ACKAIDING,     1 },
    };
    uint8_t f[96];
    size_t n = ubx_build_valset(f, sizeof(f), UBX_LAYER_RAM, cfg, ARRAY_SIZE(cfg));
    int err = -ETIMEDOUT;

    if (n == 0) {
        return -EMSGSIZE;
    }
    for (int attempt = 0; attempt < 3 && powered(gen); attempt++) {
        const uint32_t bytes0 = s_rx_bytes, events0 = s_events;
        const int in0 = (int)atomic_get(&s_rx_in), bufs0 = (int)atomic_get(&s_isr_bufs);
        const int late0 = (int)atomic_get(&s_rx_late), err0 = (int)atomic_get(&s_rx_err);
        const int refused0 = (int)atomic_get(&s_buf_refused);

        err = uart_send(f, n);
        if (err) {
            return err;
        }
        err = wait_ack(gen, UBX_CLS_CFG, UBX_ID_CFG_VALSET, ACK_MS);
        if (err == 0) {
            return 0;
        }
        /* What came back says where it went wrong: nothing at all is the
         * wire or the baud rate, bytes without frames is noise or a bad
         * rate, frames without the ACK is a receiver that talks but will
         * not take the configuration. */
        LOG_WRN("configuration %s: heard %u bytes (%d queued, %d buffers filled) and %u frames "
                "in %d ms [%d late, %d refused, %d UART errors]",
                err == -EIO ? "refused (NAK)" : "not acknowledged",
                (unsigned)(s_rx_bytes - bytes0), (int)atomic_get(&s_rx_in) - in0,
                (int)atomic_get(&s_isr_bufs) - bufs0, (unsigned)(s_events - events0), ACK_MS,
                (int)atomic_get(&s_rx_late) - late0, (int)atomic_get(&s_buf_refused) - refused0,
                (int)atomic_get(&s_rx_err) - err0);
    }
    return err;
}

/* Change how often the receiver fixes, with the configuration already in.
 * The satellite list is kept at about one a second whatever the rate. */
static void apply_rate(uint32_t gen)
{
    const int hz = s_rate_hz;
    const struct ubx_cfg_item cfg[] = {
        { UBX_KEY_RATE_MEAS,            (uint32_t)(1000 / hz) },
        { UBX_KEY_RATE_NAV,             1 },
        { UBX_KEY_MSGOUT_NAV_SAT_UART1, ((s_baud >= 115200) ? 1u : 5u) * (uint32_t)hz },
    };
    uint8_t f[48];
    size_t n = ubx_build_valset(f, sizeof(f), UBX_LAYER_RAM, cfg, ARRAY_SIZE(cfg));

    if (n == 0 || uart_send(f, n)) {
        return;
    }
    if (wait_ack(gen, UBX_CLS_CFG, UBX_ID_CFG_VALSET, ACK_MS) != 0) {
        LOG_WRN("receiver did not acknowledge %d fixes a second", hz);
    }
}

/* The receiver is heard but takes no configuration: read the NMEA it sends by
 * itself.  Its positions are worth more than the alert that it is dead, which
 * it is not; the alert says what is wrong instead. */
static void start_nmea_mode(void)
{
    nmea_reset(&s_nmea);
    s_search_epochs = 0;
    snprintf(s_ver, sizeof(s_ver), "NMEA only, takes no commands");
    s_nmea_mode = true;
    LOG_WRN("receiver takes no configuration: reading its NMEA at %u baud "
            "(is the nRF's TX reaching it?)", (unsigned)s_baud);
}

static int bring_up(uint32_t gen)
{
    int err = -ETIMEDOUT;
    bool talking = false;

    s_ver_logged = false;
    s_nmea_mode = false;
#if IS_ENABLED(CONFIG_APP_ASSISTNOW)
    s_configured = false;
#endif

    /* Twice at most.  A receiver can re-clock under the configuration (the
     * bench's did, a moment after switch_baud stopped waiting), and what is
     * then heard at the old rate is noise, which is not a receiver that will
     * not listen: find it again before deciding that. */
    for (int pass = 0; pass < 2 && powered(gen); pass++) {
        err = find_receiver(gen);
        if (err) {
            return err;
        }
        switch_baud(gen);

        /* NMEA heard while the configuration goes in shows the receiver is
         * talking at this rate, whatever it then does with what it is told. */
        const uint32_t nmea0 = s_parser.nmea;

        err = configure(gen);
        talking = (s_parser.nmea != nmea0);
        if (err == 0 || talking) {
            break;
        }
        if (pass == 0 && powered(gen)) {
            LOG_WRN("nothing valid at %u baud while configuring — finding the receiver again",
                    (unsigned)s_baud);
        }
    }
    if (err) {
        if (IS_ENABLED(CONFIG_APP_GNSS_EXT_NMEA_FALLBACK) && powered(gen) && talking) {
            start_nmea_mode();
            return 0;
        }
        return err;
    }
    LOG_INF("receiver configured: %u baud, NAV-PVT/DOP/SAT every second",
            (unsigned)s_baud);
    /* What it is, for the log and the board test.  The reply is read with the
     * positions; the receiver is configured whether or not it comes. */
    (void)send_poll(UBX_CLS_MON, UBX_ID_MON_VER);
#if IS_ENABLED(CONFIG_APP_ASSISTNOW)
    (void)send_poll(UBX_CLS_SEC, UBX_ID_SEC_UNIQID);
    s_sat_epochs = 0;
    s_configured = true;
#endif
    return 0;
}

/* Parse until the rail drops or the receiver goes quiet.  0 if it had been
 * sending positions; -ENODATA if it took the configuration and never sent one,
 * which is as much a failure as not answering. */
static int run(uint32_t gen)
{
    const int64_t started = k_uptime_get();
    const int err0 = (int)atomic_get(&s_rx_err), drop0 = (int)atomic_get(&s_rx_drop);
    const uint32_t cks0 = s_parser.bad_cks;
    bool seen = false;

    s_last_pvt_ms = started;
    while (powered(gen)) {
        pump_once();
        if (s_rate_dirty) {
            s_rate_dirty = false;
            apply_rate(gen);
        }
        seen |= (s_last_pvt_ms != started);

        if (k_uptime_get() - s_last_pvt_ms > SILENT_MS) {
            LOG_WRN("no position from the receiver for %d s — finding it again "
                    "(since it started: %d bad checksums, %d UART errors, %d bytes dropped)",
                    SILENT_MS / 1000, (int)(s_parser.bad_cks - cks0),
                    (int)atomic_get(&s_rx_err) - err0, (int)atomic_get(&s_rx_drop) - drop0);
            return seen ? 0 : -ENODATA;
        }
    }
    return 0;
}

static void gnss_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    for (;;) {
        k_sem_take(&s_wake_sem, K_FOREVER);

        while (s_rail_up) {
            uint32_t gen = s_gen;
            int err = bring_up(gen);

            if (err == 0) {
                s_dead = false;
                err = run(gen);
                if (err == 0) {
                    s_fails = 0;
                    continue;
                }
            }
            if (!powered(gen)) {
                break;
            }
            LOG_WRN("receiver bring-up failed: %d", err);
            if (++s_fails >= DEAD_AFTER_FAILS) {
                s_dead = true;
                s_dead_alert_due = true;
            }
            for (int i = 0; i < RETRY_PAUSE_MS / 100 && powered(gen); i++) {
                k_msleep(100);
            }
        }
    }
}

/* -- the rail ------------------------------------------------------------------- */

/* Called by hw_domain: with the rail up and the pins released, and just before
 * the pins are parked and the rail dropped.  Runs on whichever thread asked
 * for the rail, so up only wakes the thread, and down closes the UART itself
 * before the pins go. */
static void rail_notify(bool up)
{
    if (up) {
        s_gen++;
        s_fails = 0;
        s_dead = false;
        s_nmea_mode = false;
        s_tracked_sv = 0;
        s_best_cno = 0;
        s_rate_hz = 1;
        s_rate_dirty = false;
#if IS_ENABLED(CONFIG_APP_ASSISTNOW)
        s_configured = false;
        s_assist_asked = false;
        s_assisted = false;
        s_sat_epochs = 0;
        k_mutex_lock(&s_ids_lock, K_FOREVER);
        s_uniqid_len = 0;
        s_monver_len = 0;
        k_mutex_unlock(&s_ids_lock);
#endif
        s_rail_up = true;
        k_sem_give(&s_wake_sem);
        return;
    }
    s_rail_up = false;
#if IS_ENABLED(CONFIG_APP_ASSISTNOW)
    s_configured = false;
#endif
    s_gen++;
    uart_close();
    k_sem_give(&s_rx_sem);          /* let the thread notice */
}

/* -- the gnss_* interface ------------------------------------------------------------- */

int gnss_init(void)
{
    static struct k_thread thread;
    static K_THREAD_STACK_DEFINE(stack, THREAD_STACK);
    static bool inited;

    if (inited) {
        return 0;
    }
    inited = true;

    IRQ_CONNECT(NRFX_IRQ_NUMBER_GET(GNSS_UARTE), GNSS_UARTE_IRQ_PRIO,
                nrfx_uarte_irq_handler, &s_uarte, 0);
    ubx_parser_reset(&s_parser);
    k_thread_create(&thread, stack, K_THREAD_STACK_SIZEOF(stack), gnss_thread,
                    NULL, NULL, NULL, THREAD_PRIO, 0, K_NO_WAIT);
    /* The rail is usually up already (hw_aux_power_on at boot): this calls
     * back at once if so. */
    hw_domain_notify_set(HW_DOMAIN_AUX, rail_notify);
    LOG_INF("init ok (external receiver on UARTE2: TX P0.%d RX P0.%d)",
            PIN_GNSS_TX, PIN_GNSS_RX);
    return 0;
}

int gnss_start(void)
{
    s_have_fix = false;
    s_start_ms = k_uptime_get();
    k_sem_reset(&s_fix_sem);
    if (!s_rail_up) {
        LOG_WRN("start: the receiver's rail is off");
    }
    return 0;
}

int gnss_stop(void)
{
    return 0;
}

int gnss_resume(void)
{
    k_sem_reset(&s_fix_sem);
    return 0;
}

/* Optional callback run about once a second while gnss_collect() waits; see
 * the note in gnss.c. */
static void (*s_tick_cb)(void);

void gnss_set_tick(void (*cb)(void))
{
    s_tick_cb = cb;
}

/* Nothing here is waiting for the radio. */
uint32_t gnss_blocked_ms(void)
{
    return 0;
}

uint32_t gnss_starved_epochs(void)
{
    return 0;
}

int32_t gnss_last_wait_ms(void)
{
    return s_last_wait_ms;
}

const char *gnss_module_version(void)
{
    return s_ver;
}

int gnss_set_rate_hz(int hz)
{
    if (hz < 1 || hz > 10) {
        return -EINVAL;
    }
    if (s_nmea_mode) {
        return -ENOTSUP;                /* it takes no commands */
    }
    if (hz == s_rate_hz) {
        return 0;
    }
    s_rate_hz = hz;
    s_rate_dirty = true;
    k_sem_give(&s_rx_sem);              /* let the thread look now */
    return 0;
}

/* Once per boot, the first time a caller learns the receiver is not
 * answering, or is answering only in NMEA.  From the callers' side and not the
 * thread's: the alert queue is the main thread's.  The NMEA one is for as long
 * as it is so: a bring-up that fell back and then won on its next pass (the
 * receiver had re-clocked) is not a receiver that takes no commands, and the
 * main thread may not ask until after. */
static void alert_if_dead(void)
{
    if (s_dead_alert_due && !s_dead_alerted) {
        s_dead_alerted = true;
        alert_enqueue("GNSS receiver not responding (UART)",
                      CONFIG_APP_HW_FAULT_PRIORITY);
    }
    if (s_nmea_mode && !s_nmea_alerted) {
        s_nmea_alerted = true;
        /* It is hearing nothing but still sending positions, so a fault to
         * notice and not a loss: normal priority, below the dead alert's. */
        alert_enqueue("GNSS receiver takes no commands, using NMEA (check UART TX)", 0);
    }
}

/* Same contract as gnss.c: -EAGAIN on timeout, -EINTR if the ignition changed
 * (the caller wants to report that, not a position), and in addition -ENODEV
 * when the receiver's rail is off and -EIO when it is not answering, both
 * without waiting out the timeout for a fix that cannot come. */
int gnss_collect(int timeout_ms, struct gnss_fix *out)
{
    const int64_t entered_ms = k_uptime_get();
    bool cold = !s_have_fix;

    out->valid = false;
    if (!s_rail_up) {
        return -ENODEV;
    }
    alert_if_dead();

    if (cold && timeout_ms >= GPS_FIX_TIMEOUT_MS) {
        timeout_ms = GPS_COLD_FIX_TIMEOUT_MS;
        LOG_INF("cold start: timeout extended to %ds", timeout_ms / 1000);
    }

    /* A deadline, not a countdown of the semaphore waits: the tick runs
     * the OBD poll for half a second between them. */
    const int64_t deadline = k_uptime_get() + timeout_ms;
    int64_t last_log_ms = k_uptime_get();
    int err = -EAGAIN;
    const int tick = 1000;

    /* The key turning ends the wait: whatever the caller wanted a fix for,
     * the ignition change is now the thing to report.  A change has to hold
     * before it counts: the car's line drops out while the starter turns, and
     * the backup module's wake pulse holds it on for a moment with the key
     * off. */
    const int ign_at_entry = ignition_read();
    int64_t ign_changed_ms = 0;

    for (;;) {
        int64_t left = deadline - k_uptime_get();

        if (left <= 0) {
            err = -EAGAIN;
            break;
        }
        err = k_sem_take(&s_fix_sem, K_MSEC((int)MIN(left, tick)));
        if (err == 0) {
            break;
        }
        watchdog_kick();

        if (s_tick_cb) {
            s_tick_cb();
        }
        alert_if_dead();            /* bring-up can finish while this waits */

        int64_t now = k_uptime_get();
        int ign = ignition_read();

        if (ign == ign_at_entry) {
            ign_changed_ms = 0;
        } else if (ign_changed_ms == 0) {
            ign_changed_ms = now;
        }
        if (ign_changed_ms != 0 &&
            now - ign_changed_ms >= (ign == 0 ? BACKUP_PULSE_MAX_MS
                                              : IGN_CRANK_MAX_MS)) {
            LOG_INF("ignition %s — fix wait ended after %llds",
                    ign == 0 ? "on" : "off", (now - entered_ms) / 1000);
            err = -EINTR;
            break;
        }
        if (!s_rail_up) {
            err = -ENODEV;
            break;
        }
        if (s_dead) {
            err = -EIO;
            break;
        }

        if (cold && now - last_log_ms >= 30000) {
            last_log_ms = now;
            LOG_INF("cold start: %llds remaining, %d SVs", (deadline - now) / 1000,
                    s_tracked_sv);
        }
    }

    s_last_wait_ms = (int32_t)(k_uptime_get() - entered_ms);

    if (err == -EINTR) {
        /* Not a failed search: the receiver keeps what it has. */
        return err;
    }
    if (err == -EIO) {
        alert_if_dead();
    }
    if (err == -EAGAIN && collect_fix_wait_ms > 0) {
        /* The drive stream's short wait running out is how it goes on without
         * a fix: main.c sends the last position and counts it.  Not a fault to
         * warn about every few seconds, and a warning is sent to the server. */
        return err;
    }
    if (err) {
        LOG_WRN("%s fix %s (%d SVs tracked, best cn0 %d dBHz, %s)",
                cold ? "cold" : "warm",
                err == -EAGAIN ? "timeout" : "wait ended early",
                s_tracked_sv, s_best_cno,
                !s_rail_up ? "rail off" :
                s_dead ? "receiver not answering" :
                s_nmea_mode ? "receiver up, NMEA only" : "receiver up");
        return err;
    }

    unsigned int key = irq_lock();

    *out = s_fix;
    irq_unlock(key);
    out->fix_uptime_ms = k_uptime_get();
    out->epoch_ms = s_pvt_ms;
    return 0;
}

#if IS_ENABLED(CONFIG_APP_ASSISTNOW)

/* -- assistance (assistnow.c) ------------------------------------------------------ */

static size_t hex_upper(char *out, size_t cap, const uint8_t *in, size_t n)
{
    static const char digits[] = "0123456789ABCDEF";

    if (cap < 2 * n + 1) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[2 * n] = '\0';
    return 2 * n;
}

int gnss_ext_ids(char *uid_hex, size_t uid_sz, char *ver_hex, size_t ver_sz)
{
    int err = -EAGAIN;

    k_mutex_lock(&s_ids_lock, K_FOREVER);
    if (s_uniqid_len && s_monver_len) {
        err = (hex_upper(uid_hex, uid_sz, s_uniqid_frame, s_uniqid_len) &&
               hex_upper(ver_hex, ver_sz, s_monver_frame, s_monver_len)) ? 0 : -ENOSPC;
    }
    k_mutex_unlock(&s_ids_lock);
    return err;
}

bool gnss_ext_assist_useful(void)
{
    return s_rail_up && s_configured && !s_nmea_mode && !s_have_fix && !s_assisted;
}

void gnss_ext_assist_done(void)
{
    s_assisted = true;
}

/* What a server may pass to the receiver: the assistance messages and nothing
 * else.  A response that carried a configuration or a reset would otherwise be
 * obeyed, so the whitelist is the ids of the ephemeris and almanac messages of
 * each constellation, AssistNow Offline orbits and the time and position
 * hints; the database dump and flash messages are not on it. */
static bool assist_frame_ok(const uint8_t *f, int n)
{
    if (n > (int)sizeof(s_tx) || f[2] != UBX_CLS_MGA) {
        return false;
    }
    switch (f[3]) {
    case 0x00:                  /* GPS */
    case 0x02:                  /* Galileo */
    case 0x03:                  /* BeiDou */
    case 0x05:                  /* QZSS */
    case 0x06:                  /* GLONASS */
    case UBX_ID_MGA_ANO:
    case UBX_ID_MGA_INI:
        return true;
    default:
        return false;
    }
}

int gnss_ext_assist_inject(const uint8_t *data, size_t len, struct gnss_assist_result *res)
{
    size_t off = 0;
    unsigned sent = 0;
    int err = 0;

    memset(res, 0, sizeof(*res));
    atomic_set(&s_mga_ok, 0);
    atomic_set(&s_mga_bad, 0);
    s_mga_first_info = 0;

    while (off < len) {
        int n = ubx_frame_scan(data + off, len - off);

        if (n == 0) {
            break;                      /* a truncated last frame */
        }
        if (n < 0) {
            off++;                      /* not a frame: on to the next sync pattern */
            continue;
        }
        if (!s_rail_up || !s_configured || s_nmea_mode) {
            err = -ENODEV;
            break;
        }
        if (assist_frame_ok(data + off, n)) {
            if (uart_send(data + off, (size_t)n)) {
                err = -EIO;
                break;
            }
            sent++;
            if (sent % ASSIST_WINDOW == 0) {
                /* Let the receiver catch up: it takes assistance as fast as it
                 * is sent only until its buffers fill. */
                const int64_t until = k_uptime_get() + 300;

                while (k_uptime_get() < until &&
                       (unsigned)(atomic_get(&s_mga_ok) + atomic_get(&s_mga_bad)) + ASSIST_LAG < sent) {
                    k_msleep(5);
                }
            } else {
                k_msleep(2);
            }
        }
        off += (size_t)n;
    }

    const int64_t until = k_uptime_get() + ASSIST_ACK_MS;

    while (k_uptime_get() < until &&
           (unsigned)(atomic_get(&s_mga_ok) + atomic_get(&s_mga_bad)) < sent) {
        k_msleep(10);
    }
    res->sent = (uint16_t)MIN(sent, (unsigned)UINT16_MAX);
    res->accepted = (uint16_t)atomic_get(&s_mga_ok);
    res->refused = (uint16_t)atomic_get(&s_mga_bad);
    res->first_info = s_mga_first_info;
    return err;
}

#endif /* CONFIG_APP_ASSISTNOW */
