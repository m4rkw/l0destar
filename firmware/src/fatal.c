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
 *
 * And a stack overflow never gets here at all on the car build: the hardware
 * stack guard's fault escalates to a HardFault, which the secure side (TF-M)
 * takes and halts on, leaving only a watchdog reset behind (stack_note.h).  So
 * after a watchdog reset with no note, the first init reads the threads'
 * stacks back (stack_note.h) and, if one ran out, writes this same note
 * itself, marked inferred.
 */

#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/init.h>
#include <stdio.h>

#include "app.h"
#include "stack_note.h"

LOG_MODULE_REGISTER(fatal, CONFIG_APP_LOG_LEVEL);

/* Changed with the layout (thread and stack_size were added), so a note left by
 * an image with the old one is not read as this one's. */
#define FATAL_MAGIC 0x10DE57A4u

/* Deliberately __noinit: zeroing it at boot would erase the thing it exists
 * to carry.  Power-on leaves RAM undefined, which is what the magic is for. */
static __noinit struct {
    uint32_t magic;
    uint32_t reason;
    uint32_t pc;
    uint32_t lr;
    uint32_t thread;        /* k_current_get() at the fault */
    uint32_t stack_size;    /* that thread's stack in bytes, 0 if not known */
    uint32_t inferred;      /* 1: written by the next boot from the stack fill, not by a fault handler */
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

#if IS_ENABLED(CONFIG_APP_STACK_NOTE)
/* Kernel and log-core threads, named in the report like sysworkq below. */
extern struct k_thread z_main_thread;
#if IS_ENABLED(CONFIG_LOG_PROCESS_THREAD)
extern struct k_thread logging_thread;
#endif

/* The image's threads' stacks, kept for the next boot (stack_note.h). */
static __noinit struct stack_note_reg s_reg;

/* Changes whenever the image does, so a table from the old one is not read. */
static uint32_t image_stamp(void)
{
    return (uint32_t)(uintptr_t)&s_reg ^ ((uint32_t)(uintptr_t)fatal_last_crash << 7) ^
           (uint32_t)(uintptr_t)&s_crash;
}

static void reg_thread(const struct k_thread *t, void *ud)
{
    ARG_UNUSED(ud);
    (void)stack_note_add(&s_reg, (uint32_t)(uintptr_t)t, (uint32_t)t->stack_info.start,
                         (uint32_t)t->stack_info.size);
}

/* Called from log_stack_check(): a few minutes after boot and on sleep entry,
 * by which time every thread exists.  The table lives across boots, so a
 * thread created later is only missed until the next call. */
void fatal_stack_register(void)
{
    stack_note_begin(&s_reg, image_stamp());
    k_thread_foreach(reg_thread, NULL);
    stack_note_seal(&s_reg);
}

static const uint8_t *stack_map(uint32_t start, uint32_t size)
{
    const void *p = (const void *)(uintptr_t)start;

    return in_ram(p, size) ? (const uint8_t *)p : NULL;
}

/* PRE_KERNEL_1: before the kernel creates a thread, which would refill its
 * stack and lose what the last run left in it.  Only after a watchdog reset
 * (what a TF-M halt ends in) and only when no handler left a note of its own. */
static int stack_note_init(void)
{
    uint32_t cause = 0;

    if (s_crash.magic == FATAL_MAGIC ||
        hwinfo_get_reset_cause(&cause) != 0 ||
        !(cause & (RESET_WATCHDOG | RESET_CPU_LOCKUP)) ||
        !stack_note_valid(&s_reg, image_stamp())) {
        return 0;
    }

    int i = stack_note_find(&s_reg, stack_map);

    if (i >= 0) {
        s_crash.magic = FATAL_MAGIC;
        s_crash.reason = K_ERR_STACK_CHK_FAIL;
        s_crash.pc = 0;
        s_crash.lr = 0;
        s_crash.thread = s_reg.e[i].thread;
        s_crash.stack_size = s_reg.e[i].size;
        s_crash.inferred = 1;
    }
    return 0;
}

SYS_INIT(stack_note_init, PRE_KERNEL_1, 0);

/* "main", "log", "sysworkq", else NULL */
static const char *thread_name(uint32_t t)
{
    if (t == (uint32_t)(uintptr_t)&z_main_thread) {
        return "main";
    }
#if IS_ENABLED(CONFIG_LOG_PROCESS_THREAD)
    if (t == (uint32_t)(uintptr_t)&logging_thread) {
        return "log";
    }
#endif
    if (t == (uint32_t)(uintptr_t)k_work_queue_thread_get(&k_sys_work_q)) {
        return "sysworkq";
    }
    return NULL;
}
#else
static const char *thread_name(uint32_t t)
{
    return t == (uint32_t)(uintptr_t)k_work_queue_thread_get(&k_sys_work_q) ? "sysworkq" : NULL;
}
#endif

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    k_tid_t thread = k_current_get();

    s_crash.magic  = FATAL_MAGIC;
    s_crash.reason = reason;
    s_crash.pc     = esf ? esf->basic.pc : 0;
    s_crash.lr     = esf ? esf->basic.lr : 0;
    s_crash.thread = (uint32_t)(uintptr_t)thread;
    s_crash.stack_size = 0;
    s_crash.inferred = 0;
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
            const char *name = thread_name(s_crash.thread);
            char who[16] = "";

            if (name) {
                snprintf(who, sizeof(who), " (%s)", name);
            }
            if (s_crash.inferred) {
                /* No registers to place it: the thread is the whole note. */
                if (name) {
                    snprintf(s_str, sizeof(s_str), "fatal:%u@%s",
                             (unsigned)s_crash.reason, name);
                } else {
                    snprintf(s_str, sizeof(s_str), "fatal:%u@t%u",
                             (unsigned)s_crash.reason, (unsigned)s_crash.stack_size);
                }
            } else {
                snprintf(s_str, sizeof(s_str), "fatal:%u@0x%x",
                         (unsigned)s_crash.reason, (unsigned)s_crash.pc);
            }
            LOG_ERR("previous boot ended in fatal error %u at 0x%08x (lr 0x%08x), "
                    "thread 0x%08x%s, stack %u B%s",
                    (unsigned)s_crash.reason, (unsigned)s_crash.pc,
                    (unsigned)s_crash.lr, (unsigned)s_crash.thread, who,
                    (unsigned)s_crash.stack_size,
                    s_crash.inferred
                        ? " (stack full at the watchdog reset, read from its fill: no fault note, TF-M took the fault)"
                        : s_crash.reason == K_ERR_STACK_CHK_FAIL
                              ? " (overflow: pc/lr invalid)" : "");
        }
        s_crash.magic = 0;
    }
    return s_str[0] ? s_str : NULL;
}
