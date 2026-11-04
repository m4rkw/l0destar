/*
 * A first-in first-out queue of small records on a NOR flash, written so that
 * the flash is not worn out by it (CONFIG_APP_FLASH_BACKLOG; databuf.c uses it
 * for the telemetry that does not fit in RAM).
 *
 * No clock, no SPI, no logging: the caller supplies the device operations and
 * the time, so the rules run on the host against a simulated flash that counts
 * every erase (tests/test_flashlog.py).
 *
 * WEAR POLICY
 * -----------
 * The part this was written for (MX25R6435F, 8 MiB in 4 KiB sectors) is rated
 * for 100,000 erase cycles per sector.  Spent evenly, that is 56 years of one
 * 470-byte record a second, round the clock.  Spent unevenly it is a day: one
 * sector erased per record reaches the rating in 28 hours.  So the rules are
 * about unevenness, and they are what this module is for:
 *
 *   1. A byte is programmed once between erases.  Records are appended and
 *      never changed: no "sent" flag, no index, no counter is ever written
 *      back into a record or a sector.  Nothing here updates in place.
 *   2. A sector is erased only when it is about to be written again, never to
 *      mark data consumed and never to clear the queue.  A sector that was
 *      read out stays as it is until the ring comes round to it.
 *   3. Sectors are used in physical order, round and round.  Each is erased
 *      once per lap, so at any moment the erase counts of any two sectors
 *      differ by at most one (plus one for each interrupted erase).
 *   4. The place the ring has got to survives a reset.  Every sector's header
 *      carries a sequence number that only goes up, and the newest header says
 *      where to carry on.  Starting again at sector 0 after each reboot is
 *      the usual way a log wears out its first few sectors; this cannot.
 *   5. A budget: once the lifetime lap count (from the sequence numbers, so it
 *      needs no storage of its own) reaches max_cycles nothing is opened any
 *      more.  The rating is a guarantee from the maker, not a target.
 *   6. A rate limit: at most max_opens_per_hour sectors are opened in each
 *      hour (counted from the first open of it), whatever the caller asks.  A
 *      bug that appended in a loop would otherwise be a chip-killer; at the
 *      default it would take nearly ten years of that to get through the
 *      rating.
 *   7. No retry storms.  A sector that will not erase or program is skipped,
 *      not retried, and three failures in a row stop all writes until the next
 *      boot.  Reading what is already there carries on.
 *
 * SCOPE
 * -----
 * The queue lives for one boot.  What a previous boot left in the flash is not
 * read back: a record carries the uptime it was built at (up=), which means
 * nothing after a reset, and a record sent again without a true age would be
 * filed by the server as live.  The sector headers are all that is read at
 * start-up, and only to find where the ring has got to.
 *
 * FORMAT
 * ------
 * Sector:  16-byte header, then records back to back to the end.
 *            0  magic    'L0FL'
 *            4  seq      u32, 1 for the first sector ever opened
 *            8  version  u16
 *           10  layout   u16, sector size in 256-byte units
 *           12  reserved u16
 *           14  crc      u16, CRC-16/CCITT-FALSE of bytes 0..13
 * Record:    0  type     u8, 0xA7
 *            1  len      u16
 *            3  crc      u16, over type, len and payload
 *            5  payload
 * A record does not cross a sector.  All integers are little-endian.
 *
 * Not safe to call from more than one thread.
 */

#ifndef APP_FLASHLOG_H_
#define APP_FLASHLOG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FLASHLOG_SECTOR_SIZE   4096u
#define FLASHLOG_PAGE_SIZE      256u
#define FLASHLOG_SEC_HDR_SIZE    16u
#define FLASHLOG_REC_HDR_SIZE     5u
#define FLASHLOG_PAYLOAD_MAX   1024u
#define FLASHLOG_MIN_SECTORS      8u

/* Sectors tried in one append before it gives up, and failures in a row (of
 * a sector opening or a record being programmed) after which writes stop. */
#define FLASHLOG_OPEN_TRIES       3u
#define FLASHLOG_DEAD_AFTER       3u

#define FLASHLOG_ERR_ARG    (-1)    /* bad argument, or a record that cannot be one */
#define FLASHLOG_ERR_IO     (-2)    /* the device failed */
#define FLASHLOG_ERR_RATE   (-3)    /* too many sectors opened this hour: try later */
#define FLASHLOG_ERR_WORN   (-4)    /* the cycle budget is spent: never again */
#define FLASHLOG_ERR_DEAD   (-5)    /* writes stopped after repeated failures */
#define FLASHLOG_ERR_STALE  (-6)    /* the cursor was overtaken by a drop */
#define FLASHLOG_ERR_CRC    (-7)    /* a record could not be read (corrupt, or
                                     * longer than the buffer); the rest of its
                                     * sector is skipped */

/* The flash.  read() may be any length; prog() is never given more than a page
 * or one that crosses a page boundary, and only ever for bytes that are erased;
 * erase() is given the address of a sector.  All return 0 or a negative error. */
struct flashlog_dev {
    int (*read)(void *ctx, uint32_t addr, void *buf, size_t len);
    int (*prog)(void *ctx, uint32_t addr, const void *buf, size_t len);
    int (*erase)(void *ctx, uint32_t addr);
    void    *ctx;
    uint32_t sectors;           /* 4 KiB sectors from address 0 the log may use */
};

struct flashlog_cfg {
    uint32_t max_cycles;            /* 0 = no limit */
    uint32_t max_opens_per_hour;    /* 0 = no limit */
};

struct flashlog_stats {
    uint32_t opens;             /* sectors opened (including ones that failed) */
    uint32_t erases;            /* erase commands issued */
    uint32_t blank_skips;       /* sectors opened that were already blank */
    uint32_t skipped;           /* sectors given up on */
    uint32_t appended;          /* records written */
    uint32_t consumed;          /* records read and committed */
    uint32_t dropped;           /* unread records overwritten because the ring was full */
    uint32_t lost;              /* records that could not be read back */
    uint32_t io_errors;
    uint32_t refused_rate;
    uint32_t refused_worn;
};

struct flashlog {
    const struct flashlog_dev *dev;
    struct flashlog_cfg        cfg;
    uint16_t                  *cnt;         /* records per sector, indexed by sector */

    /* Where the ring has got to, from the sector headers. */
    uint32_t next_seq;
    uint32_t head_idx;                      /* sector of seq next_seq - 1 */

    /* This boot's window: seq first_seq is in sector first_idx, and each seq
     * after it in the next sector round. */
    bool     anchored;
    uint32_t first_seq;
    uint32_t first_idx;

    /* The sector being written (seq 0: none yet) and the read cursor. */
    uint32_t wr_seq;
    uint32_t wr_off;
    uint32_t rd_seq;
    uint32_t rd_off;
    uint32_t rd_left;                       /* unread records in sector rd_seq */
    uint32_t pending;
    uint32_t gen;                           /* bumped when a cursor is invalidated */

    uint32_t fails;                         /* consecutive failures */
    bool     dead;
    bool     win_valid;
    uint32_t win_start;
    uint32_t win_count;

    struct flashlog_stats st;
};

/* A position in the queue to read from.  Reading moves the cursor, not the
 * queue: nothing is consumed until flashlog_commit(), so a datagram that failed
 * to send leaves its records where they were. */
struct flashlog_cur {
    uint32_t seq, off, left;
    uint32_t n;                 /* records passed so far, read or lost */
    uint32_t lost;              /* of them, the ones that could not be read */
    uint32_t gen;
};

/* Reads the sector headers (nothing is written) and finds where the ring has
 * got to.  cnt has dev->sectors entries.  0, or a negative error. */
int flashlog_init(struct flashlog *fl, const struct flashlog_dev *dev,
                  uint16_t *cnt, const struct flashlog_cfg *cfg);

/* Adds a record to the end of the queue.  now_s is any clock in seconds that
 * does not run backwards within a boot. */
int flashlog_append(struct flashlog *fl, const void *rec, uint16_t len,
                    uint32_t now_s);

uint32_t flashlog_pending(const struct flashlog *fl);

void flashlog_cursor(const struct flashlog *fl, struct flashlog_cur *c);

/* The record at the cursor into buf (cap bytes), and the cursor on past it.
 * Returns its length; 0 when there are no more; FLASHLOG_ERR_CRC when one could
 * not be read, or is longer than cap (the cursor has moved past the rest of
 * that sector, and counts it as lost: call again). */
int flashlog_next(struct flashlog *fl, struct flashlog_cur *c, void *buf,
                  uint16_t cap);

/* Takes everything the cursor has passed out of the queue.  No erase and no
 * write: the sectors are reused when the ring comes round to them. */
int flashlog_commit(struct flashlog *fl, struct flashlog_cur *c);

/* Sector erases the lifetime of the log has needed per sector, from the
 * sequence numbers: at most this many on any sector. */
uint32_t flashlog_cycles(const struct flashlog *fl);

#endif
