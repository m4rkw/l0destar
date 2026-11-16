/*
 * OBD-II mode 01 PIDs: the catalogue, the selection, the support bitmaps and
 * the scan request.  Pure C with no Zephyr includes, so it is checked on the
 * host: tests/test_obd_pids.py.  kline_obd.c does the bus work.
 *
 * The catalogue has one row per PID the tracker can decode.  Its wire integer
 * is round(value * scale), the same table the server holds (OBD_CATALOGUE in
 * server/tracker/obd_pids.py), so a value is sent as an integer and the packet
 * needs no float formatting.  The 16 original keys (orpm, ocl ...) keep their
 * names and scales; every other PID travels as o<2 hex digits>=<int>.
 *
 * SELECTION    tunable xop: PIDs as concatenated 2-digit hex, "0C05".
 * SCAN         reply token os=<0|1> asks for the support bitmaps; the answer
 *              is osp=<hex> on the next record (8 hex digits per 32 PIDs).
 */
#ifndef OBD_PIDS_H
#define OBD_PIDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define OBD_NOT_AVAILABLE INT32_MIN
#define OBD_SEL_MAX       12        /* PIDs in a selection */
#define OBD_BM_WORDS      7         /* support words 00 20 40 60 80 A0 C0 */

/* The live values of a poll.  Legacy PIDs keep their own fields; every other
 * selected PID is held in x[] (pid, wire integer). */
struct obd_snapshot {
	bool valid;
	int32_t rpm;            /* rpm            */
	int32_t speed;          /* km/h           */
	int32_t coolant;        /* deg C          */
	int32_t intake;         /* deg C          */
	int32_t load;           /* %      x10     */
	int32_t throttle;       /* %      x10     */
	int32_t maf;            /* g/s    x100    */
	int32_t timing;         /* deg    x10     */
	int32_t stft, ltft;     /* %      x10     */
	int32_t rpm_min;        /* rpm, over the cycle */
	int32_t rpm_max;
	int32_t rpm_avg;
	int32_t fuel_status;    /* raw bitmap     */
	int32_t mil;            /* 0 / 1          */
	int32_t dtc_count;      /* stored codes   */
	uint8_t nx;             /* entries used in xpid / xv */
	uint8_t xpid[OBD_SEL_MAX];
	int32_t xv[OBD_SEL_MAX];
};

/* -- the catalogue ----------------------------------------------------------- */

enum obd_fmt {
	F_RAW_A,        /* A                                 */
	F_PCT_A,        /* A * 100 / 255 %, x10              */
	F_TEMP_A,       /* A - 40 deg C                      */
	F_TRIM_A,       /* (A - 128) * 100 / 128 %, x10      */
	F_RPM,          /* (256 A + B) / 4                   */
	F_TIMING,       /* A / 2 - 64 deg, x10               */
	F_RAW_AB,       /* 256 A + B                         */
	F_O2_MV,        /* A * 5 mV (A / 200 V)              */
	F_FRAIL_REL,    /* 0.079 * (256 A + B) kPa, x10      */
	F_KPA_X10_AB,   /* 10 * (256 A + B) kPa              */
	F_CAT_TEMP,     /* (256 A + B) / 10 - 40 deg C, x10  */
	F_PCT_AB,       /* (256 A + B) * 100 / 255 %, x10    */
	F_EQUIV,        /* (256 A + B) * 2 / 65536, x1000    */
	F_FUEL_RATE,    /* (256 A + B) * 0.05 L/h, x100      */
	F_KPA3_A,       /* 3 * A kPa                         */
};

#define OBD_NO_LEGACY ((const char *)0)

struct obd_cat {
	uint8_t     pid;
	uint8_t     nbytes;         /* response data bytes needed */
	const char *name;
	const char *unit;
	int16_t     scale;          /* wire integer per unit */
	enum obd_fmt fmt;
	const char *legacy;         /* the original record key, or NULL */
};

static const struct obd_cat obd_catalogue[] = {
	{ 0x01, 1, "Monitor status",           "",     1,    F_RAW_A,      "omil"  },
	{ 0x03, 1, "Fuel system status",       "",     1,    F_RAW_A,      "ofs"   },
	{ 0x04, 1, "Engine load",              "%",    10,   F_PCT_A,      "old"   },
	{ 0x05, 1, "Coolant temperature",      "C",    1,    F_TEMP_A,     "ocl"   },
	{ 0x06, 1, "Short fuel trim B1",       "%",    10,   F_TRIM_A,     "ostft" },
	{ 0x07, 1, "Long fuel trim B1",        "%",    10,   F_TRIM_A,     "oltft" },
	{ 0x08, 1, "Short fuel trim B2",       "%",    10,   F_TRIM_A,     NULL    },
	{ 0x09, 1, "Long fuel trim B2",        "%",    10,   F_TRIM_A,     NULL    },
	{ 0x0A, 1, "Fuel pressure",            "kPa",  1,    F_KPA3_A,     NULL    },
	{ 0x0B, 1, "Intake manifold pressure", "kPa",  1,    F_RAW_A,      NULL    },
	{ 0x0C, 2, "Engine speed",             "rpm",  1,    F_RPM,        "orpm"  },
	{ 0x0D, 1, "Vehicle speed",            "km/h", 1,    F_RAW_A,      "ospd"  },
	{ 0x0E, 1, "Timing advance",           "deg",  10,   F_TIMING,     "otim"  },
	{ 0x0F, 1, "Intake air temperature",   "C",    1,    F_TEMP_A,     "oit"   },
	{ 0x10, 2, "Mass air flow",            "g/s",  100,  F_RAW_AB,     "omaf"  },
	{ 0x11, 1, "Throttle position",        "%",    10,   F_PCT_A,      "oth"   },
	{ 0x14, 1, "O2 sensor B1S1",           "V",    1000, F_O2_MV,      NULL    },
	{ 0x15, 1, "O2 sensor B1S2",           "V",    1000, F_O2_MV,      NULL    },
	{ 0x16, 1, "O2 sensor B1S3",           "V",    1000, F_O2_MV,      NULL    },
	{ 0x17, 1, "O2 sensor B1S4",           "V",    1000, F_O2_MV,      NULL    },
	{ 0x18, 1, "O2 sensor B2S1",           "V",    1000, F_O2_MV,      NULL    },
	{ 0x19, 1, "O2 sensor B2S2",           "V",    1000, F_O2_MV,      NULL    },
	{ 0x1A, 1, "O2 sensor B2S3",           "V",    1000, F_O2_MV,      NULL    },
	{ 0x1B, 1, "O2 sensor B2S4",           "V",    1000, F_O2_MV,      NULL    },
	{ 0x1F, 2, "Run time since start",     "s",    1,    F_RAW_AB,     NULL    },
	{ 0x21, 2, "Distance with MIL on",     "km",   1,    F_RAW_AB,     NULL    },
	{ 0x22, 2, "Fuel rail pressure (vac)", "kPa",  10,   F_FRAIL_REL,  NULL    },
	{ 0x23, 2, "Fuel rail pressure",       "kPa",  1,    F_KPA_X10_AB, NULL    },
	{ 0x2C, 1, "Commanded EGR",            "%",    10,   F_PCT_A,      NULL    },
	{ 0x2D, 1, "EGR error",                "%",    10,   F_TRIM_A,     NULL    },
	{ 0x2E, 1, "Evap purge",               "%",    10,   F_PCT_A,      NULL    },
	{ 0x2F, 1, "Fuel level",               "%",    10,   F_PCT_A,      NULL    },
	{ 0x30, 1, "Warm-ups since clear",     "",     1,    F_RAW_A,      NULL    },
	{ 0x31, 2, "Distance since clear",     "km",   1,    F_RAW_AB,     NULL    },
	{ 0x33, 1, "Barometric pressure",      "kPa",  1,    F_RAW_A,      NULL    },
	{ 0x3C, 2, "Catalyst temp B1S1",       "C",    10,   F_CAT_TEMP,   NULL    },
	{ 0x3D, 2, "Catalyst temp B2S1",       "C",    10,   F_CAT_TEMP,   NULL    },
	{ 0x3E, 2, "Catalyst temp B1S2",       "C",    10,   F_CAT_TEMP,   NULL    },
	{ 0x3F, 2, "Catalyst temp B2S2",       "C",    10,   F_CAT_TEMP,   NULL    },
	{ 0x42, 2, "Control module voltage",   "V",    1000, F_RAW_AB,     NULL    },
	{ 0x43, 2, "Absolute load",            "%",    10,   F_PCT_AB,     NULL    },
	{ 0x44, 2, "Commanded equivalence",    "",     1000, F_EQUIV,      NULL    },
	{ 0x45, 1, "Relative throttle",        "%",    10,   F_PCT_A,      NULL    },
	{ 0x46, 1, "Ambient temperature",      "C",    1,    F_TEMP_A,     NULL    },
	{ 0x47, 1, "Absolute throttle B",      "%",    10,   F_PCT_A,      NULL    },
	{ 0x48, 1, "Absolute throttle C",      "%",    10,   F_PCT_A,      NULL    },
	{ 0x49, 1, "Accelerator pedal D",      "%",    10,   F_PCT_A,      NULL    },
	{ 0x4A, 1, "Accelerator pedal E",      "%",    10,   F_PCT_A,      NULL    },
	{ 0x4B, 1, "Accelerator pedal F",      "%",    10,   F_PCT_A,      NULL    },
	{ 0x4C, 1, "Commanded throttle",       "%",    10,   F_PCT_A,      NULL    },
	{ 0x4D, 2, "Time with MIL on",         "min",  1,    F_RAW_AB,     NULL    },
	{ 0x4E, 2, "Time since codes cleared", "min",  1,    F_RAW_AB,     NULL    },
	{ 0x52, 1, "Ethanol fuel",             "%",    10,   F_PCT_A,      NULL    },
	{ 0x5C, 1, "Oil temperature",          "C",    1,    F_TEMP_A,     NULL    },
	{ 0x5E, 2, "Fuel rate",                "L/h",  100,  F_FUEL_RATE,  NULL    },
};

#define OBD_CAT_N (sizeof(obd_catalogue) / sizeof(obd_catalogue[0]))

static inline const struct obd_cat *obd_cat_find(uint8_t pid)
{
	for (size_t i = 0; i < OBD_CAT_N; i++) {
		if (obd_catalogue[i].pid == pid) {
			return &obd_catalogue[i];
		}
	}
	return NULL;
}

/* Wire integer from the response data bytes b[0..n).  false if the PID is not
 * in the catalogue or n is short. */
static inline bool obd_pid_decode(uint8_t pid, const uint8_t *b, int n, int32_t *out)
{
	const struct obd_cat *c = obd_cat_find(pid);

	if (!c || n < (int)c->nbytes) {
		return false;
	}

	int32_t a = b[0];
	int32_t ab = c->nbytes >= 2 ? a * 256 + b[1] : 0;

	switch (c->fmt) {
	case F_RAW_A:      *out = a;                          break;
	case F_PCT_A:      *out = a * 1000 / 255;             break;
	case F_TEMP_A:     *out = a - 40;                     break;
	case F_TRIM_A:     *out = (a - 128) * 1000 / 128;     break;
	case F_RPM:        *out = ab / 4;                     break;
	case F_TIMING:     *out = a * 5 - 640;                break;
	case F_RAW_AB:     *out = ab;                         break;
	case F_O2_MV:      *out = a * 5;                      break;
	case F_FRAIL_REL:  *out = (int32_t)(((int64_t)ab * 79 + 50) / 100); break;
	case F_KPA_X10_AB: *out = ab * 10;                    break;
	case F_CAT_TEMP:   *out = ab - 400;                   break;
	case F_PCT_AB:     *out = (int32_t)((int64_t)ab * 1000 / 255); break;
	case F_EQUIV:      *out = (int32_t)(((int64_t)ab * 2000 + 32768) / 65536); break;
	case F_FUEL_RATE:  *out = ab * 5;                     break;
	case F_KPA3_A:     *out = a * 3;                      break;
	default:           return false;
	}
	return true;
}

/* -- the selection ----------------------------------------------------------- */

struct obd_sel {
	uint8_t n;
	uint8_t pid[OBD_SEL_MAX];
};

static inline int obd_hexval(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	return -1;
}

static inline bool obd_sel_has(const struct obd_sel *s, uint8_t pid)
{
	for (unsigned int i = 0; i < s->n; i++) {
		if (s->pid[i] == pid) {
			return true;
		}
	}
	return false;
}

/* "0C05" -> {0x0C, 0x05}, in order.  A pair that is not two hex digits, a PID
 * that is not in the catalogue and a repeat are dropped; at most OBD_SEL_MAX
 * are kept.  Never reads past the NUL.  Returns the count (the list may be
 * empty). */
static inline int obd_sel_parse(const char *s, struct obd_sel *out)
{
	out->n = 0;
	for (size_t i = 0; s && s[i] && s[i + 1] && out->n < OBD_SEL_MAX; i += 2) {
		int hi = obd_hexval(s[i]), lo = obd_hexval(s[i + 1]);

		if (hi < 0 || lo < 0) {
			continue;       /* not a token: look again from the next pair */
		}

		uint8_t pid = (uint8_t)(hi * 16 + lo);

		if (obd_cat_find(pid) && !obd_sel_has(out, pid)) {
			out->pid[out->n++] = pid;
		}
	}
	return out->n;
}

/* The selection a unit uses: `tun` if it yields at least one PID, else the
 * built default `def`, else (a bad default) 0C05. */
static inline void obd_sel_effective(const char *tun, const char *def, struct obd_sel *out)
{
	if (obd_sel_parse(tun, out) == 0 && obd_sel_parse(def, out) == 0) {
		out->n = 2;
		out->pid[0] = 0x0C;
		out->pid[1] = 0x05;
	}
}

/* The PID whose selection turns on a record key ("orpm" -> 0x0C), 0 if the
 * key is not one of the original sixteen. */
static inline uint8_t obd_legacy_pid(const char *key)
{
	for (size_t i = 0; i < OBD_CAT_N; i++) {
		if (obd_catalogue[i].legacy && strcmp(obd_catalogue[i].legacy, key) == 0) {
			return obd_catalogue[i].pid;
		}
	}
	if (strcmp(key, "ormin") == 0 || strcmp(key, "ormax") == 0 || strcmp(key, "oravg") == 0) {
		return 0x0C;
	}
	if (strcmp(key, "odtc") == 0) {
		return 0x01;
	}
	return 0;
}

/* -- the snapshot, by PID ---------------------------------------------------- */

static inline int32_t *obd_snap_legacy(struct obd_snapshot *s, uint8_t pid)
{
	switch (pid) {
	case 0x03: return &s->fuel_status;
	case 0x04: return &s->load;
	case 0x05: return &s->coolant;
	case 0x06: return &s->stft;
	case 0x07: return &s->ltft;
	case 0x0C: return &s->rpm;
	case 0x0D: return &s->speed;
	case 0x0E: return &s->timing;
	case 0x0F: return &s->intake;
	case 0x10: return &s->maf;
	case 0x11: return &s->throttle;
	default:   return NULL;
	}
}

static inline void obd_snap_clear(struct obd_snapshot *s)
{
	memset(s, 0, sizeof(*s));
	s->rpm = s->speed = s->coolant = s->intake = OBD_NOT_AVAILABLE;
	s->load = s->throttle = s->timing = s->stft = s->ltft = OBD_NOT_AVAILABLE;
	s->maf = s->fuel_status = s->mil = s->dtc_count = OBD_NOT_AVAILABLE;
	s->rpm_min = s->rpm_max = s->rpm_avg = OBD_NOT_AVAILABLE;
}

/* Set the value of a PID other than 0x01 (which is two fields, set by the
 * caller).  A PID with no field of its own goes in x[]; false if that is full. */
static inline bool obd_snap_set(struct obd_snapshot *s, uint8_t pid, int32_t v)
{
	int32_t *f = obd_snap_legacy(s, pid);

	if (f) {
		*f = v;
		return true;
	}
	for (unsigned int i = 0; i < s->nx; i++) {
		if (s->xpid[i] == pid) {
			s->xv[i] = v;
			return true;
		}
	}
	if (s->nx >= OBD_SEL_MAX) {
		return false;
	}
	s->xpid[s->nx] = pid;
	s->xv[s->nx++] = v;
	return true;
}

/* Mark a PID's value as not read (stale). */
static inline void obd_snap_forget(struct obd_snapshot *s, uint8_t pid)
{
	if (pid == 0x01) {
		s->mil = s->dtc_count = OBD_NOT_AVAILABLE;
		return;
	}
	(void)obd_snap_set(s, pid, OBD_NOT_AVAILABLE);
}

/* -- the record fields ------------------------------------------------------- */

/* Append ",k=v;k=v" for the values that were read and whose PID is selected:
 * the original keys first in their old order, then o<hex>= for the others in
 * the order of the selection.  Returns the bytes written. */
static inline int obd_format(char *buf, int max, const struct obd_snapshot *s,
			     const struct obd_sel *sel)
{
	static const struct {
		const char *key;
		size_t off;
	} map[] = {
		{ "orpm",  offsetof(struct obd_snapshot, rpm)         },
		{ "ormin", offsetof(struct obd_snapshot, rpm_min)     },
		{ "ormax", offsetof(struct obd_snapshot, rpm_max)     },
		{ "oravg", offsetof(struct obd_snapshot, rpm_avg)     },
		{ "ospd",  offsetof(struct obd_snapshot, speed)       },
		{ "ocl",   offsetof(struct obd_snapshot, coolant)     },
		{ "oit",   offsetof(struct obd_snapshot, intake)      },
		{ "old",   offsetof(struct obd_snapshot, load)        },
		{ "oth",   offsetof(struct obd_snapshot, throttle)    },
		{ "omaf",  offsetof(struct obd_snapshot, maf)         },
		{ "otim",  offsetof(struct obd_snapshot, timing)      },
		{ "ostft", offsetof(struct obd_snapshot, stft)        },
		{ "oltft", offsetof(struct obd_snapshot, ltft)        },
		{ "ofs",   offsetof(struct obd_snapshot, fuel_status) },
		{ "omil",  offsetof(struct obd_snapshot, mil)         },
		{ "odtc",  offsetof(struct obd_snapshot, dtc_count)   },
	};
	int len = 0;
	bool first = true;

	if (!s->valid || max <= 0) {
		return 0;
	}

	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]) + sel->n; i++) {
		char key[8];
		int32_t v = OBD_NOT_AVAILABLE;

		if (i < sizeof(map) / sizeof(map[0])) {
			if (!obd_sel_has(sel, obd_legacy_pid(map[i].key))) {
				continue;
			}
			v = *(const int32_t *)((const char *)s + map[i].off);
			snprintf(key, sizeof(key), "%s", map[i].key);
		} else {
			/* In the order of the selection; legacy PIDs have their own keys. */
			uint8_t pid = sel->pid[i - sizeof(map) / sizeof(map[0])];

			for (unsigned int x = 0; x < s->nx; x++) {
				if (s->xpid[x] == pid) {
					v = s->xv[x];
				}
			}
			snprintf(key, sizeof(key), "o%02X", pid);
		}
		if (v == OBD_NOT_AVAILABLE) {
			continue;
		}

		int n = snprintf(&buf[len], max - len, "%c%s=%d", first ? ',' : ';', key, (int)v);

		if (n <= 0 || n >= max - len) {
			break;          /* out of room: emit what fits */
		}
		len += n;
		first = false;
	}
	buf[len] = '\0';           /* a cut-off item must not be left behind */
	return len;
}

/* Which PIDs the poll reads: the selected ones, engine speed and vehicle speed
 * (the tracker's own state), the status word (the stored-code watch) and the
 * ones an alert rule names (alert_mask, bit n = PID n for PIDs 0..31). */
static inline bool obd_pid_wanted(const struct obd_sel *sel, uint32_t alert_mask, uint8_t pid)
{
	return pid == 0x0C || pid == 0x0D || pid == 0x01 || obd_sel_has(sel, pid) ||
	       (pid < 32 && (alert_mask & (1u << pid)) != 0);
}

/* -- the support bitmaps ------------------------------------------------------ */

/* bm[w] is the 4-byte answer to PID 0x20 * w, MSB first.  Bit (pid - 1) % 32
 * from the MSB of word (pid - 1) / 32 says PID pid is supported. */
static inline bool obd_bm_supported(const uint8_t bm[][4], int nwords, uint8_t pid)
{
	if (pid < 1 || pid > 0xE0) {
		return false;
	}

	int i = pid - 1, w = i / 32;

	if (w >= nwords) {
		return false;
	}
	return (bm[w][(i % 32) / 8] & (0x80 >> (i % 8))) != 0;
}

/* The last bit of a word (PID 0x20, 0x40 ...) says the next word exists. */
static inline bool obd_bm_more(const uint8_t word[4])
{
	return (word[3] & 0x01) != 0;
}

/* Uppercase hex of the words, 8 digits each, trailing all-zero words dropped
 * but at least one kept.  `size` must be at least 8 * nwords + 1 to hold it
 * all; returns the length, or 0 (empty string) if nwords < 1 or it will not
 * fit. */
static inline size_t obd_bm_hex(const uint8_t bm[][4], int nwords, char *out, size_t size)
{
	static const char hex[] = "0123456789ABCDEF";
	int keep = nwords;

	if (size == 0) {
		return 0;
	}
	out[0] = '\0';
	if (nwords < 1 || nwords > OBD_BM_WORDS) {
		return 0;
	}
	while (keep > 1 && (bm[keep - 1][0] | bm[keep - 1][1] | bm[keep - 1][2] | bm[keep - 1][3]) == 0) {
		keep--;
	}
	if ((size_t)keep * 8 + 1 > size) {
		return 0;
	}
	for (int w = 0; w < keep; w++) {
		for (int b = 0; b < 4; b++) {
			out[w * 8 + b * 2]     = hex[bm[w][b] >> 4];
			out[w * 8 + b * 2 + 1] = hex[bm[w][b] & 15];
		}
	}
	out[keep * 8] = '\0';
	return (size_t)keep * 8;
}

/* -- the os=<0|1> reply token and the scan request ---------------------------- */

/* Takes every "os=<0|1>" token (at the start or after a comma, ending at a
 * comma or the end) out of cmd, as install_take_im() does for im=.  Returns
 * the value of the first, or -1 when there is none or its value is not 0 or 1
 * (that token is then left alone). */
static inline int obd_os_take(char *cmd)
{
	int result = -1;
	char *p = cmd;

	while (*p) {
		char *comma = strchr(p, ',');
		size_t len = comma ? (size_t)(comma - p) : strlen(p);

		if (len == 4 && strncmp(p, "os=", 3) == 0 && (p[3] == '0' || p[3] == '1')) {
			if (result < 0) {
				result = p[3] - '0';
			}
			if (comma) {
				memmove(p, comma + 1, strlen(comma + 1) + 1);
				continue;
			}
			if (p > cmd) {
				p[-1] = '\0';
			} else {
				*p = '\0';
			}
			break;
		}
		if (!comma) {
			break;
		}
		p = comma + 1;
	}
	return result;
}

/* One scan per request.  The server repeats os=1 on every reply until it has
 * processed the answer, so a scan is started by a change from os=0 (or from
 * nothing: a fresh boot) to os=1, not by every os=1.  os=0 withdraws a request.
 * A request that is still being repeated OBD_SCAN_RETRY_MS after its answer
 * was queued is taken as lost and made again; a failed scan (engine off, no
 * answer) is tried again after OBD_SCAN_BACKOFF_MS. */
#define OBD_SCAN_RETRY_MS    (30 * 60 * 1000LL)
#define OBD_SCAN_BACKOFF_MS  (30 * 1000LL)

struct obd_scan {
	bool    want;           /* a scan is owed */
	bool    last_os;        /* the previous reply said 1 */
	bool    done;           /* a scan has been answered since boot */
	int64_t done_ms;
	int64_t next_ms;        /* not before */
};

static inline void obd_scan_init(struct obd_scan *st)
{
	memset(st, 0, sizeof(*st));
}

/* A reply's os= value (0 or 1). */
static inline void obd_scan_reply(struct obd_scan *st, int os, int64_t now_ms)
{
	if (os == 1) {
		if (!st->last_os) {
			st->want = true;
		} else if (!st->want && st->done && now_ms - st->done_ms >= OBD_SCAN_RETRY_MS) {
			st->want = true;
		}
		st->last_os = true;
	} else if (os == 0) {
		st->last_os = false;
		st->want = false;
	}
}

static inline bool obd_scan_due(const struct obd_scan *st, int64_t now_ms)
{
	return st->want && now_ms >= st->next_ms;
}

static inline void obd_scan_failed(struct obd_scan *st, int64_t now_ms)
{
	st->next_ms = now_ms + OBD_SCAN_BACKOFF_MS;
}

static inline void obd_scan_finished(struct obd_scan *st, int64_t now_ms)
{
	st->want = false;
	st->done = true;
	st->done_ms = now_ms;
}

#endif
