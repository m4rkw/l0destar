/*
 * Delta records for the drive stream: a record after the first of its datagram
 * with what the record before it already said left out.  Pure, so it is checked
 * on the host: tests/test_record_delta.py builds this and holds its output to
 * server/tracker/deltarec.py's encode(), byte for byte, and reads it back
 * with the server's decoder.  The wire format is in server/docs/PROTOCOL.md
 * (Delta records).
 *
 * The stream sends up to four records a datagram and most of a record is the
 * one before it again: the altitude, HDOP and satellite count, the die
 * temperatures and supply, the signal reading, and the OBD figures the K wire
 * refreshes once a second.  At a metered 3.9 records a second that was a third
 * of the data the stream used.
 *
 * What the encoder does to a record, given the whole record before it in the
 * same datagram:
 *
 *   - spd, alt, hdg, hdop, sat, bat and up that equal the previous record's are
 *     left empty;
 *   - the sticky extras (it, mt, vs, up, the signal reading and the OBD keys)
 *     that equal the previous record's are not sent;
 *   - "d=1" goes on the end.
 *
 * It does neither when the record could not be read back the same: when a
 * sticky extra the previous record had is gone from this one (the server would
 * take "not sent" for "unchanged"), or when the result would not be shorter.
 * Then the record goes whole.  The first record of a datagram always does, and
 * nothing is carried from one datagram to the next, so a datagram that is lost,
 * repeated or late decodes as it would have in order.
 *
 * The caller keeps the whole records for the backlog and for a datagram that
 * has to be sent again: this is only ever what goes on the air.
 */
#ifndef RECORD_DELTA_H
#define RECORD_DELTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define RD_FIXED      12      /* ts, lat, lon, spd, alt, hdg, hdop, sat, bat, ign, up, pon */
#define RD_GROUPS     64      /* extras (comma separated groups) a record may have */
#define RD_WAS        48      /* sticky pairs of the previous record */
#define RD_MARKER     "d=1"

/* The fixed fields the encoder blanks when unchanged: spd, alt, hdg, hdop, sat,
 * bat, up.  The timestamp and position always go, and so do the ignition and
 * powered-on flag, which are state the server acts on and cost a byte each. */
static inline bool rd_blankable(int i)
{
	return (i >= 3 && i <= 8) || i == 10;
}

/* One record, cut at its commas.  Offsets, not pointers: a record is under
 * 64 KB and this sits on the main thread's stack. */
struct rd_rec {
	const char *s;
	uint16_t fixed_off[RD_FIXED], fixed_len[RD_FIXED];
	uint16_t group_off[RD_GROUPS], group_len[RD_GROUPS];
	int groups;
};

/* Cuts a record into its fixed fields (the timestamp keeps its own comma) and
 * its extras.  False for anything that is not a record of at least twelve
 * fields, or that has more extras than fit. */
static inline bool rd_parse(const char *s, size_t len, struct rd_rec *r)
{
	size_t start = 0;
	int piece = 0;

	if (len > UINT16_MAX) {
		return false;
	}
	r->s = s;
	r->groups = 0;
	for (size_t i = 0; i <= len; i++) {
		if (i < len && s[i] != ',') {
			continue;
		}
		if (piece == 1) {                       /* the timestamp: both its pieces */
			r->fixed_off[0] = 0;
			r->fixed_len[0] = (uint16_t)i;
		} else if (piece >= 2 && piece <= RD_FIXED) {
			r->fixed_off[piece - 1] = (uint16_t)start;
			r->fixed_len[piece - 1] = (uint16_t)(i - start);
		} else if (piece > RD_FIXED) {
			if (r->groups >= RD_GROUPS) {
				return false;
			}
			r->group_off[r->groups] = (uint16_t)start;
			r->group_len[r->groups] = (uint16_t)(i - start);
			r->groups++;
		}
		piece++;
		start = i + 1;
	}
	return piece >= RD_FIXED + 1;
}

static inline bool rd_ishex(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
}

/* Whether an extra may be left out when it equals the previous record's:
 * firmware and server must agree (deltarec.STICKY). */
static inline bool rd_sticky(const char *k, size_t n)
{
	static const char *const keys[] = {
		"it", "mt", "vs", "up",
		"rsrp", "snr", "band", "pathloss", "rsrq", "ce", "txrep",
		"orpm", "ormin", "ormax", "oravg", "ospd", "ocl", "oit", "old", "oth",
		"omaf", "otim", "ostft", "oltft", "ofs", "omil", "odtc",
	};

	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		if (strlen(keys[i]) == n && memcmp(keys[i], k, n) == 0) {
			return true;
		}
	}
	return n == 3 && k[0] == 'o' && rd_ishex(k[1]) && rd_ishex(k[2]);     /* o<2 hex> */
}

/* Extras whose body is one value that may hold ';' itself: never split, never
 * dropped (deltarec.OPAQUE). */
static inline bool rd_opaque(const char *g, size_t n)
{
	static const char *const heads[] = {
		"dbg=", "rst=", "ir=", "ie=", "fc=", "fe=", "osp=", "tb=", "wt=",
		"bk=", "ap=", "ak=", "ws=", "mv=", "acc=",
	};

	for (size_t i = 0; i < sizeof(heads) / sizeof(heads[0]); i++) {
		size_t h = strlen(heads[i]);

		if (n >= h && memcmp(g, heads[i], h) == 0) {
			return true;
		}
	}
	return false;
}

/* The next ';' separated pair of a group, as Python's str.split(';') gives them
 * (an empty group is one empty pair).  False after the last. */
static inline bool rd_pair(const char *g, size_t gl, size_t *pos, const char **pair, size_t *pn)
{
	size_t i;

	if (*pos > gl) {
		return false;
	}
	i = *pos;
	while (i < gl && g[i] != ';') {
		i++;
	}
	*pair = g + *pos;
	*pn = i - *pos;
	*pos = i + 1;
	return true;
}

/* A pair as key and value; false when it has no '='. */
static inline bool rd_kv(const char *pair, size_t pn, const char **v, size_t *kn, size_t *vn)
{
	const char *eq = (const char *)memchr(pair, '=', pn);

	if (!eq) {
		return false;
	}
	*kn = (size_t)(eq - pair);
	*v = eq + 1;
	*vn = pn - *kn - 1;
	return true;
}

/* A sticky reading of the previous record. */
struct rd_was {
	const char *key, *val;
	uint16_t kn, vn;
};

/* The latest reading the previous record had for a key, or NULL. */
static inline const struct rd_was *rd_find(const struct rd_was *w, int n, const char *k, size_t kn)
{
	for (int i = n - 1; i >= 0; i--) {
		if (w[i].kn == kn && memcmp(w[i].key, k, kn) == 0) {
			return &w[i];
		}
	}
	return NULL;
}

/* Whether any pair of the record's plain extras has this key. */
static inline bool rd_has_key(const struct rd_rec *r, const char *k, size_t kn)
{
	for (int g = 0; g < r->groups; g++) {
		const char *gp = r->s + r->group_off[g];
		size_t gl = r->group_len[g], pos = 0, pn, vn, pkn;
		const char *pair, *v;

		if (rd_opaque(gp, gl)) {
			continue;
		}
		while (rd_pair(gp, gl, &pos, &pair, &pn)) {
			if (rd_kv(pair, pn, &v, &pkn, &vn) && pkn == kn && memcmp(pair, k, kn) == 0) {
				return true;
			}
		}
	}
	return false;
}

struct rd_out {
	char *p;
	size_t cap, n;
	bool over;
};

static inline void rd_put(struct rd_out *o, const char *s, size_t n)
{
	if (o->over || o->n + n > o->cap) {
		o->over = true;
		return;
	}
	memcpy(o->p + o->n, s, n);
	o->n += n;
}

static inline void rd_putc(struct rd_out *o, char c)
{
	rd_put(o, &c, 1);
}

/* `cur` as a delta against the whole record `prev`, written to out.  Returns its
 * length, 0 when the record is better sent whole (it cannot be a delta, or
 * would not come out shorter), or -1 when out is too small. */
static inline int rdelta_encode(const char *prev, size_t prev_len, const char *cur, size_t cur_len,
				char *out, size_t out_size)
{
	struct rd_rec a, b;                 /* a: the record before, b: this one */
	struct rd_was was[RD_WAS];
	int nwas = 0;
	struct rd_out o = { out, out_size ? out_size - 1 : 0, 0, false };

	if (!rd_parse(prev, prev_len, &a) || !rd_parse(cur, cur_len, &b)) {
		return 0;
	}
	/* Already a delta, or a delta before it: never encoded twice. */
	for (int g = 0; g < a.groups; g++) {
		if (a.group_len[g] == strlen(RD_MARKER) &&
		    memcmp(prev + a.group_off[g], RD_MARKER, strlen(RD_MARKER)) == 0) {
			return 0;
		}
	}
	for (int g = 0; g < b.groups; g++) {
		if (b.group_len[g] == strlen(RD_MARKER) &&
		    memcmp(cur + b.group_off[g], RD_MARKER, strlen(RD_MARKER)) == 0) {
			return 0;
		}
	}

	/* What the record before sent as sticky readings. */
	for (int g = 0; g < a.groups; g++) {
		const char *gp = prev + a.group_off[g];
		size_t gl = a.group_len[g], pos = 0, pn, kn, vn;
		const char *pair, *v;

		if (rd_opaque(gp, gl)) {
			continue;
		}
		while (rd_pair(gp, gl, &pos, &pair, &pn)) {
			if (rd_kv(pair, pn, &v, &kn, &vn) && rd_sticky(pair, kn)) {
				if (nwas >= RD_WAS) {
					return 0;
				}
				was[nwas].key = pair;
				was[nwas].kn = (uint16_t)kn;
				was[nwas].val = v;
				was[nwas].vn = (uint16_t)vn;
				nwas++;
			}
		}
	}
	/* One it had and this one has not: "unchanged" would be a lie. */
	for (int i = 0; i < nwas; i++) {
		if (!rd_has_key(&b, was[i].key, was[i].kn)) {
			return 0;
		}
	}

	/* The fixed fields. */
	for (int i = 0; i < RD_FIXED; i++) {
		const char *f = cur + b.fixed_off[i];
		size_t fl = b.fixed_len[i];
		bool same = rd_blankable(i) && fl == a.fixed_len[i] &&
			    memcmp(f, prev + a.fixed_off[i], fl) == 0;

		if (i > 0) {
			rd_putc(&o, ',');
		}
		if (!same) {
			rd_put(&o, f, fl);
		}
	}

	/* The extras: whole groups, or their pairs but the unchanged sticky ones. */
	for (int g = 0; g < b.groups; g++) {
		const char *gp = cur + b.group_off[g];
		size_t gl = b.group_len[g], pos = 0, pn, kn, vn;
		const char *pair, *v;
		bool any = false;

		if (rd_opaque(gp, gl)) {
			rd_putc(&o, ',');
			rd_put(&o, gp, gl);
			continue;
		}
		while (rd_pair(gp, gl, &pos, &pair, &pn)) {
			if (rd_kv(pair, pn, &v, &kn, &vn) && rd_sticky(pair, kn)) {
				const struct rd_was *w = rd_find(was, nwas, pair, kn);

				if (w && w->vn == vn && memcmp(w->val, v, vn) == 0) {
					continue;
				}
			}
			if (!any) {
				rd_putc(&o, ',');
				any = true;
			} else {
				rd_putc(&o, ';');
			}
			rd_put(&o, pair, pn);
		}
	}
	rd_putc(&o, ',');
	rd_put(&o, RD_MARKER, strlen(RD_MARKER));

	if (o.over) {
		return -1;
	}
	if (o.n >= cur_len) {
		return 0;
	}
	out[o.n] = '\0';
	return (int)o.n;
}

/* A datagram's records, newline separated, as they go on the air: the first
 * whole, each later one a delta against the whole record before it where that
 * can be.  Returns the length written, or -1 when out is too small (the
 * caller sends the records whole). */
static inline int rdelta_encode_batch(const char *recs, size_t len, char *out, size_t out_size)
{
	size_t o = 0, at = 0;
	const char *prev = NULL;
	size_t prev_len = 0;

	while (at <= len) {
		const char *line = recs + at;
		const char *nl = (const char *)memchr(line, '\n', len - at);
		size_t n = nl ? (size_t)(nl - line) : len - at;
		int w = 0;

		if (at > 0) {
			if (o + 1 >= out_size) {
				return -1;
			}
			out[o++] = '\n';
		}
		if (prev) {
			w = rdelta_encode(prev, prev_len, line, n, out + o, out_size - o);
			if (w < 0) {
				return -1;
			}
		}
		if (w == 0) {
			if (o + n >= out_size) {
				return -1;
			}
			memcpy(out + o, line, n);
			w = (int)n;
		}
		o += (size_t)w;
		prev = line;
		prev_len = n;
		at += n + 1;
	}
	if (o >= out_size) {
		return -1;
	}
	out[o] = '\0';
	return (int)o;
}

#endif /* RECORD_DELTA_H */
