/*
 * How deep the interrupt stack went before the last reset.  Pure, so it is
 * checked on the host: tests/test_isr_mark.py.  isr_mark.c fills and reads the
 * real stack.
 *
 * Every interrupt handler runs on one stack, CONFIG_ISR_STACK_SIZE (2 KB).
 * When it runs out, the CPU's stack limit stops the push, the fault that
 * follows has no stack to be stacked on and escalates to a HardFault, and on
 * this part the HardFault belongs to the secure side: TF-M prints "FATAL
 * ERROR: HardFault" on a UART that nobody reads in a car and halts the CPU
 * with interrupts off.  The application never runs again.  The hardware
 * watchdog resets the chip up to 12 s later and the next boot says plain
 * rst=wdt, with no note of any kind (measured on the bench, 2026-11-16).  A
 * kernel timer that stopped looks the same from outside, so the car's three
 * freezes of that day could not be told from this.
 *
 * So the stack is filled with a pattern at every boot, and read back at the
 * next one before it is filled again.  The lowest byte that is no longer the
 * pattern is as far down as any handler got; a stack that ran out reads as
 * full, and anything else gives its real peak.
 */
#ifndef ISR_MARK_H
#define ISR_MARK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Changes with the layout, so a record left by another image is ignored. */
#define ISR_MARK_MAGIC   0x15A57AC1u

/* Not Zephyr's own 0xAA (CONFIG_INIT_STACKS), which would read as unused. */
#define ISR_MARK_FILL    0xA5u

/* The top of the stack is left unfilled: whatever started the kernel sits
 * there, and nothing can be learned from the first few bytes anyway. */
#define ISR_MARK_KEEP    128

/* A peak with less than this left is a stack that ran out: an exception frame
 * is 32 bytes, 104 with the FPU's registers, and the push that did not fit
 * was refused. */
#define ISR_MARK_FULL    128

/* What the last fill left in noinit RAM. */
struct isr_mark_state {
	uint32_t magic;
	uint32_t size;      /* of the stack that was filled */
	uint32_t check;     /* over the two words above: RAM after a power cut fails it */
};

static inline uint32_t isr_mark_sum(const struct isr_mark_state *s)
{
	uint32_t h = 2166136261u;                  /* FNV-1a over the two words */

	h = (h ^ s->magic) * 16777619u;
	h = (h ^ s->size) * 16777619u;
	return h;
}

static inline void isr_mark_seal(struct isr_mark_state *s, uint32_t size)
{
	s->size = size;
	s->magic = ISR_MARK_MAGIC;
	s->check = isr_mark_sum(s);
}

/* True when the record was left by a fill of a stack of this size. */
static inline bool isr_mark_valid(const struct isr_mark_state *s, uint32_t size)
{
	return s->magic == ISR_MARK_MAGIC && s->size == size && s->check == isr_mark_sum(s);
}

/* Bytes at the low end of the stack that still hold the pattern. */
static inline size_t isr_mark_unused(const uint8_t *base, size_t size)
{
	size_t n = 0;

	while (n < size && base[n] == ISR_MARK_FILL) {
		n++;
	}
	return n;
}

/* The deepest the handlers went, in bytes from the stack's top. */
static inline size_t isr_mark_used(const uint8_t *base, size_t size)
{
	return size - isr_mark_unused(base, size);
}

/* Fills the stack with the pattern, except for its top ISR_MARK_KEEP bytes.
 * Only from a thread: the handlers' own frames live in that memory. */
static inline void isr_mark_fill(uint8_t *base, size_t size)
{
	if (size > ISR_MARK_KEEP) {
		memset(base, ISR_MARK_FILL, size - ISR_MARK_KEEP);
	}
}

/* Half full or more: not routine, so it gets a log line as well as the token. */
static inline bool isr_mark_loud(size_t used, size_t size)
{
	return used >= size / 2;
}

/* Whether to say anything about a peak.  After a reset that nothing planned it
 * is the answer to "was it the interrupt stack?", so always; after any other
 * only when it is high. */
static inline bool isr_mark_report(size_t used, size_t size, bool unplanned)
{
	return unplanned || isr_mark_loud(used, size);
}

/* "isr:1948", for rst=. */
static inline void isr_mark_token(size_t used, char *buf, size_t n)
{
	snprintf(buf, n, "isr:%u", (unsigned)used);
}

/* The log line for a loud peak. */
static inline void isr_mark_line(size_t used, size_t size, char *buf, size_t n)
{
	snprintf(buf, n, "previous boot: interrupt stack peak %u of %u bytes%s", (unsigned)used,
		 (unsigned)size, size - used < ISR_MARK_FULL ? " (full)" : "");
}

#endif /* ISR_MARK_H */
