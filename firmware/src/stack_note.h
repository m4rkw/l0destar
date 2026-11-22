/*
 * Which thread's stack ran out, read back after the reset that followed.  Pure,
 * so it is checked on the host: tests/test_stack_note.py.  fatal.c keeps the
 * table and reads the real stacks.
 *
 * A thread stack that overflows (CONFIG_BUILTIN_STACK_GUARD, PSPLIM) raises a
 * UsageFault whose exception frame has no room on that stack, so the fault
 * escalates to a HardFault, and on this part the HardFault belongs to the
 * secure side: TF-M prints "FATAL ERROR: HardFault" and halts with interrupts
 * off.  TF-M's non-secure fault service hands only BusFault, SecureFault and
 * SPU faults to the application (ns_fault_service.c), so k_sys_fatal_error_
 * handler never runs, no note is left and the next boot says plain rst=wdt.
 *
 * What is left is the stacks themselves: CONFIG_INIT_STACKS fills each with
 * 0xAA when its thread is created, and the push that overflowed (or the stack
 * pointer that was dropped past the limit by a big frame) leaves the bottom of
 * it overwritten.  The application keeps a table of its threads' stacks in
 * noinit RAM; the first init of the next boot, before any thread is created and
 * refills its stack, counts the pattern left at the bottom of each.  A stack
 * with almost none left is the one that ran out (or one so close that it was
 * the next to).  A frame bigger than what was left is stopped before it
 * writes anything, so a stack can overflow with a margin; the threshold only
 * catches the ones that went to the end.
 */
#ifndef STACK_NOTE_H
#define STACK_NOTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Changes with the layout, so a table left by another image is ignored. */
#define STACK_NOTE_MAGIC  0x57AC4E02u
#define STACK_NOTE_MAX    16

/* Zephyr's own fill (CONFIG_INIT_STACKS). */
#define STACK_NOTE_FILL   0xAAu

/* Fewer bytes than this left at the bottom: the stack ran out.  An exception
 * frame is 32 bytes, 104 with the FPU's registers. */
#define STACK_NOTE_FULL   96

struct stack_note_ent {
	uint32_t thread;      /* the k_thread object: names the well-known ones */
	uint32_t start;       /* lowest address of its stack */
	uint32_t size;
};

struct stack_note_reg {
	uint32_t magic;
	uint32_t stamp;       /* identifies the image: stacks move when it changes */
	uint32_t n;
	struct stack_note_ent e[STACK_NOTE_MAX];
	uint32_t check;       /* over everything above: RAM after a power cut fails it */
};

static inline uint32_t stack_note_sum(const struct stack_note_reg *r)
{
	const uint32_t *w = (const uint32_t *)r;
	size_t count = offsetof(struct stack_note_reg, check) / sizeof(uint32_t);
	uint32_t h = 2166136261u;                  /* FNV-1a over the words */

	for (size_t i = 0; i < count; i++) {
		h = (h ^ w[i]) * 16777619u;
	}
	return h;
}

/* Starts a table.  It is invalid until sealed, so a reset in the middle of
 * being filled reads as nothing. */
static inline void stack_note_begin(struct stack_note_reg *r, uint32_t stamp)
{
	r->magic = 0;
	r->stamp = stamp;
	r->n = 0;
}

/* False when the table is full. */
static inline bool stack_note_add(struct stack_note_reg *r, uint32_t thread, uint32_t start,
				  uint32_t size)
{
	if (r->n >= STACK_NOTE_MAX) {
		return false;
	}
	r->e[r->n].thread = thread;
	r->e[r->n].start = start;
	r->e[r->n].size = size;
	r->n++;
	return true;
}

static inline void stack_note_seal(struct stack_note_reg *r)
{
	r->magic = STACK_NOTE_MAGIC;
	r->check = stack_note_sum(r);     /* covers the magic, so after it */
}

static inline bool stack_note_valid(const struct stack_note_reg *r, uint32_t stamp)
{
	return r->magic == STACK_NOTE_MAGIC && r->stamp == stamp && r->n <= STACK_NOTE_MAX &&
	       r->check == stack_note_sum(r);
}

/* Bytes at the low end of a stack that still hold the pattern. */
static inline size_t stack_note_unused(const uint8_t *base, size_t size)
{
	size_t n = 0;

	while (n < size && base[n] == STACK_NOTE_FILL) {
		n++;
	}
	return n;
}

/* The entry whose stack has the least pattern left, if that is under
 * STACK_NOTE_FULL; -1 for none.  map(start, size) gives the stack's memory, or
 * NULL when it cannot be read (a table that is stale in a way the stamp missed
 * points anywhere). */
static inline int stack_note_find(const struct stack_note_reg *r,
				  const uint8_t *(*map)(uint32_t start, uint32_t size))
{
	int worst = -1;
	size_t least = STACK_NOTE_FULL;

	for (uint32_t i = 0; i < r->n; i++) {
		const struct stack_note_ent *e = &r->e[i];
		const uint8_t *base;

		if (e->size <= STACK_NOTE_FULL || (base = map(e->start, e->size)) == NULL) {
			continue;
		}
		size_t left = stack_note_unused(base, e->size);

		if (left < least) {
			least = left;
			worst = (int)i;
		}
	}
	return worst;
}

#endif /* STACK_NOTE_H */
