/*
 * Software task watchdog, 32 s window, backed by the SoC's hardware watchdog.
 *
 * Both layers have to be asked for explicitly, and this module used to ask
 * for neither:
 *
 *   - task_wdt only configures the hardware fallback when task_wdt_init() is
 *     given a device.  CONFIG_TASK_WDT_HW_FALLBACK=y alone does nothing;
 *     passing NULL leaves the task watchdog as a bare kernel timer.
 *   - task_wdt only reboots on its own for a channel registered with *no*
 *     callback.  A registered callback replaces that reboot, so an empty one
 *     turns the timeout into a no-op.
 *
 * With NULL and an empty callback, as this was, a thread that stopped feeding
 * was never noticed at all: a unit that blocked in a modem call sat dead in a
 * car for hours and only came back when it was power-cycled by hand.
 *
 * The callback deliberately does not reboot.  task_wdt stops rescheduling its
 * timer once a channel has expired, so leaving it alone means the hardware
 * watchdog goes unfed and resets the SoC a couple of seconds later — and that
 * reset is a watchdog reset, which RESETREAS records and dbglog reports as
 * rst=wdt on the next record that reaches the server.  sys_reboot() here
 * would report rst=sw instead, indistinguishable from a commanded reboot, and
 * hide exactly the event worth seeing.  Without a hardware backend there is
 * nothing to fall back on, so then it does reboot.
 *
 * Before it goes, the callback notes where the loop was stuck.  rst=wdt
 * alone said nothing at 14:56 on 2026-10-11: the car reset mid-drive during
 * an operator change, and the warnings that would have explained it were in
 * RAM.  The main thread is blocked at that moment, so its stack holds the
 * return addresses of the calls it is blocked in: the callback takes the
 * first few code addresses found there into noinit RAM, as fatal.c does,
 * and the next boot reports them in rst= ("stall:<addr>/<addr>/...", look
 * them up in the map file).  A scan, not an unwind, so an address can be a
 * stale one left on the stack; the first few are the innermost and the
 * most telling.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/linker/linker-defs.h>
#include <cmsis_core.h>
#include <hal/nrf_wdt.h>
#include <stdio.h>

#include "app.h"
#include "freeze_note.h"

LOG_MODULE_REGISTER(wdt, CONFIG_APP_LOG_LEVEL);

static const struct device *const s_hw = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int  s_channel = -1;
static bool s_hw_backed;
static struct k_thread *s_fed_by;       /* the thread that kicks: main() */

#define STALL_MAGIC 0x57A11ED0u
#define STALL_ADDRS 4               /* rst= is varchar(48) on the server */
#define STALL_SCAN  192                 /* words of stack looked through */

/* __noinit for the same reason as fatal.c's note. */
static __noinit struct {
    uint32_t magic;
    uint32_t addr[STALL_ADDRS];
} s_stall;

/* The return addresses on the stalled thread's stack, innermost first:
 * Thumb code addresses (odd) inside the image's text. */
static void note_stall(void)
{
    struct k_thread *t = s_fed_by;

    s_stall.magic = 0;
    if (t == NULL) {
        return;
    }

    uint32_t *sp = (uint32_t *)t->callee_saved.psp;
    uint32_t *top = (uint32_t *)(t->stack_info.start + t->stack_info.size);
    uint32_t lo = (uint32_t)__text_region_start;
    uint32_t hi = (uint32_t)__text_region_end;
    int n = 0;

    if (sp < (uint32_t *)t->stack_info.start || sp >= top) {
        return;
    }
    for (int i = 0; i < STALL_SCAN && sp + i < top && n < STALL_ADDRS;
         i++) {
        uint32_t w = sp[i];

        if ((w & 1u) && w >= lo && w < hi) {
            s_stall.addr[n++] = w & ~1u;
        }
    }
    while (n < STALL_ADDRS) {
        s_stall.addr[n++] = 0;
    }
    s_stall.magic = STALL_MAGIC;
}

const char *watchdog_last_stall(void)
{
    static char s_str[64];
    static bool s_read;

    if (!s_read) {
        s_read = true;
        if (s_stall.magic == STALL_MAGIC) {
            int pos = snprintf(s_str, sizeof(s_str), "stall:");

            for (int i = 0; i < STALL_ADDRS && s_stall.addr[i] &&
                 pos < (int)sizeof(s_str); i++) {
                pos += snprintf(s_str + pos, sizeof(s_str) - pos, "%s%x",
                                i ? "/" : "", (unsigned)s_stall.addr[i]);
            }
            LOG_ERR("previous boot's watchdog: %s", s_str);
        }
        s_stall.magic = 0;
    }
    return s_str[0] ? s_str : NULL;
}

#if IS_ENABLED(CONFIG_APP_WDT_FREEZE_CAPTURE)
/* -- the early warning ------------------------------------------------------
 *
 * The note above only exists when the kernel is alive: the 32 s callback runs
 * from the kernel's timer interrupt.  Three car resets on 2026-11-16 left plain
 * rst=wdt, no note and no "no kick" line, and came 12 to 13 s after the last
 * datagram: the hardware watchdog's own window (12 s, fed by that same timer),
 * so the timer had stopped.  Nothing in a thread can see that, so this does.
 *
 * The watchdog raises an interrupt about 60 us before it resets (two clocks of
 * its 32 kHz).  The driver's own handler is left out (WDT_NRFX_NO_IRQ), and this
 * one is taken at the highest priority there is.  It saves what freeze_note.h
 * describes into noinit RAM, and the next boot reports it.  It does nothing
 * else, and touches no kernel state: at that priority it may not call the
 * kernel.
 *
 * How far that reaches was measured on the bench (a deliberate freeze each way,
 * one per boot), because it is less than the priority numbers suggest.  On this
 * part the non-secure priorities lose their lowest bit to TrustZone's priority
 * scaling, so hardware levels 0 and 1 are one effective level, 2 and 3 the next,
 * and so on.  The early warning (level 0) therefore runs over:
 *   - a handler at level 2 or below: the system timer's (the case that matters
 *     for a corrupted timeout list), the GNSS UART's, GPIO's, ...;
 *   - a thread holding BASEPRI 0x40 or more.
 * and does not run over:
 *   - a handler at level 1, which is where the modem library puts its IPC
 *     interrupt (its priority comes from the devicetree);
 *   - irq_lock(), which is BASEPRI 0x20 here, nor PRIMASK, nor a locked-up CPU.
 * A note says which handler or code it was.  No note after a watchdog reset
 * rules out the first group and leaves the second.
 *
 * Naked, because the frame of the code it interrupted is on the stack that
 * EXC_RETURN in lr names, and has to be found before a prologue moves it. */
static __noinit struct freeze_note s_freeze;
static volatile bool s_cb_ran;          /* the 32 s task watchdog expired */

#define FREEZE_BAD_FRAME  (1u << 1)     /* the interrupted stack pointer was not usable */
#define SRAM_START        ((uintptr_t)CONFIG_SRAM_BASE_ADDRESS)
#define SRAM_END          (SRAM_START + (uintptr_t)CONFIG_SRAM_SIZE * 1024U)

void wdt_early_c(uint32_t *frame, uint32_t exc_return);

void wdt_early_c(uint32_t *frame, uint32_t exc_return)
{
    struct freeze_note *n = &s_freeze;
    uintptr_t f = (uintptr_t)frame;

    n->flags = s_cb_ran ? FREEZE_APP_CB_RAN : 0;
    n->exc_return = exc_return;
    n->sp = (uint32_t)f;
    /* A stack pointer gone wrong must not fault in here: that would be a fatal
     * error and a reboot reported as such, hiding the watchdog's. */
    if ((f & 3u) == 0 && f >= SRAM_START && f + 8 * sizeof(uint32_t) <= SRAM_END) {
        for (int i = 0; i < 8; i++) {
            n->frame[i] = frame[i];
        }
        for (int i = 0; i < FREEZE_ABOVE; i++) {
            n->above[i] = (f + (8 + i + 1) * sizeof(uint32_t) <= SRAM_END) ? frame[8 + i] : 0;
        }
    } else {
        n->flags |= FREEZE_BAD_FRAME;
        for (int i = 0; i < 8; i++) {
            n->frame[i] = 0;
        }
        for (int i = 0; i < FREEZE_ABOVE; i++) {
            n->above[i] = 0;
        }
    }
    n->iabr[0] = NVIC->IABR[0];
    n->iabr[1] = NVIC->IABR[1];
    n->ispr[0] = NVIC->ISPR[0];
    n->ispr[1] = NVIC->ISPR[1];
    n->icsr = SCB->ICSR;
    n->rtc[0] = NRF_RTC1->COUNTER;
    for (int i = 0; i < 4; i++) {
        n->rtc[1 + i] = NRF_RTC1->CC[i];
    }
    for (int i = 0; i < 3; i++) {
        n->rtc[5 + i] = NRF_RTC1->EVENTS_COMPARE[i];
    }
    freeze_note_seal(n);
    nrf_wdt_event_clear(NRF_WDT, NRF_WDT_EVENT_TIMEOUT);
}

static void __attribute__((naked, used)) wdt_early_isr(void)
{
    __asm volatile(
        "tst   lr, #4\n"            /* EXC_RETURN bit 2: the stack the code was using */
        "ite   eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "mov   r1, lr\n"
        "b     wdt_early_c\n");     /* its return is the exception's */
}

static void freeze_capture_arm(void)
{
    const unsigned int irq = NRFX_IRQ_NUMBER_GET(NRF_WDT);

    IRQ_DIRECT_CONNECT(NRFX_IRQ_NUMBER_GET(NRF_WDT), 0, wdt_early_isr, 0);
    /* The highest level: Zephyr's priority 0 comes to hardware level 1, and 0
     * is the same effective level here, but is the better one to ask for. */
    NVIC_SetPriority((IRQn_Type)irq, 0);
    irq_enable(irq);
    nrf_wdt_event_clear(NRF_WDT, NRF_WDT_EVENT_TIMEOUT);
    nrf_wdt_int_enable(NRF_WDT, NRF_WDT_INT_TIMEOUT_MASK);
}

/* What the early warning saw before the last reset, for rst= ("fz:<pc>/<lr>"),
 * with the detail on the log as well.  NULL when there is nothing: the 32 s
 * callback ran (the stall note has it), or the early warning could not. */
const char *watchdog_last_freeze(void)
{
    static char s_str[32];
    static bool s_read;

    if (!s_read) {
        s_read = true;
        if (freeze_note_valid(&s_freeze) && !(s_freeze.flags & FREEZE_APP_CB_RAN)) {
            char text[128];

            freeze_note_short(&s_freeze, s_str, sizeof(s_str));
            for (int i = 0; i < FREEZE_LINES; i++) {
                if (freeze_note_line(&s_freeze, i, text, sizeof(text))) {
                    LOG_ERR("previous boot froze: %s", text);
                }
            }
        }
        s_freeze.magic = 0;
    }
    return s_str[0] ? s_str : NULL;
}
#else
const char *watchdog_last_freeze(void)
{
    return NULL;
}
#endif /* CONFIG_APP_WDT_FREEZE_CAPTURE */

static void wdt_timeout_cb(int channel_id, void *user_data)
{
    ARG_UNUSED(channel_id);
    ARG_UNUSED(user_data);

#if IS_ENABLED(CONFIG_APP_WDT_FREEZE_CAPTURE)
    s_cb_ran = true;
#endif
    note_stall();
    LOG_ERR("no kick for %ds — resetting", WATCHDOG_TIMEOUT_S);
    /* Deferred logging: without this the message dies with the reset. */
    LOG_PANIC();

    if (!s_hw_backed) {
        sys_reboot(SYS_REBOOT_COLD);
    }
}

void watchdog_init(void)
{
    const struct device *hw = s_hw;

    if (!device_is_ready(hw)) {
        LOG_ERR("%s not ready — no hardware backing", hw->name);
        hw = NULL;
    }

    int err = task_wdt_init(hw);
    if (err && err != -EALREADY) {
        LOG_WRN("task_wdt_init: %d", err);
        return;
    }
    s_hw_backed = (hw != NULL);
    s_fed_by = k_current_get();

    s_channel = task_wdt_add(WATCHDOG_TIMEOUT_S * 1000, wdt_timeout_cb, NULL);
    if (s_channel < 0) {
        LOG_WRN("task_wdt_add: %d", s_channel);
        return;
    }
    LOG_INF("enabled (%ds, %s)", WATCHDOG_TIMEOUT_S,
            s_hw_backed ? "hw-backed" : "software only");
#if IS_ENABLED(CONFIG_APP_WDT_FREEZE_CAPTURE)
    if (s_hw_backed) {
        freeze_capture_arm();
    }
#endif
}

void watchdog_kick(void)
{
    if (s_channel >= 0) {
        task_wdt_feed(s_channel);
    }
}
