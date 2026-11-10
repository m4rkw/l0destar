/*
 * Fatal error handler.
 *
 * Zephyr's default one halts: k_sys_fatal_error_handler() calls
 * arch_system_halt(), which locks interrupts and spins forever.  On a device
 * in a car that is the worst possible outcome — the fault dump goes out of a
 * console nobody is watching and then the unit is gone: no telemetry, no
 * ignition wake (interrupts are locked), no accelerometer alerts, no sleep,
 * and nothing short of pulling power brings it back.  CONFIG_RESET_ON_FATAL
 * _ERROR would reboot instead, but it reboots silently: RESETREAS says SREQ,
 * which reads as an ordinary commanded reboot, so the record after it looks
 * like a routine restart and the crash leaves no trace at all.
 *
 * So: reboot, but leave a note first.  The note lives in noinit RAM, which
 * survives a warm reset, and dbglog folds it into the rst= field of the next
 * record that reaches the server — rst=sw+fatal:4@0x2a1c8 rather than a bare
 * rst=sw.  Reason and PC are enough to place the fault in the map file.
 *
 * Except for a stack overflow (reason 2): the hardware cannot push a frame
 * onto a stack that has just run out, so the registers printed are whatever
 * the memory below it held and the PC places nothing.  The note therefore also
 * keeps which thread it was and how big its stack is, and the next boot's ERR
 * line says so: 1152 or 8192 bytes names the thread where the PC names none.
 */

#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <stdio.h>

#include "app.h"

LOG_MODULE_REGISTER(fatal, CONFIG_APP_LOG_LEVEL);

/* Changed with the layout (thread and stack_size were added), so a note left by
 * an image with the old one is not read as this one's. */
#define FATAL_MAGIC 0x10DE57A3u

/* Deliberately __noinit: zeroing it at boot would erase the thing it exists
 * to carry.  Power-on leaves RAM undefined, which is what the magic is for. */
static __noinit struct {
    uint32_t magic;
    uint32_t reason;
    uint32_t pc;
    uint32_t lr;
    uint32_t thread;        /* k_current_get() at the fault */
    uint32_t stack_size;    /* that thread's stack in bytes, 0 if not known */
} s_crash;

/* Whether a thread object can be read: in RAM and word aligned.  The handler
 * reads one field of it, and a fault handler that faults in its own path never
 * reaches the reboot. */
static bool in_ram(const void *p, size_t len)
{
    uintptr_t lo = CONFIG_SRAM_BASE_ADDRESS;
    uintptr_t hi = lo + (uintptr_t)CONFIG_SRAM_SIZE * 1024U;
    uintptr_t a = (uintptr_t)p;

    return a >= lo && a < hi && len <= hi - a && (a & 3U) == 0;
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    k_tid_t thread = k_current_get();

    s_crash.magic  = FATAL_MAGIC;
    s_crash.reason = reason;
    s_crash.pc     = esf ? esf->basic.pc : 0;
    s_crash.lr     = esf ? esf->basic.lr : 0;
    s_crash.thread = (uint32_t)(uintptr_t)thread;
    s_crash.stack_size = 0;
#if IS_ENABLED(CONFIG_THREAD_STACK_INFO)
    if (in_ram(thread, sizeof(*thread))) {
        s_crash.stack_size = (uint32_t)thread->stack_info.size;
    }
#endif

    /* LOG_PANIC() first: it puts the log core into synchronous mode, so the
     * Zephyr fault dump queued ahead of this actually reaches the console.
     * Then printk, which by then writes straight out — a plain LOG_ERR here
     * can still be sitting in the deferred queue when the reset lands, and
     * a crash that prints nothing is what makes a revert loop unreadable. */
    LOG_PANIC();
    printk("*** FATAL: reason %u pc 0x%08x lr 0x%08x thread 0x%08x stack %u — rebooting ***\n",
           reason, (unsigned)s_crash.pc, (unsigned)s_crash.lr,
           (unsigned)s_crash.thread, (unsigned)s_crash.stack_size);

    sys_reboot(SYS_REBOOT_COLD);
    CODE_UNREACHABLE;
}

/* "fatal:<reason>@<pc>" for the boot after a crash, NULL otherwise.  Read
 * once and cleared, like the reset cause it rides with, so it is reported by
 * the restart it belongs to and not by every one after it. */
const char *fatal_last_crash(void)
{
    static char s_str[32];
    static bool s_read;

    if (!s_read) {
        s_read = true;
        if (s_crash.magic == FATAL_MAGIC) {
            /* The system workqueue is the one thread worth naming: it runs the
             * modem's event handlers, so it is where the app's code meets the
             * library's.  The stack size tells the rest apart. */
            const char *who =
                (s_crash.thread == (uint32_t)(uintptr_t)k_work_queue_thread_get(&k_sys_work_q))
                    ? " (sysworkq)" : "";

            snprintf(s_str, sizeof(s_str), "fatal:%u@0x%x",
                     (unsigned)s_crash.reason, (unsigned)s_crash.pc);
            LOG_ERR("previous boot ended in fatal error %u at 0x%08x (lr 0x%08x), "
                    "thread 0x%08x%s, stack %u B%s",
                    (unsigned)s_crash.reason, (unsigned)s_crash.pc,
                    (unsigned)s_crash.lr, (unsigned)s_crash.thread, who,
                    (unsigned)s_crash.stack_size,
                    s_crash.reason == K_ERR_STACK_CHK_FAIL
                        ? " (overflow: pc/lr invalid)" : "");
        }
        s_crash.magic = 0;
    }
    return s_str[0] ? s_str : NULL;
}
