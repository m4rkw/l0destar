/*
 * The interrupt stack's high-water mark across a reset: isr_mark.h says why.
 *
 * The first application init (a thread, so no handler is running and the whole
 * stack is dead memory) reads how far down the previous run got, then fills the
 * stack with the pattern again.  rst= carries "isr:<bytes>" after a reset that
 * nothing planned, and after any reset when the peak was half the stack or
 * more, with a log line for those.
 *
 * What the peak includes: the handlers of the previous run, and this boot's own
 * up to this point (the kernel's start-up runs on this memory too, from the
 * top).  A power cut, a pin reset or a new image leaves no valid record, and
 * then there is nothing to report.
 */
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include "app.h"
#include "isr_mark.h"

#ifdef CONFIG_APP_ISR_STACK_MARK

LOG_MODULE_REGISTER(isr_mark, LOG_LEVEL_INF);

/* The kernel's own declaration (kernel_internal.h) is not on the include path. */
K_KERNEL_STACK_ARRAY_DECLARE(z_interrupt_stacks, CONFIG_MP_MAX_NUM_CPUS, CONFIG_ISR_STACK_SIZE);

static __noinit struct isr_mark_state s_state;
static size_t s_size;
static size_t s_used;
static bool s_have;

static int isr_mark_init(void)
{
	uint8_t *base = K_KERNEL_STACK_BUFFER(z_interrupt_stacks[0]);

	s_size = K_KERNEL_STACK_SIZEOF(z_interrupt_stacks[0]);
	s_have = isr_mark_valid(&s_state, s_size);
	if (s_have) {
		s_used = isr_mark_used(base, s_size);
	}

	/* Invalid while it is being filled, so a reset in the middle reads as
	 * nothing rather than as a peak. */
	s_state.magic = 0;
	isr_mark_fill(base, s_size);
	isr_mark_seal(&s_state, s_size);
	return 0;
}

SYS_INIT(isr_mark_init, APPLICATION, 0);

const char *isr_mark_last(bool unplanned)
{
	static char s_token[16];
	static bool s_read;

	if (!s_read) {
		s_read = true;
		if (s_have && isr_mark_report(s_used, s_size, unplanned)) {
			isr_mark_token(s_used, s_token, sizeof(s_token));
			if (isr_mark_loud(s_used, s_size)) {
				char text[80];

				isr_mark_line(s_used, s_size, text, sizeof(text));
				LOG_WRN("%s", text);
			}
		}
	}
	return s_token[0] ? s_token : NULL;
}

#else /* CONFIG_APP_ISR_STACK_MARK */

const char *isr_mark_last(bool unplanned)
{
	ARG_UNUSED(unplanned);
	return NULL;
}

#endif
