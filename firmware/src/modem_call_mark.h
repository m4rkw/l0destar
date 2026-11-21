/*
 * The last call into the modem library, kept across a reset.  Pure, so it is
 * checked on the host: tests/test_modem_call_mark.py.  modem_call_mark.c holds
 * the state and reads it back at boot.
 *
 * Drive-time freezes happen in registration loss and weak cells, and some end
 * in a plain rst=wdt with no stall and no freeze note: the CPU was stuck
 * somewhere the watchdog's early warning cannot run, most likely waiting on or
 * inside the nrf_modem library (IPC to the modem core, an AT command, a
 * socket).  So every such call leaves a mark in noinit RAM on the way in and
 * clears it on the way out.  A mark still set after a watchdog reset is the
 * call the firmware was in.
 *
 * One slot per class of call rather than one for all, because several threads
 * call the modem at once and a quick call in one must not wipe out the slow one
 * another is stuck in.  A slot is written by one thread at a time in practice;
 * if two ever share a class the later one wins, which loses a stuck call at
 * worst, never invents one.  Nothing locks: a few stores, so it is cheap and
 * safe in a handler.
 *
 * rst= gets one token for the newest call still set, "mc:<name>", with the
 * name from the tables below: [a-z0-9-] after the colon.
 */
#ifndef MODEM_CALL_MARK_H
#define MODEM_CALL_MARK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Changes with the layout, so a record left by another image is ignored. */
#define MCM_MAGIC   0x3C4A11C0u

/* Classes of call: one slot each. */
enum mcm_class {
	MCM_C_SOCK,     /* the transport thread's socket calls */
	MCM_C_SOCK2,    /* any other thread's (agnss, traccar) */
	MCM_C_AT,       /* nrf_modem_at_*: detail = the command's first 4 letters */
	MCM_C_LTE,      /* lte_lc_* and modem_info_* */
	MCM_C_INIT,     /* modem library init and shutdown */
	MCM_C_HND,      /* inside an event handler: what = event type + 1 */
	MCM_C_GNSS,     /* nrf_modem_gnss_* */
	MCM_C_COUNT
};

/* what, per class.  0 is never used, so a zeroed slot is empty. */
enum mcm_sock { MCM_SOCK_SOCKET = 1, MCM_SOCK_CONN, MCM_SOCK_SEND, MCM_SOCK_RECV,
		MCM_SOCK_POLL, MCM_SOCK_CLOSE, MCM_SOCK_OPT, MCM_SOCK_DNS, MCM_SOCK_SENDTO };
enum mcm_at { MCM_AT_ANY = 1 };
enum mcm_lte { MCM_LTE_SYSMODE = 1, MCM_LTE_FUNC, MCM_LTE_NORMAL, MCM_LTE_OFFLINE,
	       MCM_LTE_PWROFF, MCM_LTE_PSM, MCM_LTE_MODE, MCM_LTE_REG, MCM_LTE_EVAL,
	       MCM_LTE_NCELL, MCM_LTE_NCANCEL, MCM_LTE_INFO };
enum mcm_init { MCM_INIT_INIT = 1, MCM_INIT_SHUT };
enum mcm_gnss { MCM_GNSS_READ = 1, MCM_GNSS_START, MCM_GNSS_STOP, MCM_GNSS_CFG, MCM_GNSS_PRIO };

struct mcm_slot {
	uint32_t detail;    /* AT: the command code (mcm_at_code); else 0 */
	uint16_t seq;       /* ordering across slots: newest = highest */
	uint8_t what;
	uint8_t entered;    /* 1 between enter and leave */
};

struct mcm_state {
	uint32_t magic;
	uint16_t seq;
	uint16_t pad;
	struct mcm_slot slot[MCM_C_COUNT];
};

/* "CFUN" from "AT+CFUN=4", "XMON" from "AT%XMONITOR": up to four letters or
 * digits after the AT and whatever punctuation follows it, base 37 (0 = none),
 * first character most significant.  Takes a printf format too, so the command
 * can be passed as it is written. */
static inline uint32_t mcm_at_code(const char *cmd)
{
	uint32_t code = 0;
	int n = 0;

	if (!cmd) {
		return 0;
	}
	if ((cmd[0] == 'A' || cmd[0] == 'a') && (cmd[1] == 'T' || cmd[1] == 't')) {
		cmd += 2;
	}
	for (; *cmd && n < 4; cmd++) {
		char c = *cmd;
		uint32_t v;

		if (c >= '0' && c <= '9') {
			v = (uint32_t)(c - '0') + 1;
		} else if (c >= 'A' && c <= 'Z') {
			v = (uint32_t)(c - 'A') + 11;
		} else if (c >= 'a' && c <= 'z') {
			v = (uint32_t)(c - 'a') + 11;
		} else if (n == 0) {
			continue;               /* the + or % in front */
		} else {
			break;                  /* = ? or a format's own % */
		}
		code = code * 37 + v;
		n++;
	}
	for (; n < 4; n++) {
		code *= 37;
	}
	return code;
}

/* The lowercase letters of a code, "cfun"; "" for none. */
static inline void mcm_at_name(uint32_t code, char *buf, size_t n)
{
	char tmp[5] = { 0 };

	if (n == 0) {
		return;
	}
	for (int i = 3; i >= 0; i--) {
		uint32_t v = code % 37;

		code /= 37;
		tmp[i] = v == 0 ? '\0' : v <= 10 ? (char)('0' + v - 1) : (char)('a' + v - 11);
	}
	snprintf(buf, n, "%s", tmp);
}

/* Enter and leave: a few stores, in an order that leaves a reset in the middle
 * reading as "not entered" or as the call itself, never as a mixture. */
static inline void mcm_enter(struct mcm_state *s, unsigned cls, uint8_t what, uint32_t detail)
{
	struct mcm_slot *p = &s->slot[cls];

	p->entered = 0;
	p->what = what;
	p->detail = detail;
	p->seq = ++s->seq;
	__asm__ volatile ("" ::: "memory");
	p->entered = 1;
}

static inline void mcm_leave(struct mcm_state *s, unsigned cls)
{
	__asm__ volatile ("" ::: "memory");
	s->slot[cls].entered = 0;
}

static inline void mcm_clear(struct mcm_state *s)
{
	memset(s, 0, sizeof(*s));
	s->magic = MCM_MAGIC;
}

/* The slot to report: the newest of those still entered, or -1.  Sequence
 * numbers wrap at 65536 calls, compared as signed distance. */
static inline int mcm_newest(const struct mcm_state *s)
{
	int best = -1;

	if (s->magic != MCM_MAGIC) {
		return -1;
	}
	for (int i = 0; i < MCM_C_COUNT; i++) {
		const struct mcm_slot *p = &s->slot[i];

		if (p->entered != 1 || p->what == 0) {
			continue;
		}
		if (best < 0 || (int16_t)(uint16_t)(p->seq - s->slot[best].seq) > 0) {
			best = i;
		}
	}
	return best;
}

/* lte_lc event type to a short name; NULL for one not in the list.  Keyed by
 * the numbers of enum lte_lc_evt_type in NCS 3.3 (0 to 12).  A later SDK that
 * renumbers them only gives a wrong label. */
static inline const char *mcm_event_name(unsigned type)
{
	static const char *const names[] = {
		"reg", "psm", "edrx", "rrc", "cell", "mode", "tau", "ncell",
		"sleepw", "sleepx", "sleep", "mevt", "rai",
	};

	return type < sizeof(names) / sizeof(names[0]) ? names[type] : NULL;
}

/* The name after "mc:", e.g. "send", "at-cfun", "hnd-reg", "lte-normal". */
static inline void mcm_name(const struct mcm_state *s, int idx, char *buf, size_t n)
{
	static const char *const sock[] = { "?", "sock", "conn", "send", "recv", "poll",
					    "close", "opt", "dns", "sendto" };
	static const char *const lte[] = { "?", "lte-sysmode", "lte-func", "lte-normal",
					   "lte-offline", "lte-pwroff", "lte-psm", "lte-mode",
					   "lte-reg", "lte-eval", "lte-ncell", "lte-ncancel",
					   "lte-info" };
	static const char *const gnss[] = { "?", "gnss-read", "gnss-start", "gnss-stop",
					    "gnss-cfg", "gnss-prio" };
	const struct mcm_slot *p;
	unsigned w;

	if (n == 0) {
		return;
	}
	buf[0] = '\0';
	if (idx < 0 || idx >= MCM_C_COUNT) {
		return;
	}
	p = &s->slot[idx];
	w = p->what;
	switch (idx) {
	case MCM_C_SOCK:
	case MCM_C_SOCK2:
		snprintf(buf, n, "%s", w < sizeof(sock) / sizeof(sock[0]) ? sock[w] : "sock?");
		break;
	case MCM_C_AT: {
		char code[8];

		mcm_at_name(p->detail, code, sizeof(code));
		snprintf(buf, n, "at-%s", code[0] ? code : "x");
		break;
	}
	case MCM_C_LTE:
		snprintf(buf, n, "%s", w < sizeof(lte) / sizeof(lte[0]) ? lte[w] : "lte?");
		break;
	case MCM_C_INIT:
		snprintf(buf, n, "%s", w == MCM_INIT_SHUT ? "lib-shut" : "lib-init");
		break;
	case MCM_C_HND: {
		const char *e = w ? mcm_event_name(w - 1) : NULL;

		if (e) {
			snprintf(buf, n, "hnd-%s", e);
		} else {
			snprintf(buf, n, "hnd-%u", w ? w - 1 : 0u);
		}
		break;
	}
	case MCM_C_GNSS:
		snprintf(buf, n, "%s", w < sizeof(gnss) / sizeof(gnss[0]) ? gnss[w] : "gnss?");
		break;
	}
}

/* "mc:send" for rst=, or "" when no call was entered. */
static inline void mcm_token(const struct mcm_state *s, char *buf, size_t n)
{
	int idx = mcm_newest(s);
	char name[16];

	if (n == 0) {
		return;
	}
	buf[0] = '\0';
	if (idx >= 0) {
		mcm_name(s, idx, name, sizeof(name));
		snprintf(buf, n, "mc:%s", name);
	}
}

/* What the macros call: the state, the slot's class and what, and for an AT
 * command the command itself (any other class passes NULL). */
static inline void mcm_note_into(struct mcm_state *s, unsigned cls, uint8_t what, const char *cmd)
{
	mcm_enter(s, cls, what, cls == MCM_C_AT ? mcm_at_code(cmd) : 0);
}

#ifdef CONFIG_APP_MODEM_CALL_MARK

/* Out of line (modem_call_mark.c), so a call site is two short calls rather
 * than a dozen inline stores. */
void mcm_note(unsigned cls, uint8_t what, const char *cmd);
void mcm_done(unsigned cls);

/* The expression's value comes back unchanged. */
#define MCM_CALL(cls, what, cmd, expr)                                         \
	({                                                                     \
		mcm_note((cls), (what), (cmd));                                \
		__typeof__(expr) mcm_r_ = (expr);                              \
		mcm_done((cls));                                               \
		mcm_r_;                                                        \
	})

/* The same for a call that returns nothing. */
#define MCM_CALLV(cls, what, cmd, expr)                                        \
	do {                                                                   \
		mcm_note((cls), (what), (cmd));                                \
		expr;                                                          \
		mcm_done((cls));                                               \
	} while (0)

#define MCM_ENTER(cls, what)  mcm_note((cls), (what), NULL)
#define MCM_LEAVE(cls)        mcm_done((cls))

#else

#define MCM_CALL(cls, what, cmd, expr)   (expr)
#define MCM_CALLV(cls, what, cmd, expr)  do { expr; } while (0)
#define MCM_ENTER(cls, what)             do { } while (0)
#define MCM_LEAVE(cls)                   do { } while (0)

#endif

/* The call sites, one per library function. */
#define MCM_SOCK(what, expr)     MCM_CALL(MCM_C_SOCK, (what), NULL, expr)
#define MCM_SOCK2(what, expr)    MCM_CALL(MCM_C_SOCK2, (what), NULL, expr)
#define MCM_LTE(what, expr)      MCM_CALL(MCM_C_LTE, (what), NULL, expr)
#define MCM_LTEV(what, expr)     MCM_CALLV(MCM_C_LTE, (what), NULL, expr)
#define MCM_GNSS(what, expr)     MCM_CALL(MCM_C_GNSS, (what), NULL, expr)
#define MCM_GNSSV(what, expr)    MCM_CALLV(MCM_C_GNSS, (what), NULL, expr)

/* An AT command, named by its format or string (which cost nothing to read
 * twice).  The first argument of each is the one that holds the command. */
#define MCM_AT_PRINTF(fmt, ...)                                                \
	MCM_CALL(MCM_C_AT, MCM_AT_ANY, (fmt),                       \
		 nrf_modem_at_printf(fmt, ##__VA_ARGS__))
#define MCM_AT_CMD(buf, len, fmt, ...)                                         \
	MCM_CALL(MCM_C_AT, MCM_AT_ANY, (fmt),                       \
		 nrf_modem_at_cmd(buf, len, fmt, ##__VA_ARGS__))
#define MCM_AT_SCANF(cmd, fmt, ...)                                            \
	MCM_CALL(MCM_C_AT, MCM_AT_ANY, (cmd),                       \
		 nrf_modem_at_scanf(cmd, fmt, ##__VA_ARGS__))

#endif /* MODEM_CALL_MARK_H */
