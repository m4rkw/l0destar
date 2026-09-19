/*
 * Threshold alerts on OBD-II telemetry.
 *
 * A rule list in CONFIG_APP_OBD_ALERTS names a field of the live snapshot,
 * a comparison and a value in engineering units — "coolant>=90",
 * "rpm>6500", "stft<-12.5" — and every poll that lands a reading for that
 * field is checked against it.  Crossing the threshold queues one alert;
 * nothing more is said until the value has come back across it by the
 * rule's hysteresis, when a clear is queued at normal priority and the
 * rule is armed again.  Without hysteresis a coolant gauge sitting at the
 * threshold would fire on every other poll, and the queue holds five.
 *
 * The evaluation runs on the live snapshot after each poll rather than on
 * the record, so a slow-moving PID is checked as soon as its rotation slot
 * reads it, about once every eight seconds, and a fast one about once a
 * second.  A field the ECU does not support, or did not answer, is simply
 * skipped: no reading, no verdict.
 *
 * Rule state is dropped at key-off (obd_close with the ignition off, not a
 * mid-drive reopen), so each drive starts armed: an engine that is still
 * hot at the next key-on alerts again, and a cold one does not announce a
 * clear it never saw happen.
 *
 * GRAMMAR
 * -------
 *   rules  := rule { [,; ] rule }
 *   rule   := field op value [ "/" hysteresis ]
 *   field  := a name from the table below, or the record key ("ocl")
 *   op     := ">" | ">=" | "<" | "<=" | "="
 *   value, hysteresis := decimal, in the field's engineering unit
 *
 * The hysteresis defaults to a twentieth of the threshold (4 C on 90 C,
 * 325 rpm on 6500), which is enough to ride out a reading that hovers.
 * Values with more decimals than the field carries are rounded.
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app.h"

LOG_MODULE_REGISTER(obd_alert, CONFIG_APP_LOG_LEVEL);

#define OBD_ALERT_MAX_RULES 8

/* One row per snapshot field a rule may name.  The scale is the snapshot's
 * (the same one obd_append() sends), so thresholds are compared as the
 * integers the poll produces and the float formatting stays out of it. */
struct obd_field {
	const char *name;     /* what a rule writes */
	const char *key;      /* the record key, accepted as an alias */
	size_t off;           /* into struct obd_snapshot */
	int scale;            /* snapshot units per engineering unit */
	const char *unit;     /* for the message */
};

static const struct obd_field s_fields[] = {
	{ "rpm",      "orpm",  offsetof(struct obd_snapshot, rpm),         1,   "rpm"  },
	{ "speed",    "ospd",  offsetof(struct obd_snapshot, speed),       1,   "km/h" },
	{ "coolant",  "ocl",   offsetof(struct obd_snapshot, coolant),     1,   "C"    },
	{ "intake",   "oit",   offsetof(struct obd_snapshot, intake),      1,   "C"    },
	{ "load",     "old",   offsetof(struct obd_snapshot, load),        10,  "%"    },
	{ "throttle", "oth",   offsetof(struct obd_snapshot, throttle),    10,  "%"    },
	{ "maf",      "omaf",  offsetof(struct obd_snapshot, maf),         100, "g/s"  },
	{ "timing",   "otim",  offsetof(struct obd_snapshot, timing),      10,  "deg"  },
	{ "stft",     "ostft", offsetof(struct obd_snapshot, stft),        10,  "%"    },
	{ "ltft",     "oltft", offsetof(struct obd_snapshot, ltft),        10,  "%"    },
	{ "fuel",     "ofs",   offsetof(struct obd_snapshot, fuel_status), 1,   ""     },
	{ "mil",      "omil",  offsetof(struct obd_snapshot, mil),         1,   ""     },
	{ "dtc",      "odtc",  offsetof(struct obd_snapshot, dtc_count),   1,   ""     },
};

enum obd_op { OP_GT, OP_GE, OP_LT, OP_LE, OP_EQ };

static const char *const s_op_str[] = { ">", ">=", "<", "<=", "=" };

struct obd_rule {
	const struct obd_field *field;
	enum obd_op op;
	int32_t threshold;    /* in snapshot units */
	int32_t hysteresis;   /* in snapshot units, >= 0 */
	bool fired;
};

static struct obd_rule s_rules[OBD_ALERT_MAX_RULES];
static int             s_n_rules;

/* -- parsing --------------------------------------------------------------- */

static const struct obd_field *field_lookup(const char *name, size_t len)
{
	for (size_t i = 0; i < ARRAY_SIZE(s_fields); i++) {
		const struct obd_field *f = &s_fields[i];

		if ((strlen(f->name) == len && strncmp(f->name, name, len) == 0) ||
		    (strlen(f->key) == len && strncmp(f->key, name, len) == 0)) {
			return f;
		}
	}
	return NULL;
}

/* Parse "[-]digits[.digits]" into an integer at `scale` units per whole,
 * rounding any excess decimals.  Returns the number of characters consumed,
 * or 0 if there was no number. */
static int parse_scaled(const char *s, int scale, int32_t *out)
{
	const char *p = s;
	bool neg = false;
	int64_t whole = 0;
	int64_t frac = 0;
	int64_t frac_scale = 1;

	if (*p == '-') {
		neg = true;
		p++;
	} else if (*p == '+') {
		p++;
	}
	if (!isdigit((unsigned char)*p) && !(*p == '.' && isdigit((unsigned char)p[1]))) {
		return 0;
	}
	while (isdigit((unsigned char)*p)) {
		whole = whole * 10 + (*p++ - '0');
	}
	if (*p == '.') {
		p++;
		while (isdigit((unsigned char)*p)) {
			/* Keep one digit beyond the scale for the rounding. */
			if (frac_scale < (int64_t)scale * 10) {
				frac = frac * 10 + (*p - '0');
				frac_scale *= 10;
			}
			p++;
		}
	}

	/* frac / frac_scale of a unit, wanted in `scale` units, rounded. */
	int64_t v = whole * scale + (frac * scale * 2 + frac_scale) / (2 * frac_scale);

	*out = (int32_t)(neg ? -v : v);
	return (int)(p - s);
}

/* One rule out of the list; `*s` is advanced past it and its separator. */
static int parse_rule(const char **s, struct obd_rule *r)
{
	const char *p = *s;

	while (*p == ' ' || *p == ',' || *p == ';') {
		p++;
	}
	if (*p == '\0') {
		*s = p;
		return 0;
	}

	const char *name = p;

	while (isalnum((unsigned char)*p) || *p == '_') {
		p++;
	}
	r->field = field_lookup(name, (size_t)(p - name));
	if (!r->field) {
		char bad[16];

		snprintf(bad, sizeof(bad), "%.*s", (int)(p - name), name);
		LOG_ERR("rule '%s': unknown field '%s'", *s, bad);
		return -EINVAL;
	}

	if (p[0] == '>' && p[1] == '=') {
		r->op = OP_GE;
		p += 2;
	} else if (p[0] == '<' && p[1] == '=') {
		r->op = OP_LE;
		p += 2;
	} else if (p[0] == '>') {
		r->op = OP_GT;
		p += 1;
	} else if (p[0] == '<') {
		r->op = OP_LT;
		p += 1;
	} else if (p[0] == '=' && p[1] == '=') {
		r->op = OP_EQ;
		p += 2;
	} else if (p[0] == '=') {
		r->op = OP_EQ;
		p += 1;
	} else {
		LOG_ERR("rule '%s': expected an operator after '%s'", *s, r->field->name);
		return -EINVAL;
	}

	int n = parse_scaled(p, r->field->scale, &r->threshold);

	if (n == 0) {
		LOG_ERR("rule '%s': expected a number after '%s'", *s, s_op_str[r->op]);
		return -EINVAL;
	}
	p += n;

	if (*p == '/') {
		p++;
		n = parse_scaled(p, r->field->scale, &r->hysteresis);
		if (n == 0 || r->hysteresis < 0) {
			LOG_ERR("rule '%s': bad hysteresis", *s);
			return -EINVAL;
		}
		p += n;
	} else {
		int32_t t = r->threshold < 0 ? -r->threshold : r->threshold;

		r->hysteresis = t / 20;
	}

	if (*p != '\0' && *p != ' ' && *p != ',' && *p != ';') {
		LOG_ERR("rule '%s': trailing '%s'", *s, p);
		return -EINVAL;
	}
	r->fired = false;
	*s = p;
	return 1;
}

/* Value in engineering units for the log and the alert text: "92", "12.5",
 * "-3.25".  Never a float. */
static void fmt_value(char *out, size_t max, int32_t v, int scale)
{
	int32_t a = v < 0 ? -v : v;
	const char *sign = v < 0 ? "-" : "";

	if (scale == 1) {
		snprintf(out, max, "%s%d", sign, a);
	} else if (scale == 10) {
		snprintf(out, max, "%s%d.%d", sign, a / 10, a % 10);
	} else {
		snprintf(out, max, "%s%d.%02d", sign, a / 100, a % 100);
	}
}

void obd_alert_init(void)
{
	const char *s = CONFIG_APP_OBD_ALERTS;

	s_n_rules = 0;
	while (s_n_rules < OBD_ALERT_MAX_RULES) {
		int rc = parse_rule(&s, &s_rules[s_n_rules]);

		if (rc < 0) {
			/* Stop at the first bad rule: silently skipping it and
			 * running the rest would hide exactly the alert the
			 * operator thought they had configured. */
			s_n_rules = 0;
			LOG_ERR("OBD alerts disabled — fix CONFIG_APP_OBD_ALERTS");
			return;
		}
		if (rc == 0) {
			break;
		}
		s_n_rules++;
	}
	while (*s == ' ' || *s == ',' || *s == ';') {
		s++;
	}
	if (*s != '\0') {
		LOG_WRN("more than %d rules — the rest are ignored", OBD_ALERT_MAX_RULES);
	}

	for (int i = 0; i < s_n_rules; i++) {
		const struct obd_rule *r = &s_rules[i];
		char t[16], h[16];

		fmt_value(t, sizeof(t), r->threshold, r->field->scale);
		fmt_value(h, sizeof(h), r->hysteresis, r->field->scale);
		LOG_INF("rule %d: %s %s %s%s, hysteresis %s%s", i,
			r->field->name, s_op_str[r->op], t, r->field->unit,
			h, r->field->unit);
	}
}

void obd_alert_reset(void)
{
	for (int i = 0; i < s_n_rules; i++) {
		s_rules[i].fired = false;
	}
}

static bool rule_trips(const struct obd_rule *r, int32_t v)
{
	switch (r->op) {
	case OP_GT: return v >  r->threshold;
	case OP_GE: return v >= r->threshold;
	case OP_LT: return v <  r->threshold;
	case OP_LE: return v <= r->threshold;
	case OP_EQ: return v == r->threshold;
	}
	return false;
}

/* The condition is off by the hysteresis: the value has come back far
 * enough for another alert to mean something. */
static bool rule_clears(const struct obd_rule *r, int32_t v)
{
	switch (r->op) {
	case OP_GT: return v <= r->threshold - r->hysteresis;
	case OP_GE: return v <  r->threshold - r->hysteresis;
	case OP_LT: return v >= r->threshold + r->hysteresis;
	case OP_LE: return v >  r->threshold + r->hysteresis;
	case OP_EQ: return v != r->threshold;
	}
	return false;
}

void obd_alert_eval(const struct obd_snapshot *s)
{
	for (int i = 0; i < s_n_rules; i++) {
		struct obd_rule *r = &s_rules[i];
		const struct obd_field *f = r->field;
		int32_t v = *(const int32_t *)((const char *)s + f->off);

		if (v == OBD_NOT_AVAILABLE) {
			continue;
		}

		bool fire = !r->fired && rule_trips(r, v);
		bool clear = r->fired && rule_clears(r, v);

		if (!fire && !clear) {
			continue;
		}

		char val[16], thr[16], msg[64];

		fmt_value(val, sizeof(val), v, f->scale);
		fmt_value(thr, sizeof(thr), r->threshold, f->scale);
		/* The field name leads so a server can key an alarm type off
		 * it (traccar.c maps "coolant" and "intake" to temperature). */
		snprintf(msg, sizeof(msg), "%s %s%s (%s %s%s%s)",
			 f->name, val, f->unit,
			 s_op_str[r->op], thr, f->unit,
			 fire ? "" : " cleared");
		alert_enqueue(msg, fire ? CONFIG_APP_OBD_ALERT_PRIORITY : 0);
		r->fired = fire;
	}
}
