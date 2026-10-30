/*
 * The rules behind CONFIG_APP_HOLD_BREAK; see holdbreak.h.
 *
 * A cut never starts while anything is pending that LTE has to carry, and
 * never ends later than the window: the radio is back before the backlog can
 * thin, and a unit that cannot get it back waits for the network to return,
 * and a while longer, instead of cycling a network that is not answering.
 */

#include "holdbreak.h"

#include <stdio.h>
#include <string.h>

static bool enabled(const struct hb *s, int64_t now_ms)
{
    return !s->reg_wait && now_ms >= s->off_until_ms &&
           now_ms >= s->holdoff_until_ms;
}

bool hb_cut_wanted(const struct hb *s, const struct hb_cfg *c,
                   const struct hb_now *n, bool after_send)
{
    if (s->cut || !enabled(s, n->now_ms)) {
        return false;
    }
    /* Registered, so there is a connection to be holding; moving, so the
     * position is changing; and nothing owed that LTE has to carry. */
    if (!n->driving || !n->registered || n->urgent ||
        n->speed_kmh < c->min_kmh) {
        return false;
    }
    if (after_send) {
        /* A connection that is already gone has nothing to break. */
        return s->sticky_left > 0 && n->rrc_up;
    }
    return n->rrc_up && n->hold_ms >= (uint32_t)c->probe_ms;
}

void hb_cut(struct hb *s, const struct hb_cfg *c, int64_t now_ms,
            bool after_send)
{
    s->cut = true;
    s->cut_ms = now_ms;
    if (!after_send) {
        s->sticky_left = c->sticky;
    } else if (s->sticky_left > 0) {
        s->sticky_left--;
    }
}

void hb_cut_failed(struct hb *s, const struct hb_cfg *c, int64_t now_ms,
                   bool for_good)
{
    s->cut = false;
    s->fails++;
    s->sticky_left = 0;
    s->off_until_ms = for_good ? INT64_MAX : now_ms + c->cooldown_ms;
}

bool hb_restore_due(const struct hb *s, const struct hb_cfg *c,
                    const struct hb_now *n, bool send_due)
{
    if (!s->cut) {
        return false;
    }
    /* Whatever made the cut worth having has gone, or something now needs
     * the network: it comes back at once. */
    if (!n->driving || n->urgent || n->speed_kmh * 2 < c->min_kmh) {
        return true;
    }
    return n->now_ms - s->cut_ms >=
           (int64_t)c->window_ms + (send_due ? 0 : c->grace_ms);
}

void hb_restored(struct hb *s, int32_t off_ms, int32_t reg_ms)
{
    s->cut = false;
    if (reg_ms < 0) {
        s->fails++;
        s->sticky_left = 0;
        s->reg_wait = true;
        return;
    }
    s->cycles++;
    s->off_ms_sum += (uint32_t)off_ms;
    s->reg_ms_sum += (uint32_t)reg_ms;
    if ((uint32_t)reg_ms > s->reg_ms_max) {
        s->reg_ms_max = (uint32_t)reg_ms;
    }
}

void hb_registered(struct hb *s, const struct hb_cfg *c, int64_t now_ms)
{
    if (!s->reg_wait) {
        return;
    }
    s->reg_wait = false;
    s->holdoff_until_ms = now_ms + c->holdoff_ms;
}

void hb_new_trip(struct hb *s)
{
    s->reg_wait = false;
    s->holdoff_until_ms = 0;
}

size_t hb_crumb_format(char *out, size_t out_sz, char phase, uint32_t cut_ms,
                       uint32_t phase_ms, uint32_t alive_ms)
{
    uint32_t since_cut = alive_ms > cut_ms ? (alive_ms - cut_ms) / 1000 : 0;
    uint32_t in_phase = alive_ms > phase_ms ? (alive_ms - phase_ms) / 1000 : 0;
    int n;

    if (out_sz == 0) {
        return 0;
    }
    out[0] = '\0';
    if (phase == 0) {
        return 0;
    }
    n = snprintf(out, out_sz, "hb:%c%u/%u", phase,
                 (unsigned)(since_cut > 9999 ? 9999 : since_cut),
                 (unsigned)(in_phase > 999 ? 999 : in_phase));
    if (n < 0 || (size_t)n >= out_sz) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

enum put_mode { PUT_CUT, PUT_WHOLE, PUT_AT_SLASH };

/* Append `s` to out[0..n) after a '+', within `cap` characters.  PUT_CUT takes
 * what fits; PUT_WHOLE all or nothing; PUT_AT_SLASH what fits ending on a
 * whole '/'-separated item, or nothing (half an address says nothing). */
static size_t put(char *out, size_t cap, size_t n, const char *s,
                  enum put_mode mode)
{
    const size_t sep = n ? 1 : 0;
    size_t len, room;

    if (s == NULL || s[0] == '\0' || n + sep >= cap) {
        return n;
    }
    len = strlen(s);
    room = cap - n - sep;
    if (len > room) {
        if (mode == PUT_WHOLE) {
            return n;
        }
        len = room;
        if (mode == PUT_AT_SLASH) {
            while (len > 0 && s[len] != '/') {
                len--;
            }
            if (len == 0) {
                return n;
            }
        }
    }
    if (sep) {
        out[n++] = '+';
    }
    memcpy(out + n, s, len);
    out[n + len] = '\0';
    return n + len;
}

size_t hb_rst_compose(char *out, size_t out_sz, const char *cause,
                      const char *crumb, const char *stall)
{
    const size_t cap = out_sz - 1 < HB_RST_MAX ? out_sz - 1 : HB_RST_MAX;
    size_t n = 0;

    if (out_sz == 0) {
        return 0;
    }
    out[0] = '\0';
    n = put(out, cap, n, cause, PUT_CUT);
    n = put(out, cap, n, crumb, PUT_WHOLE);
    n = put(out, cap, n, stall, PUT_AT_SLASH);
    return n;
}

bool hb_watch_due(struct hb_watch *w, int64_t now_ms, int32_t interval_ms,
                  bool key_on)
{
    if (w->left <= 0 || now_ms < w->due_ms) {
        return false;
    }
    if (key_on) {
        w->due_ms = now_ms + interval_ms;
        return false;
    }
    w->due_ms += interval_ms;
    w->left--;
    return true;
}
