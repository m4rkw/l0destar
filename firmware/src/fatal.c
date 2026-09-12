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
 */

#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <stdio.h>

#include "app.h"

LOG_MODULE_REGISTER(fatal, CONFIG_APP_LOG_LEVEL);

#define FATAL_MAGIC 0x10DE57A2u

/* Deliberately __noinit: zeroing it at boot would erase the thing it exists
 * to carry.  Power-on leaves RAM undefined, which is what the magic is for. */
static __noinit struct {
    uint32_t magic;
    uint32_t reason;
    uint32_t pc;
    uint32_t lr;
} s_crash;

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    s_crash.magic  = FATAL_MAGIC;
    s_crash.reason = reason;
    s_crash.pc     = esf ? esf->basic.pc : 0;
    s_crash.lr     = esf ? esf->basic.lr : 0;

    /* LOG_PANIC() first: it puts the log core into synchronous mode, so the
     * Zephyr fault dump queued ahead of this actually reaches the console.
     * Then printk, which by then writes straight out — a plain LOG_ERR here
     * can still be sitting in the deferred queue when the reset lands, and
     * a crash that prints nothing is what makes a revert loop unreadable. */
    LOG_PANIC();
    printk("*** FATAL: reason %u pc 0x%08x lr 0x%08x — rebooting ***\n",
           reason, (unsigned)s_crash.pc, (unsigned)s_crash.lr);

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
            snprintf(s_str, sizeof(s_str), "fatal:%u@0x%x",
                     (unsigned)s_crash.reason, (unsigned)s_crash.pc);
            LOG_ERR("previous boot ended in fatal error %u at 0x%08x (lr 0x%08x)",
                    (unsigned)s_crash.reason, (unsigned)s_crash.pc,
                    (unsigned)s_crash.lr);
        }
        s_crash.magic = 0;
    }
    return s_str[0] ? s_str : NULL;
}
