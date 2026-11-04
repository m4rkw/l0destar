/*
 * See flashlog.h for what this is and the rules it keeps.  The rules are
 * enforced in three places, and each says so where it does:
 *
 *   open_next()   the only thing that erases, and only when a sector is about
 *                 to be written again; the rotation, the cycle budget, the rate
 *                 limit and the failure limit all live there.
 *   flashlog_append()  the only thing that programs a record, once, at the end.
 *   flashlog_commit()  writes nothing.
 */

#include <string.h>

#include "flashlog.h"

#define SECTOR   FLASHLOG_SECTOR_SIZE
#define PAGE     FLASHLOG_PAGE_SIZE
#define SEC_HDR  FLASHLOG_SEC_HDR_SIZE
#define REC_HDR  FLASHLOG_REC_HDR_SIZE

#define MAGIC     0x4C46304Cu      /* 'L' '0' 'F' 'L' */
#define VERSION   1u
#define LAYOUT    (SECTOR / 256u)
#define REC_TYPE  0xA7u

/* Past this the sequence number is about to wrap, which would put 0 (and then
 * old numbers) at the head of the ring.  At the default rate limit that is two
 * centuries away; it is checked anyway. */
#define SEQ_LIMIT 0xFFFFFF00u

static uint16_t crc16(uint16_t crc, const uint8_t *p, size_t n)
{
    while (n--) {
        crc ^= (uint16_t)((uint16_t)*p++ << 8);
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

static uint32_t get16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
    return get16(p) | (get16(p + 2) << 16);
}

static void hdr_encode(uint8_t h[SEC_HDR], uint32_t seq)
{
    put32(h, MAGIC);
    put32(h + 4, seq);
    put16(h + 8, VERSION);
    put16(h + 10, LAYOUT);
    put16(h + 12, 0);
    put16(h + 14, crc16(0xFFFFu, h, 14));
}

static bool hdr_decode(const uint8_t h[SEC_HDR], uint32_t *seq)
{
    if (get32(h) != MAGIC || get16(h + 8) != VERSION ||
        get16(h + 10) != LAYOUT || get16(h + 14) != crc16(0xFFFFu, h, 14)) {
        return false;
    }
    *seq = get32(h + 4);
    return *seq != 0 && *seq != 0xFFFFFFFFu;
}

static uint32_t sec_addr(uint32_t idx)
{
    return idx * SECTOR;
}

/* The sector seq lives in.  Valid for any seq from first_seq on. */
static uint32_t idx_of(const struct flashlog *fl, uint32_t seq)
{
    return (fl->first_idx + (seq - fl->first_seq)) % fl->dev->sectors;
}

static int rd(struct flashlog *fl, uint32_t addr, void *buf, size_t len)
{
    if (fl->dev->read(fl->dev->ctx, addr, buf, len) != 0) {
        fl->st.io_errors++;
        return FLASHLOG_ERR_IO;
    }
    return 0;
}

/* Programs data at addr, a page at a time.  Never over a byte that is not
 * erased: callers only ever write past the end of what a sector holds. */
static int prog_range(struct flashlog *fl, uint32_t addr, const uint8_t *data,
                      size_t len)
{
    while (len > 0) {
        size_t chunk = PAGE - (addr % PAGE);

        if (chunk > len) {
            chunk = len;
        }
        if (fl->dev->prog(fl->dev->ctx, addr, data, chunk) != 0) {
            fl->st.io_errors++;
            return FLASHLOG_ERR_IO;
        }
        addr += (uint32_t)chunk;
        data += chunk;
        len -= chunk;
    }
    return 0;
}

static int sector_blank(struct flashlog *fl, uint32_t idx, bool *blank)
{
    uint8_t buf[PAGE];

    for (uint32_t off = 0; off < SECTOR; off += PAGE) {
        int err = rd(fl, sec_addr(idx) + off, buf, PAGE);

        if (err) {
            return err;
        }
        for (size_t i = 0; i < PAGE; i++) {
            if (buf[i] != 0xFF) {
                *blank = false;
                return 0;
            }
        }
    }
    *blank = true;
    return 0;
}

/* Leaves the sector blank.  Looks first, and erases only if it has to: a
 * sector the ring has not been round to yet (a new chip's first lap) costs no
 * cycle.  Looks again afterwards, so a sector whose erase did not take is found
 * here and not by a record that will not read back. */
static int prepare_sector(struct flashlog *fl, uint32_t idx)
{
    bool blank;
    int err = sector_blank(fl, idx, &blank);

    if (err) {
        return err;
    }
    if (blank) {
        fl->st.blank_skips++;
        return 0;
    }
    fl->st.erases++;
    if (fl->dev->erase(fl->dev->ctx, sec_addr(idx)) != 0) {
        fl->st.io_errors++;
        return FLASHLOG_ERR_IO;
    }
    err = sector_blank(fl, idx, &blank);
    if (err) {
        return err;
    }
    if (!blank) {
        fl->st.io_errors++;
        return FLASHLOG_ERR_IO;
    }
    return 0;
}

static int write_header(struct flashlog *fl, uint32_t idx, uint32_t seq)
{
    uint8_t h[SEC_HDR], back[SEC_HDR];
    int err;

    hdr_encode(h, seq);
    err = prog_range(fl, sec_addr(idx), h, SEC_HDR);
    if (err) {
        return err;
    }
    err = rd(fl, sec_addr(idx), back, SEC_HDR);
    if (err) {
        return err;
    }
    if (memcmp(h, back, SEC_HDR) != 0) {
        fl->st.io_errors++;
        return FLASHLOG_ERR_IO;
    }
    return 0;
}

/* A queue with nothing in it has its read cursor at the write position. */
static void settle_empty(struct flashlog *fl)
{
    if (fl->pending == 0) {
        fl->rd_seq = fl->wr_seq;
        fl->rd_off = fl->wr_off;
        fl->rd_left = 0;
    }
}

/* The oldest sector holding records is about to be erased: what was not read
 * out of it is gone.  Only reached with the ring full, which at 1 record a
 * second is hours of outage; it is counted, and databuf.c says so. */
static void drop_rd_sector(struct flashlog *fl)
{
    fl->pending -= fl->rd_left;
    fl->st.dropped += fl->rd_left;
    fl->gen++;
    fl->rd_seq++;
    fl->rd_off = SEC_HDR;
    fl->rd_left = (fl->rd_seq <= fl->wr_seq) ? fl->cnt[idx_of(fl, fl->rd_seq)] : 0;
}

/* Allowed to open another sector this hour? */
static bool rate_ok(struct flashlog *fl, uint32_t now_s)
{
    if (fl->cfg.max_opens_per_hour == 0) {
        return true;
    }
    if (!fl->win_valid || now_s - fl->win_start >= 3600u) {
        fl->win_valid = true;
        fl->win_start = now_s;
        fl->win_count = 0;
    }
    if (fl->win_count >= fl->cfg.max_opens_per_hour) {
        return false;
    }
    fl->win_count++;
    return true;
}

uint32_t flashlog_cycles(const struct flashlog *fl)
{
    return (fl->next_seq - 1u) / fl->dev->sectors;
}

/* Opens the next sector of the ring for writing.  This is where a sector gets
 * erased, which is the only wear there is, so everything that protects the chip
 * is here:
 *
 *   - the sector is always the one after the newest, found from the headers, so
 *     the rotation never starts over and no sector is erased twice in a lap;
 *   - nothing is opened once the lifetime cycle budget is spent, or more than
 *     max_opens_per_hour times in an hour;
 *   - a sector that fails is given up on (its slot in the ring stays empty, so
 *     the numbering stays in step with the sectors) and the next one is tried,
 *     at most FLASHLOG_OPEN_TRIES times a call and FLASHLOG_DEAD_AFTER times in
 *     a row before writes stop altogether. */
static int open_next(struct flashlog *fl, uint32_t now_s)
{
    const uint32_t n = fl->dev->sectors;

    if (fl->dead) {
        return FLASHLOG_ERR_DEAD;
    }
    if (!fl->anchored) {
        fl->first_seq = fl->next_seq;
        fl->first_idx = (fl->head_idx + 1u) % n;
        fl->anchored = true;
    }

    for (uint32_t attempt = 0; attempt < FLASHLOG_OPEN_TRIES; attempt++) {
        if (fl->next_seq >= SEQ_LIMIT ||
            (fl->cfg.max_cycles != 0 && flashlog_cycles(fl) >= fl->cfg.max_cycles)) {
            fl->st.refused_worn++;
            return FLASHLOG_ERR_WORN;
        }
        if (!rate_ok(fl, now_s)) {
            fl->st.refused_rate++;
            return FLASHLOG_ERR_RATE;
        }

        const uint32_t seq = fl->next_seq;
        const uint32_t idx = (fl->head_idx + 1u) % n;

        /* The sector about to be reused is the one seq - n lived in.  If that
         * is still ahead of the read cursor the ring is full: the oldest
         * sector goes, whole, and with it whatever was unread in it. */
        while (fl->pending > 0 && fl->rd_seq <= fl->wr_seq &&
               fl->rd_seq + n <= seq) {
            drop_rd_sector(fl);
        }
        settle_empty(fl);

        int err = prepare_sector(fl, idx);

        if (!err) {
            err = write_header(fl, idx, seq);
        }

        fl->st.opens++;
        fl->next_seq = seq + 1u;
        fl->head_idx = idx;
        fl->cnt[idx] = 0;

        if (!err) {
            fl->wr_seq = seq;
            fl->wr_off = SEC_HDR;
            if (fl->pending == 0) {
                fl->rd_seq = seq;
                fl->rd_off = SEC_HDR;
                fl->rd_left = 0;
            }
            return 0;
        }

        fl->st.skipped++;
        if (++fl->fails >= FLASHLOG_DEAD_AFTER) {
            fl->dead = true;
            return FLASHLOG_ERR_DEAD;
        }
    }
    return FLASHLOG_ERR_IO;
}

int flashlog_init(struct flashlog *fl, const struct flashlog_dev *dev,
                  uint16_t *cnt, const struct flashlog_cfg *cfg)
{
    if (!fl || !dev || !cnt || !dev->read || !dev->prog || !dev->erase ||
        dev->sectors < FLASHLOG_MIN_SECTORS) {
        return FLASHLOG_ERR_ARG;
    }

    memset(fl, 0, sizeof(*fl));
    fl->dev = dev;
    fl->cnt = cnt;
    if (cfg) {
        fl->cfg = *cfg;
    }
    memset(cnt, 0, dev->sectors * sizeof(cnt[0]));

    /* The newest sector header says where the ring has got to.  Nothing else
     * is read, and nothing is written. */
    uint32_t max_seq = 0;
    uint32_t max_idx = dev->sectors - 1u;

    for (uint32_t i = 0; i < dev->sectors; i++) {
        uint8_t h[SEC_HDR];
        uint32_t seq;
        int err = rd(fl, sec_addr(i), h, SEC_HDR);

        if (err) {
            return err;
        }
        if (hdr_decode(h, &seq) && seq > max_seq) {
            max_seq = seq;
            max_idx = i;
        }
    }
    fl->next_seq = max_seq + 1u;
    fl->head_idx = max_idx;
    return 0;
}

int flashlog_append(struct flashlog *fl, const void *rec, uint16_t len,
                    uint32_t now_s)
{
    if (!fl || !fl->dev || !rec || len == 0 || len > FLASHLOG_PAYLOAD_MAX) {
        return FLASHLOG_ERR_ARG;
    }
    if (fl->dead) {
        return FLASHLOG_ERR_DEAD;
    }

    const uint32_t need = REC_HDR + len;

    if (fl->wr_seq == 0 || fl->wr_off + need > SECTOR) {
        int err = open_next(fl, now_s);

        if (err) {
            return err;
        }
    }

    const uint32_t idx = idx_of(fl, fl->wr_seq);
    const uint32_t rec_off = fl->wr_off;
    const uint8_t *payload = rec;
    uint8_t hdr[REC_HDR];
    uint8_t stage[PAGE];
    uint16_t crc;

    hdr[0] = REC_TYPE;
    put16(hdr + 1, len);
    crc = crc16(0xFFFFu, hdr, 3);
    crc = crc16(crc, payload, len);
    put16(hdr + 3, crc);

    /* Header and payload as one run, a page at a time: a record is a few
     * program commands, not one per field. */
    uint32_t addr = sec_addr(idx) + rec_off;
    uint32_t done = 0;
    int err = 0;

    while (done < need && !err) {
        uint32_t chunk = PAGE - (addr % PAGE);

        if (chunk > need - done) {
            chunk = need - done;
        }
        for (uint32_t i = 0; i < chunk; i++) {
            uint32_t k = done + i;

            stage[i] = (k < REC_HDR) ? hdr[k] : payload[k - REC_HDR];
        }
        if (fl->dev->prog(fl->dev->ctx, addr, stage, chunk) != 0) {
            fl->st.io_errors++;
            err = FLASHLOG_ERR_IO;
        }
        addr += chunk;
        done += chunk;
    }

    if (err) {
        /* Whatever of the record got in is garbage after the last good one.
         * The sector is closed rather than written past it, and the next
         * append opens another: a chip that cannot be programmed is stopped
         * by FLASHLOG_DEAD_AFTER, not allowed to take an erase for each try. */
        fl->wr_off = SECTOR;
        if (++fl->fails >= FLASHLOG_DEAD_AFTER) {
            fl->dead = true;
        }
        return err;
    }

    const bool was_empty = (fl->pending == 0);

    fl->wr_off += need;
    fl->cnt[idx]++;
    fl->pending++;
    fl->st.appended++;
    fl->fails = 0;
    if (was_empty) {
        fl->rd_seq = fl->wr_seq;
        fl->rd_off = rec_off;
        fl->rd_left = 1;
    } else if (fl->rd_seq == fl->wr_seq) {
        fl->rd_left++;
    }
    return 0;
}

uint32_t flashlog_pending(const struct flashlog *fl)
{
    return fl->pending;
}

void flashlog_cursor(const struct flashlog *fl, struct flashlog_cur *c)
{
    c->seq = fl->rd_seq;
    c->off = fl->rd_off;
    c->left = fl->rd_left;
    c->n = 0;
    c->lost = 0;
    c->gen = fl->gen;
}

/* One try at the record at the cursor.  0 and the record in buf, or an error
 * with the cursor untouched. */
static int read_record(struct flashlog *fl, const struct flashlog_cur *c,
                       void *buf, uint16_t cap, uint16_t *len_out)
{
    const uint32_t addr = sec_addr(idx_of(fl, c->seq)) + c->off;
    uint8_t h[REC_HDR];

    if (rd(fl, addr, h, REC_HDR) != 0 || h[0] != REC_TYPE) {
        return FLASHLOG_ERR_CRC;
    }

    uint32_t len = get16(h + 1);

    if (len == 0 || len > FLASHLOG_PAYLOAD_MAX || c->off + REC_HDR + len > SECTOR) {
        return FLASHLOG_ERR_CRC;
    }
    if (len > cap) {
        /* Longer than the caller can take, so as good as lost to it.  Refusing
         * it and leaving it at the head would stop the queue on it for good. */
        return FLASHLOG_ERR_CRC;
    }
    if (rd(fl, addr + REC_HDR, buf, len) != 0) {
        return FLASHLOG_ERR_CRC;
    }

    uint16_t crc = crc16(0xFFFFu, h, 3);

    crc = crc16(crc, buf, len);
    if (crc != get16(h + 3)) {
        return FLASHLOG_ERR_CRC;
    }
    *len_out = (uint16_t)len;
    return 0;
}

int flashlog_next(struct flashlog *fl, struct flashlog_cur *c, void *buf,
                  uint16_t cap)
{
    if (c->gen != fl->gen) {
        return FLASHLOG_ERR_STALE;
    }
    while (c->left == 0) {
        if (fl->wr_seq == 0 || c->seq >= fl->wr_seq) {
            return 0;
        }
        c->seq++;
        c->off = SEC_HDR;
        c->left = fl->cnt[idx_of(fl, c->seq)];
    }

    uint16_t len = 0;

    /* A second look before a record is given up on: on a bus in a car a read
     * can be wrong when the flash is not. */
    int err = read_record(fl, c, buf, cap, &len);

    if (err == FLASHLOG_ERR_CRC) {
        err = read_record(fl, c, buf, cap, &len);
    }
    if (err == FLASHLOG_ERR_CRC) {
        /* Where the next record starts is in this one's header, so the rest of
         * the sector cannot be found. */
        c->n += c->left;
        c->lost += c->left;
        c->left = 0;
        return FLASHLOG_ERR_CRC;
    }
    if (err) {
        return err;
    }
    c->off += REC_HDR + len;
    c->left--;
    c->n++;
    return len;
}

int flashlog_commit(struct flashlog *fl, struct flashlog_cur *c)
{
    if (c->gen != fl->gen) {
        return FLASHLOG_ERR_STALE;
    }
    if (c->n > fl->pending) {
        return FLASHLOG_ERR_ARG;
    }

    fl->rd_seq = c->seq;
    fl->rd_off = c->off;
    fl->rd_left = c->left;
    fl->pending -= c->n;
    fl->st.consumed += c->n - c->lost;
    fl->st.lost += c->lost;

    /* Nothing left to read in the sector being written: whatever was lost
     * there is not waited for, and what is appended next is read next. */
    if (fl->rd_left == 0 && fl->rd_seq == fl->wr_seq) {
        fl->rd_off = fl->wr_off;
    }
    settle_empty(fl);

    fl->gen++;
    c->gen = fl->gen;
    c->n = 0;
    c->lost = 0;
    return 0;
}
