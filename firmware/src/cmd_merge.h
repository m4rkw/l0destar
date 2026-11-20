/*
 * Merging the commands of a server reply into the ones still waiting to run.
 * Pure string handling, so it is checked on the host: tests/test_cmd_merge.py.
 *
 * Several replies can arrive between two runs of the state machine (a backlog
 * drain, or a modem that held its datagrams and delivered the answers in one
 * burst), and the server deletes a command once it has put it in a reply, so
 * what is waiting is the only copy and is kept, behind the newest reply's.
 * The settings that ride every reply (fota=, gb=, gw=, le=, ...) come again
 * with the next one and only the newest value counts, so a waiting copy of one
 * is dropped, not kept beside it.  Keeping the ones nobody had listed filled
 * the 128-byte buffer: every further reply then logged "server command
 * dropped, no room" for one of them (2,394 lines in four days, about 138 KB of
 * uplink), and a real command queued behind them could be the item dropped.
 */
#ifndef CMD_MERGE_H
#define CMD_MERGE_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* The largest buffer cmd_merge_add() works in. */
#define CMD_MERGE_MAX 256

/* Called with an item that had to be discarded for want of room. */
typedef void (*cmd_merge_drop_t)(const char *item, size_t len);

/* The fields that are state, not events: only what the newest reply says about
 * them counts, whether or not it says anything, so a waiting copy goes.
 * track= is no longer acted on but a server that still sends it must not fill
 * the buffer with stale copies. */
static inline bool cmd_merge_is_state(const char *item, size_t len)
{
	static const char *const keys[] = { "fota=", "track=", "gb=", "im=", "gx=" };

	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		size_t k = strlen(keys[i]);

		if (len >= k && strncmp(item, keys[i], k) == 0) {
			return true;
		}
	}
	return false;
}

/* Whether `list` (comma separated) already has what makes `item` (len bytes)
 * redundant: an item with the same key, for "key=value" (cmd_run acts on the
 * first copy it finds, which is the newest), or the same text, for a bare
 * command. */
static inline bool cmd_merge_has(const char *list, const char *item, size_t len)
{
	const char *eq = (const char *)memchr(item, '=', len);
	size_t keylen = eq ? (size_t)(eq - item) + 1 : len;	/* "key=", or the whole command */
	const char *p = list;

	while (*p) {
		const char *comma = strchr(p, ',');
		size_t plen = comma ? (size_t)(comma - p) : strlen(p);

		if (plen >= keylen && strncmp(p, item, keylen) == 0 && (eq || plen == len)) {
			return true;
		}
		if (!comma) {
			break;
		}
		p = comma + 1;
	}
	return false;
}

/* `pending`, a buffer of cap bytes, becomes `cmd` followed by what was waiting
 * and still counts.  An item that does not fit goes to dropped(). */
static inline void cmd_merge_add(char *pending, size_t cap, const char *cmd,
				 cmd_merge_drop_t dropped)
{
	char merged[CMD_MERGE_MAX];
	size_t used = strlen(cmd);

	if (cap > sizeof(merged)) {
		cap = sizeof(merged);
	}
	if (cap == 0) {
		return;
	}
	if (used >= cap) {
		used = cap - 1;
	}
	memcpy(merged, cmd, used);
	merged[used] = '\0';

	const char *p = pending;

	while (*p) {
		const char *comma = strchr(p, ',');
		size_t len = comma ? (size_t)(comma - p) : strlen(p);

		if (len > 0 && !cmd_merge_is_state(p, len) && !cmd_merge_has(merged, p, len)) {
			size_t need = len + (used > 0 ? 1 : 0);

			if (used + need < cap) {
				if (used > 0) {
					merged[used++] = ',';
				}
				memcpy(&merged[used], p, len);
				used += len;
				merged[used] = '\0';
			} else if (dropped) {
				dropped(p, len);
			}
		}
		if (!comma) {
			break;
		}
		p = comma + 1;
	}
	memcpy(pending, merged, used + 1);
}

#endif /* CMD_MERGE_H */
