/*
 * When to take LTE away from a network that will not release the radio, and
 * when to give it back (CONFIG_APP_HOLD_BREAK; Kconfig has the why).
 *
 * Decisions only: no clock, no radio, no logging.  The caller gathers the
 * facts (main.c), does the switching (modem.c) and says what came of it, so
 * the rules can be exercised on the host without a modem.
 */

#ifndef APP_HOLDBREAK_H_
#define APP_HOLDBREAK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hb_cfg {
    int32_t probe_ms;       /* connected this long after our last datagram: a hold */
    int32_t window_ms;      /* LTE stays off at most this long */
    int32_t grace_ms;       /* beyond the window, where no send is due to end it */
    int32_t cooldown_ms;    /* off this long after the modem failed to let LTE go */
    int32_t holdoff_ms;     /* off this long after the modem registered again,
                             * following a restore that did not register */
    int32_t min_kmh;        /* cut above this; an open cut ends below half of it */
    int32_t sticky;         /* sends cut straight away, per hold the probe finds */
};

/* What the caller knows at the moment of asking. */
struct hb_now {
    int64_t  now_ms;
    int32_t  speed_kmh;     /* the vehicle's, 0 or more */
    bool     rrc_up;        /* the network holds a connection now */
    uint32_t hold_ms;       /* for how long since the later of its start and our
                             * last datagram: what the network has added */
    bool     driving;       /* key on, engine running, not coasting, not tracking */
    bool     registered;
    bool     urgent;        /* something that cannot wait for LTE */
};

struct hb {
    bool     cut;           /* LTE is off for the receiver's sake */
    int64_t  cut_ms;
    int64_t  off_until_ms;  /* no cuts before this; INT64_MAX: not this boot */
    bool     reg_wait;      /* a restore did not register: no cuts until the modem
                             * does (hb_registered) */
    int64_t  holdoff_until_ms;  /* and none before this, which hb_registered sets */
    int32_t  sticky_left;
    /* For the log: finished cycles, restores that failed, and the sums. */
    uint32_t cycles;
    uint32_t fails;
    uint32_t off_ms_sum;
    uint32_t reg_ms_sum;
    uint32_t reg_ms_max;
};

/* Take LTE away now?  `after_send`: asked just after a send and its replies,
 * where a hold seen lately is cut at once; otherwise asked while the receiver
 * waits for a fix, where only a connection that has outlived the probe is. */
bool hb_cut_wanted(const struct hb *s, const struct hb_cfg *c,
                   const struct hb_now *n, bool after_send);

/* LTE went off. */
void hb_cut(struct hb *s, const struct hb_cfg *c, int64_t now_ms,
            bool after_send);

/* The command to switch it off failed.  `for_good`: the modem refused it, so
 * asking again will not help before the next boot. */
void hb_cut_failed(struct hb *s, const struct hb_cfg *c, int64_t now_ms,
                   bool for_good);

/* Give LTE back now?  `send_due`: asked with a batch ready to go, which is
 * where the window normally ends; otherwise asked from the loop's top, which
 * allows the grace on top of the window. */
bool hb_restore_due(const struct hb *s, const struct hb_cfg *c,
                    const struct hb_now *n, bool send_due);

/* LTE is back, or is not.  reg_ms < 0: it did not register in time, and the
 * feature stays off until the modem registers by itself (hb_registered). */
void hb_restored(struct hb *s, int32_t off_ms, int32_t reg_ms);

/* The modem has registered.  Ends the wait a restore that did not register
 * began: nothing was gained by staying off while there was no network to cut
 * (a cut needs a registration), and a dead zone ends when the vehicle leaves
 * it.  Cuts stay off for holdoff_ms from here all the same, in case the cycling
 * had a hand in it.  Nothing happens if no restore failed. */
void hb_registered(struct hb *s, const struct hb_cfg *c, int64_t now_ms);

/* The unit has been parked: the wait for a registration and the hold-off after
 * it are over.  A refusal (hb_cut_failed, for_good) stays. */
void hb_new_trip(struct hb *s);

/* -- what the next boot says about the breaker -------------------------------
 *
 * 2026-10-26 13:10: the car's watchdog reset it 130 s after a cut.  The log
 * lines that would have said where it was were in RAM, no stall address was
 * captured, and rst=wdt alone said only that it happened.  So the breaker
 * keeps a note of where it is in noinit RAM (crumb.c), and a reset nobody asked
 * for reports it in rst= on the next boot.
 *
 * The phases, by what the code is doing; 0 is none (no cycle under way). */
#define HB_PH_CUT      'C'   /* deciding to cut: settling, then CFUN=20 */
#define HB_PH_OFF      'O'   /* LTE off, the window running */
#define HB_PH_RESTORE  'R'   /* CFUN=21 and the wait for a registration */
#define HB_PH_QUERY    'Q'   /* AT queries about the search: none has a timeout of its own */
#define HB_PH_FAILED   'F'   /* the restore did not register: breaking is off, the loop runs */
#define HB_PH_DELIVER  'D'   /* registered again: the backlog and the batch going out */

/* "hb:Q98/0": the last sign of life (a watchdog kick) was 98 s after the cut and
 * 0 s after the phase began, and the phase was Q.  Seconds are clamped (9999,
 * 999) and a time before its start reads 0.  Writes nothing for phase 0.  At
 * most 12 characters and the terminator.  Returns the length. */
size_t hb_crumb_format(char *out, size_t out_sz, char phase, uint32_t cut_ms,
                       uint32_t phase_ms, uint32_t alive_ms);

/* rst= is varchar(48) on the server, which runs in strict mode: a longer value
 * is refused, and the record with it, which is the first after a boot and
 * carries fw= as well.  The parts of the field in the order of what each is
 * worth: the cause (cut short if it must be), the crumb (whole or not at all),
 * and the stall addresses (as many whole ones as fit).  NULL or empty parts are
 * skipped.  The result is at most HB_RST_MAX characters.  Returns the length. */
#define HB_RST_MAX 47
size_t hb_rst_compose(char *out, size_t out_sz, const char *cause,
                      const char *crumb, const char *stall);

/* -- the modem's account of a search that has not registered ------------------
 *
 * After a restore that did not register the unit says what the modem makes of
 * the search every interval_ms, `left` times (modem_search_report: four AT
 * queries, none with a timeout of its own).  Not with the key on.  On
 * 2026-10-26 the car reset twice in 84 minutes on the road (13:10, rst=wdt;
 * 14:37, which only the timing of its first manifest fetch gives away), both
 * with this search under way, and the history has watchdog resets without it
 * (18 Sep, 11 Oct, 13 Oct), so it is a lead and not a finding.  Parked, it has
 * run 21 times in a row without trouble (10:55, a cell heard every time).  The
 * reports wait for the key to go off, and are not used up meanwhile. */
struct hb_watch {
    int64_t due_ms;     /* the next report is due at this time */
    int32_t left;       /* how many more are wanted; 0 or less: none */
};

/* Is a report due now?  If so it is counted and the next is due interval_ms
 * after the one just due.  With the key on the answer is no, nothing is counted
 * and the next is due interval_ms from now, so they neither pile up while the
 * key is on nor all come at once when it goes off. */
bool hb_watch_due(struct hb_watch *w, int64_t now_ms, int32_t interval_ms,
                  bool key_on);

#endif /* APP_HOLDBREAK_H_ */
