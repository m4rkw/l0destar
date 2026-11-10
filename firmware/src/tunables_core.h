/*
 * Runtime overrides of build-time settings ("tunables"), the parser and the
 * range limits.  Pure C with no Zephyr includes, so it is checked on the host:
 * tests/test_tunables.py.  tunables.c holds the built (Kconfig) values and
 * the accessors the firmware calls.
 *
 * The server sends the whole set on every reply as
 *     tu=<code>:<value>[;<code>:<value>...]      or tu=-  for none
 * and the table is made equal to the pairs listed: a key not listed goes back
 * to its built value, an unknown code or a value out of range is ignored.
 * The one string, xoa, has its commas sent as '|'.
 */
#ifndef TUNABLES_CORE_H
#define TUNABLES_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum tun_key {
	TUN_XTT,	/* tow tilt, degrees */
	TUN_XPI,	/* parked impact, mg */
	TUN_XII,	/* immediate impact, mg (0 = never) */
	TUN_XAP,	/* accel alert priority */
	TUN_XAB,	/* accel alert backoff, s */
	TUN_XIS,	/* ignition-on sleep interval, s */
	TUN_XBW,	/* battery warning, mV */
	TUN_XBP,	/* battery power-off, mV */
	TUN_XDS,	/* drive stream on/off */
	TUN_XDI,	/* drive stream interval, s (0 = as fast as possible) */
	TUN_XTP,	/* track period, ms */
	TUN_XDD,	/* databuf drain, s */
	TUN_XNS,	/* network search timeout, s */
	TUN_XRP,	/* response poll, s */
	TUN_XLW,	/* LED on accel wake */
	TUN_N_INT
};

#define TUN_OA_MAX 120		/* characters of the OBD alert rule list */

static const struct {
	char code[4];
	int32_t min, max;
} tun_limits[TUN_N_INT] = {
	[TUN_XTT] = {"xtt", 0, 30},
	[TUN_XPI] = {"xpi", 300, 2500},
	[TUN_XII] = {"xii", 0, 1200},
	[TUN_XAP] = {"xap", -2, 2},
	[TUN_XAB] = {"xab", 0, 86400},
	[TUN_XIS] = {"xis", 10, 600},
	[TUN_XBW] = {"xbw", 11000, 12500},
	[TUN_XBP] = {"xbp", 10500, 12200},
	[TUN_XDS] = {"xds", 0, 1},
	[TUN_XDI] = {"xdi", 0, 900},
	[TUN_XTP] = {"xtp", 200, 5000},
	[TUN_XDD] = {"xdd", 1, 600},
	[TUN_XNS] = {"xns", 60, 86400},
	[TUN_XRP] = {"xrp", 5, 600},
	[TUN_XLW] = {"xlw", 0, 1},
};

/* The poweroff level may be at most this far under the warning level. */
#define TUN_BATTERY_GAP_MV 100

struct tun_table {
	bool    set[TUN_N_INT];
	int32_t val[TUN_N_INT];
	bool    oa_set;
	char    oa[TUN_OA_MAX + 1];
};

static inline void tun_table_clear(struct tun_table *t)
{
	memset(t, 0, sizeof(*t));
}

/* A whole decimal integer in s[0..len), optional leading '-'. */
static inline bool tun_parse_int(const char *s, size_t len, int32_t *out)
{
	size_t i = 0;
	bool neg = false;
	int64_t v = 0;

	if (len > 0 && s[0] == '-') {
		neg = true;
		i = 1;
	}
	if (i >= len || len - i > 9) {
		return false;
	}
	for (; i < len; i++) {
		if (s[i] < '0' || s[i] > '9') {
			return false;
		}
		v = v * 10 + (s[i] - '0');
	}
	*out = (int32_t)(neg ? -v : v);
	return true;
}

/* Make *t equal to the pairs in list[0..len).  "-" or nothing is the empty
 * set.  Never reads past len, never writes outside *t. */
static inline void tun_table_apply(struct tun_table *t, const char *list, size_t len)
{
	size_t pos = 0;

	tun_table_clear(t);
	while (pos < len) {
		const char *seg = list + pos;
		size_t n = 0;

		while (pos + n < len && seg[n] != ';') {
			n++;
		}
		pos += n + 1;

		if (n < 5 || seg[3] != ':') {		/* "xxx:v" at least */
			continue;
		}
		const char *v = seg + 4;
		size_t vlen = n - 4;

		for (int k = 0; k < TUN_N_INT; k++) {
			if (memcmp(seg, tun_limits[k].code, 3) != 0) {
				continue;
			}
			int32_t x;

			if (tun_parse_int(v, vlen, &x) &&
			    x >= tun_limits[k].min && x <= tun_limits[k].max) {
				t->set[k] = true;
				t->val[k] = x;
			} else {
				t->set[k] = false;
			}
			break;
		}
		if (memcmp(seg, "xoa", 3) == 0) {
			bool ok = vlen > 0 && vlen <= TUN_OA_MAX;

			for (size_t i = 0; ok && i < vlen; i++) {
				ok = v[i] >= 0x20 && v[i] < 0x7f;
			}
			if (ok) {
				for (size_t i = 0; i < vlen; i++) {
					t->oa[i] = v[i] == '|' ? ',' : v[i];
				}
				t->oa[vlen] = '\0';
				t->oa_set = true;
			} else {
				t->oa_set = false;
				t->oa[0] = '\0';
			}
		}
	}
}

/* The effective value of integer key k: the override if set, else `built`.
 * The battery pair is held together: poweroff at most warning - 100 mV. */
static inline int32_t tun_table_get(const struct tun_table *t, enum tun_key k,
				    const int32_t built[TUN_N_INT])
{
	int32_t v = t->set[k] ? t->val[k] : built[k];

	if (k == TUN_XBP && (t->set[TUN_XBP] || t->set[TUN_XBW])) {
		int32_t w = t->set[TUN_XBW] ? t->val[TUN_XBW] : built[TUN_XBW];

		if (v > w - TUN_BATTERY_GAP_MV) {
			v = w - TUN_BATTERY_GAP_MV;
		}
	}
	return v;
}

/* The built values as the "tb=" record field, in the tu= grammar: all fifteen
 * integer keys plus xoa (commas sent as '|', "xoa:" when empty), ';'-separated
 * in the order xtt xpi xii xap xab xoa xis xbw xbp xds xdi xtp xdd xns xrp xlw.
 * Worst case with every integer in its limits and a 120-char xoa is 251 bytes
 * plus the NUL; TUN_BUILT_BUF leaves room for a few out-of-range Kconfig
 * values.  Returns the length, or -1 (out[0] = '\0') if it does not fit or the
 * xoa is longer than TUN_OA_MAX. */
#define TUN_BUILT_BUF 272

static inline int tun_format_built(char *out, size_t cap, const int32_t built[TUN_N_INT],
				   const char *oa)
{
	size_t n = 0;
	size_t oalen = strlen(oa);

	if (cap == 0) {
		return -1;
	}
	out[0] = '\0';
	if (oalen > TUN_OA_MAX) {
		return -1;
	}
	for (int k = 0; k < TUN_N_INT; k++) {
		char num[16];
		size_t nl = 0;
		int32_t v = built[k];
		uint32_t u = v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
		char rev[12];
		size_t r = 0;

		do {
			rev[r++] = (char)('0' + u % 10);
			u /= 10;
		} while (u);
		if (v < 0) {
			num[nl++] = '-';
		}
		while (r) {
			num[nl++] = rev[--r];
		}
		/* "xxx:" + digits + ";" + the NUL */
		if (n + 4 + nl + 1 + 1 > cap) {
			out[0] = '\0';
			return -1;
		}
		if (n) {
			out[n++] = ';';
		}
		memcpy(out + n, tun_limits[k].code, 3);
		out[n + 3] = ':';
		memcpy(out + n + 4, num, nl);
		n += 4 + nl;
		if (k == TUN_XAB) {
			if (n + 1 + 4 + oalen + 1 > cap) {
				out[0] = '\0';
				return -1;
			}
			out[n++] = ';';
			memcpy(out + n, "xoa:", 4);
			n += 4;
			for (size_t i = 0; i < oalen; i++) {
				out[n++] = oa[i] == ',' ? '|' : oa[i];
			}
		}
	}
	out[n] = '\0';
	return (int)n;
}

#endif
