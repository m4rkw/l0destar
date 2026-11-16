/*
 * What the hardware watchdog's early warning leaves behind when it catches a
 * frozen kernel, and how the next boot words it.  Pure, so it is checked on
 * the host: tests/test_freeze_note.py.  The interrupt that fills it in is in
 * watchdog.c.
 *
 * The watchdog's own window is 12 s and the kernel's timer feeds it, so a reset
 * that no thread's kick explains and that left no "no kick" line means the
 * timer interrupt stopped running: something held interrupts off, sat in an
 * interrupt handler, or locked the CPU up.  The early warning comes about 60 us
 * before the reset, at the highest interrupt priority, so it runs over a stuck
 * handler at the system timer's level or below (not over the modem's IPC
 * interrupt, irq_lock(), PRIMASK or a lock-up: watchdog.c has what was
 * measured).  What it saves says which it was:
 *
 *   - the registers of the code it interrupted (pc, lr, xpsr): pc says where it
 *     was stuck, and the exception number in xpsr says whether that was
 *     thread code or an interrupt handler (and which);
 *   - which interrupts were active and which pending: the stuck handler is the
 *     active one, and the timer's interrupt waiting beside it is the proof that
 *     it was being held off;
 *   - the system timer's own registers: a compare that has passed with its
 *     event never served is a masked or starved interrupt, one that is far in
 *     the future is a tick that was never programmed.
 *
 * No note at all after a watchdog reset says the early warning could not run
 * either: a handler at the modem IPC's level, irq_lock(), PRIMASK, a locked-up
 * CPU, or the supply.  That rules out a stuck handler at the timer's level or
 * below, which is worth knowing too.
 */
#ifndef FREEZE_NOTE_H
#define FREEZE_NOTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Changes with the layout, so a note left by another image is ignored. */
#define FREEZE_MAGIC     0xF2EE2E02u
#define FREEZE_ABOVE     8
#define FREEZE_LINES     5

/* A thread had stopped kicking and the 32 s task watchdog's callback had run:
 * the kernel was alive, and the stall note (watchdog.c) says where. */
#define FREEZE_APP_CB_RAN  (1u << 0)

struct freeze_note {
	uint32_t magic;
	uint32_t flags;
	uint32_t exc_return;               /* EXC_RETURN of the early warning's entry */
	uint32_t frame[8];                 /* r0-r3, r12, lr, pc, xpsr it interrupted */
	uint32_t sp;                       /* where that frame was */
	uint32_t above[FREEZE_ABOVE];      /* the words above the frame */
	uint32_t iabr[2];                  /* NVIC: active interrupts, IRQ 0-63 */
	uint32_t ispr[2];                  /* NVIC: pending interrupts, IRQ 0-63 */
	uint32_t icsr;                     /* SCB ICSR */
	uint32_t rtc[8];                   /* system timer: counter, cc[0..3], compare events[0..2] */
	uint32_t check;                    /* over everything above: RAM after a power cut fails it */
};

static inline uint32_t freeze_note_sum(const struct freeze_note *n)
{
	const uint32_t *w = (const uint32_t *)n;
	size_t count = offsetof(struct freeze_note, check) / sizeof(uint32_t);
	uint32_t h = 2166136261u;                  /* FNV-1a over the words */

	for (size_t i = 0; i < count; i++) {
		h = (h ^ w[i]) * 16777619u;
	}
	return h;
}

/* Seals the note: the check first, the magic last. */
static inline void freeze_note_seal(struct freeze_note *n)
{
	n->magic = FREEZE_MAGIC;
	n->check = freeze_note_sum(n);
}

static inline bool freeze_note_valid(const struct freeze_note *n)
{
	return n->magic == FREEZE_MAGIC && n->check == freeze_note_sum(n);
}

/* The code address, Thumb bit off, as a map file or addr2line wants it. */
static inline uint32_t freeze_note_pc(const struct freeze_note *n)
{
	return n->frame[6] & ~1u;
}

static inline uint32_t freeze_note_lr(const struct freeze_note *n)
{
	return n->frame[5] & ~1u;
}

/* IPSR of the interrupted code: 0 for thread code, else the exception number
 * (IRQ number + 16). */
static inline uint32_t freeze_note_exc(const struct freeze_note *n)
{
	return n->frame[7] & 0x1ffu;
}

/* "i<irq>" for an interrupt handler, "e<n>" for a system exception, "" for
 * thread code. */
static inline void freeze_note_where(const struct freeze_note *n, char *buf, size_t size)
{
	uint32_t exc = freeze_note_exc(n);

	if (size == 0) {
		return;
	}
	if (exc == 0) {
		buf[0] = '\0';
	} else if (exc >= 16) {
		snprintf(buf, size, "i%u", (unsigned)(exc - 16));
	} else {
		snprintf(buf, size, "e%u", (unsigned)exc);
	}
}

/* "fz:<pc>/<lr>[/i<irq>]" for rst=: the code address, its caller, and the
 * interrupt it was in, if any. */
static inline void freeze_note_short(const struct freeze_note *n, char *buf, size_t size)
{
	char where[8];

	freeze_note_where(n, where, sizeof(where));
	snprintf(buf, size, "fz:%x/%x%s%s", (unsigned)freeze_note_pc(n),
		 (unsigned)freeze_note_lr(n), where[0] ? "/" : "", where);
}

/* "42,21", or "-" for none: the interrupts set in two 32-bit words. */
static inline void freeze_note_irqs(const uint32_t w[2], char *buf, size_t size)
{
	size_t pos = 0;

	if (size == 0) {
		return;
	}
	buf[0] = '\0';
	for (unsigned irq = 0; irq < 64; irq++) {
		if (!(w[irq / 32] & (1u << (irq % 32)))) {
			continue;
		}
		int k = snprintf(buf + pos, size - pos, "%s%u", pos ? "," : "", irq);

		if (k < 0 || (size_t)k >= size - pos) {
			buf[size - 1] = '\0';
			return;
		}
		pos += (size_t)k;
	}
	if (pos == 0) {
		snprintf(buf, size, "-");
	}
}

/* Detail line idx (0 to FREEZE_LINES - 1), short enough for one log line.
 * False when idx is out of range. */
static inline bool freeze_note_line(const struct freeze_note *n, int idx, char *buf, size_t size)
{
	char a[40], p[40], where[8];

	if (idx < 0 || idx >= FREEZE_LINES || size == 0) {
		return false;
	}
	switch (idx) {
	case 0:
		freeze_note_where(n, where, sizeof(where));
		snprintf(buf, size, "pc 0x%08x lr 0x%08x xpsr 0x%08x %s sp 0x%08x%s",
			 (unsigned)freeze_note_pc(n), (unsigned)freeze_note_lr(n),
			 (unsigned)n->frame[7], where[0] ? where : "thread", (unsigned)n->sp,
			 (n->flags & FREEZE_APP_CB_RAN) ? " (task wdt had expired)" : "");
		break;
	case 1:
		freeze_note_irqs(n->iabr, a, sizeof(a));
		freeze_note_irqs(n->ispr, p, sizeof(p));
		snprintf(buf, size, "active irq %s pending irq %s icsr 0x%08x", a, p,
			 (unsigned)n->icsr);
		break;
	case 2:
		snprintf(buf, size, "rtc1 cnt %u cc %u %u %u %u ev %u %u %u",
			 (unsigned)n->rtc[0], (unsigned)n->rtc[1], (unsigned)n->rtc[2],
			 (unsigned)n->rtc[3], (unsigned)n->rtc[4], (unsigned)(n->rtc[5] & 1u),
			 (unsigned)(n->rtc[6] & 1u), (unsigned)(n->rtc[7] & 1u));
		break;
	case 3:
		snprintf(buf, size, "r0-r3,r12 %x %x %x %x %x",
			 (unsigned)n->frame[0], (unsigned)n->frame[1], (unsigned)n->frame[2],
			 (unsigned)n->frame[3], (unsigned)n->frame[4]);
		break;
	default: {
		int pos = snprintf(buf, size, "stack above the frame");

		for (int i = 0; i < FREEZE_ABOVE && pos > 0 && (size_t)pos < size; i++) {
			pos += snprintf(buf + pos, size - (size_t)pos, " %x",
					(unsigned)n->above[i]);
		}
		break;
	}
	}
	return true;
}

#endif /* FREEZE_NOTE_H */
