/*
 * Installation mode, the pure parts (checked on the host: tests/test_install.py):
 * the ir= self-test string, the alert summary, the im= reply token, and the
 * retained-RAM note that keeps the mode across a soft reset.  See install.c.
 */
#ifndef INSTALL_CORE_H
#define INSTALL_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* -- ir=<name>:<s>;<name>:<s>;... -------------------------------------------
 * s is '1' ok, '0' failed, '-' not fitted or not applicable.  Names are
 * [a-z0-9]{1,8}; at most 20 entries and 160 characters in all. */
#define INSTALL_IR_NAME_MAX    8
#define INSTALL_IR_MAX_ENTRIES 20
#define INSTALL_IR_MAX_LEN     160

#define INSTALL_OK    1
#define INSTALL_FAIL  0
#define INSTALL_NA   (-1)

struct install_item {
	const char *name;
	int8_t state;           /* INSTALL_OK / INSTALL_FAIL / INSTALL_NA */
};

static inline int install_name_valid(const char *n)
{
	size_t len = n ? strlen(n) : 0;

	if (len < 1 || len > INSTALL_IR_NAME_MAX) {
		return 0;
	}
	for (size_t i = 0; i < len; i++) {
		if (!((n[i] >= 'a' && n[i] <= 'z') || (n[i] >= '0' && n[i] <= '9'))) {
			return 0;
		}
	}
	return 1;
}

/* The ir= value for the items, in order.  An entry with a bad name is left
 * out; entries past the 20th, or that would take the text past 160 characters,
 * are dropped whole.  Returns the length written (buf is NUL-terminated;
 * size must be at least 1). */
static inline size_t install_ir_build(const struct install_item *it, int n,
				      char *buf, size_t size)
{
	size_t used = 0;
	int entries = 0;

	buf[0] = '\0';
	for (int i = 0; i < n && entries < INSTALL_IR_MAX_ENTRIES; i++) {
		if (!install_name_valid(it[i].name)) {
			continue;
		}
		char s = it[i].state == INSTALL_OK ? '1'
			 : it[i].state == INSTALL_FAIL ? '0' : '-';
		size_t need = strlen(it[i].name) + 2 + (used ? 1 : 0);

		if (used + need > INSTALL_IR_MAX_LEN || used + need + 1 > size) {
			break;
		}
		used += (size_t)snprintf(buf + used, size - used, "%s%s:%c",
					 used ? ";" : "", it[i].name, s);
		entries++;
	}
	return used;
}

/* "Install check: N ok, M failed: a, b" -- the one alert an instreport raises.
 * Items that are not fitted count as neither.  Returns the length (the text is
 * cut to fit size, never overflowing it). */
static inline size_t install_summary(const struct install_item *it, int n,
				     char *buf, size_t size)
{
	int ok = 0, bad = 0;
	size_t used;

	for (int i = 0; i < n; i++) {
		if (it[i].state == INSTALL_OK) {
			ok++;
		} else if (it[i].state == INSTALL_FAIL) {
			bad++;
		}
	}
	used = (size_t)snprintf(buf, size, "Install check: %d ok, %d failed", ok, bad);
	if (used >= size) {
		return size ? size - 1 : 0;
	}
	if (bad > 0) {
		int first = 1;

		for (int i = 0; i < n && used + 1 < size; i++) {
			if (it[i].state != INSTALL_FAIL) {
				continue;
			}
			int w = snprintf(buf + used, size - used, "%s%s",
					 first ? ": " : ", ", it[i].name);

			if (w < 0 || (size_t)w >= size - used) {
				used = size - 1;
				break;
			}
			used += (size_t)w;
			first = 0;
		}
	}
	return used;
}

/* -- the im=<0|1> reply token -----------------------------------------------
 * Takes every "im=<0|1>" token (at the start or after a comma, ending at a
 * comma or the end) out of cmd.  Returns the value of the first, or -1 when
 * there is none or its value is not 0 or 1 (that token is then left alone).
 * The server sends it on every reply, like gb= and gx=, so it is removed
 * here: what is left is exactly what cmd_run saw before it existed. */
static inline int install_take_im(char *cmd)
{
	int result = -1;
	char *p = cmd;

	while (*p) {
		char *comma = strchr(p, ',');
		size_t len = comma ? (size_t)(comma - p) : strlen(p);

		if (len == 4 && strncmp(p, "im=", 3) == 0 &&
		    (p[3] == '0' || p[3] == '1')) {
			if (result < 0) {
				result = p[3] - '0';
			}
			if (comma) {
				memmove(p, comma + 1, strlen(comma + 1) + 1);
				continue;               /* the next token is now at p */
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

/* -- the retained-RAM note --------------------------------------------------
 * Lives in __noinit RAM, so a soft reset keeps it and power loss leaves
 * garbage: valid only if the magic and the checksum both match. */
#define INSTALL_MAGIC 0x1E57A11Au

struct install_crumb {
	uint32_t magic;
	uint32_t mode;
	uint32_t check;
};

static inline uint32_t install_crumb_sum(const struct install_crumb *c)
{
	uint32_t h = 2166136261u;                       /* FNV-1a over magic, mode */

	for (int i = 0; i < 4; i++) {
		h = (h ^ ((c->magic >> (8 * i)) & 0xff)) * 16777619u;
	}
	for (int i = 0; i < 4; i++) {
		h = (h ^ ((c->mode >> (8 * i)) & 0xff)) * 16777619u;
	}
	return h;
}

static inline void install_crumb_store(struct install_crumb *c, int mode)
{
	c->magic = INSTALL_MAGIC;
	c->mode = mode ? 1u : 0u;
	c->check = install_crumb_sum(c);
}

/* The mode the note holds, or def if it is not a valid one (wiped RAM, an
 * image that never wrote one, a bit flipped). */
static inline int install_crumb_load(const struct install_crumb *c, int def)
{
	if (c->magic != INSTALL_MAGIC || c->mode > 1u ||
	    c->check != install_crumb_sum(c)) {
		return def;
	}
	return (int)c->mode;
}

#endif
