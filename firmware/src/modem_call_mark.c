/*
 * The last call into the modem library across a reset: modem_call_mark.h says
 * why and how.
 *
 * g_mcm is __noinit RAM, like the crash note (fatal.c), the freeze note
 * (watchdog.c) and the interrupt stack's mark (isr_mark.c): a watchdog reset
 * (RESETREAS = DOG), a soft reset or a lock-up leaves SRAM as it was, so the
 * slots written before it are still there.  A power cut or a pin reset leaves
 * random bits (measured on the bench, see isr_mark.c); the magic fails and the
 * state is cleared.
 *
 * Read and cleared once at boot, before the application makes its first call,
 * so a reset in the middle of this boot reads as nothing rather than as the
 * previous run's call again.  Nothing is logged while the unit works; after a
 * reset that a mark explains, one ERR line per call still entered.
 */
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include "app.h"
#include "modem_call_mark.h"

#ifdef CONFIG_APP_MODEM_CALL_MARK

LOG_MODULE_REGISTER(modem_call_mark, LOG_LEVEL_INF);

__noinit struct mcm_state g_mcm;

static char s_token[20];            /* "mc:send" for what the last run left, or "" */

void mcm_note(unsigned cls, uint8_t what, const char *cmd)
{
	mcm_note_into(&g_mcm, cls, what, cmd);
}

void mcm_done(unsigned cls)
{
	mcm_leave(&g_mcm, cls);
}

static int modem_call_mark_init(void)
{
	if (g_mcm.magic == MCM_MAGIC) {
		mcm_token(&g_mcm, s_token, sizeof(s_token));
	}
	mcm_clear(&g_mcm);
	return 0;
}

/* Before the modem library or any application thread exists. */
SYS_INIT(modem_call_mark_init, PRE_KERNEL_1, 0);

const char *modem_call_mark_last(bool unplanned)
{
	static bool s_read;

	if (!s_read) {
		s_read = true;
		if (!unplanned) {
			s_token[0] = '\0';
		} else if (s_token[0]) {
			LOG_ERR("previous boot was in modem call %s", s_token + 3);
		}
	}
	return s_token[0] ? s_token : NULL;
}

#else /* CONFIG_APP_MODEM_CALL_MARK */

const char *modem_call_mark_last(bool unplanned)
{
	ARG_UNUSED(unplanned);
	return NULL;
}

#endif
