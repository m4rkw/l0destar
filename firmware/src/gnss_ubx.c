/*
 * u-blox UBX framing and message decoding for the external GNSS driver.
 * See gnss_ubx.h; this file has no dependencies beyond the C library.
 */

#include <stdio.h>
#include <string.h>

#include "gnss_ubx.h"

enum {
    S_SYNC1,
    S_SYNC2,
    S_CLASS,
    S_ID,
    S_LEN1,
    S_LEN2,
    S_PAYLOAD,
    S_CKA,
    S_CKB,
    S_NMEA_BODY,
    S_NMEA_CK1,
    S_NMEA_CK2,
};

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline int32_t rd32s(const uint8_t *p)
{
    return (int32_t)rd32(p);
}

static inline void ck_add(uint8_t *a, uint8_t *b, uint8_t x)
{
    *a = (uint8_t)(*a + x);
    *b = (uint8_t)(*b + *a);
}

static int hexval(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

void ubx_parser_reset(struct ubx_parser *p)
{
    p->state = S_SYNC1;
}

static void nmea_start(struct ubx_parser *p)
{
    p->state = S_NMEA_BODY;
    p->nmea_cs = 0;
    p->nmea_len = 0;
}

enum ubx_event ubx_parse_byte(struct ubx_parser *p, uint8_t b)
{
    switch (p->state) {
    case S_SYNC1:
        if (b == 0xB5) {
            p->state = S_SYNC2;
        } else if (b == '$') {
            nmea_start(p);
        }
        break;

    case S_SYNC2:
        if (b == 0x62) {
            p->state = S_CLASS;
        } else if (b == '$') {
            nmea_start(p);
        } else if (b != 0xB5) {
            p->state = S_SYNC1;
        }
        break;

    case S_CLASS:
        p->cls = b;
        p->ck_a = 0;
        p->ck_b = 0;
        ck_add(&p->ck_a, &p->ck_b, b);
        p->state = S_ID;
        break;

    case S_ID:
        p->id = b;
        ck_add(&p->ck_a, &p->ck_b, b);
        p->state = S_LEN1;
        break;

    case S_LEN1:
        p->len = b;
        ck_add(&p->ck_a, &p->ck_b, b);
        p->state = S_LEN2;
        break;

    case S_LEN2:
        p->len |= (uint16_t)(b << 8);
        ck_add(&p->ck_a, &p->ck_b, b);
        p->pos = 0;
        if (p->len > UBX_MAX_PAYLOAD) {
            /* Not skipped: the payload bytes are fed through the sync search
             * instead, so a corrupt length cannot swallow the frames behind
             * it.  A false sync inside a payload fails its checksum. */
            p->oversize++;
            p->state = S_SYNC1;
        } else {
            p->state = p->len ? S_PAYLOAD : S_CKA;
        }
        break;

    case S_PAYLOAD:
        p->payload[p->pos++] = b;
        ck_add(&p->ck_a, &p->ck_b, b);
        if (p->pos == p->len) {
            p->state = S_CKA;
        }
        break;

    case S_CKA:
        if (b == p->ck_a) {
            p->state = S_CKB;
        } else {
            p->bad_cks++;
            p->state = S_SYNC1;
        }
        break;

    case S_CKB:
        p->state = S_SYNC1;
        if (b == p->ck_b) {
            p->frames++;
            return UBX_EV_FRAME;
        }
        p->bad_cks++;
        break;

    case S_NMEA_BODY:
        if (b == '*') {
            p->state = S_NMEA_CK1;
        } else if (b == '$') {
            nmea_start(p);
        } else if (b < 0x20 || b > 0x7E || p->nmea_len >= UBX_NMEA_MAX) {
            /* Not a sentence after all.  A UBX sync byte may be what ended it. */
            p->state = (b == 0xB5) ? S_SYNC2 : S_SYNC1;
        } else {
            p->nmea_cs ^= b;
            p->nmea_len++;
        }
        break;

    case S_NMEA_CK1:
        p->nmea_hi = (int8_t)hexval(b);
        p->state = (p->nmea_hi < 0) ? S_SYNC1 : S_NMEA_CK2;
        break;

    case S_NMEA_CK2: {
        int lo = hexval(b);

        p->state = S_SYNC1;
        if (lo >= 0 && ((p->nmea_hi << 4) | lo) == p->nmea_cs) {
            p->nmea++;
            return UBX_EV_NMEA;
        }
        break;
    }

    default:
        p->state = S_SYNC1;
        break;
    }
    return UBX_EV_NONE;
}

/* -- decoders -------------------------------------------------------------- */

bool ubx_decode_nav_pvt(const uint8_t *pl, uint16_t len, struct ubx_pvt *o)
{
    if (len < 92) {
        return false;
    }
    o->itow_ms    = rd32(pl + 0);
    o->year       = rd16(pl + 4);
    o->month      = pl[6];
    o->day        = pl[7];
    o->hour       = pl[8];
    o->min        = pl[9];
    o->sec        = pl[10];
    o->valid_date = (pl[11] & 0x01) != 0;
    o->valid_time = (pl[11] & 0x02) != 0;
    o->nano       = rd32s(pl + 16);
    o->fix_type   = pl[20];
    o->fix_ok     = (pl[21] & 0x01) != 0;
    o->num_sv     = pl[23];
    o->lon_e7     = rd32s(pl + 24);
    o->lat_e7     = rd32s(pl + 28);
    o->height_mm  = rd32s(pl + 32);
    o->hmsl_mm    = rd32s(pl + 36);
    o->hacc_mm    = rd32(pl + 40);
    o->gspeed_mms = rd32s(pl + 60);
    o->head_mot_e5 = rd32s(pl + 64);
    o->pdop_e2    = rd16(pl + 76);
    return true;
}

bool ubx_decode_nav_dop(const uint8_t *pl, uint16_t len, struct ubx_dop *o)
{
    if (len < 18) {
        return false;
    }
    o->itow_ms = rd32(pl + 0);
    o->pdop_e2 = rd16(pl + 6);
    o->hdop_e2 = rd16(pl + 12);
    return true;
}

bool ubx_decode_nav_sat(const uint8_t *pl, uint16_t len, struct ubx_sat_summary *o)
{
    if (len < 8 || pl[4] != 1) {
        return false;
    }

    uint8_t n = pl[5];

    if (len < 8u + 12u * n) {
        return false;
    }
    o->itow_ms = rd32(pl + 0);
    o->listed = n;
    o->tracked = 0;
    o->used = 0;
    o->best_cno = 0;
    for (uint8_t i = 0; i < n; i++) {
        const uint8_t *sv = pl + 8 + 12 * i;
        uint8_t cno = sv[2];
        uint32_t flags = rd32(sv + 8);

        if (cno > 0) {
            o->tracked++;
        }
        if (cno > o->best_cno) {
            o->best_cno = cno;
        }
        if (flags & 0x08) {         /* svUsed */
            o->used++;
        }
    }
    return true;
}

bool ubx_decode_ack(const uint8_t *pl, uint16_t len, uint8_t *cls, uint8_t *id)
{
    if (len < 2) {
        return false;
    }
    *cls = pl[0];
    *id = pl[1];
    return true;
}

/* -- text ------------------------------------------------------------------ */

void ubx_fmt_deg6(int32_t e7, char *out, size_t sz)
{
    int64_t v = e7;
    bool neg = v < 0;

    if (neg) {
        v = -v;
    }
    v = (v + 5) / 10;       /* 1e-7 to 1e-6 degrees */
    snprintf(out, sz, "%s%u.%06u", (neg && v) ? "-" : "",
             (unsigned)(v / 1000000), (unsigned)(v % 1000000));
}

bool ubx_fmt_time(const struct ubx_pvt *p, char *out, size_t sz)
{
    if (sz == 0) {
        return false;
    }
    out[0] = '\0';
    if (!p->valid_date || !p->valid_time || p->year < 2000) {
        return false;
    }

    /* The epoch is within a millisecond of the second, and nano is signed:
     * a fix 175 us before 08:02:47 reads 47 s and -175377 ns.  A negative
     * fraction is dropped rather than carried into the previous second. */
    unsigned us = p->nano > 0 ? (unsigned)(p->nano / 1000) : 0;

    if (us > 999999) {
        us = 999999;
    }
    snprintf(out, sz, "%02u/%02u/%02u,%02u:%02u:%02u.%06u+00",
             p->day, p->month, (unsigned)(p->year % 100),
             p->hour, p->min, p->sec, us);
    return true;
}

/* -- building frames ----------------------------------------------------------- */

/* Put the header and checksum round a payload already sitting at out[6]. */
static size_t frame_finish(uint8_t *out, uint8_t cls, uint8_t id, uint16_t len)
{
    out[0] = 0xB5;
    out[1] = 0x62;
    out[2] = cls;
    out[3] = id;
    out[4] = (uint8_t)(len & 0xFF);
    out[5] = (uint8_t)(len >> 8);

    uint8_t a = 0, b = 0;

    for (size_t i = 2; i < (size_t)len + 6; i++) {
        ck_add(&a, &b, out[i]);
    }
    out[len + 6] = a;
    out[len + 7] = b;
    return (size_t)len + UBX_FRAME_OVERHEAD;
}

size_t ubx_build_frame(uint8_t *out, size_t cap, uint8_t cls, uint8_t id,
                       const uint8_t *payload, uint16_t len)
{
    if (cap < (size_t)len + UBX_FRAME_OVERHEAD) {
        return 0;
    }
    if (len) {
        memcpy(&out[6], payload, len);
    }
    return frame_finish(out, cls, id, len);
}

size_t ubx_build_poll(uint8_t *out, size_t cap, uint8_t cls, uint8_t id)
{
    return ubx_build_frame(out, cap, cls, id, NULL, 0);
}

static int key_size(uint32_t key)
{
    switch ((key >> 28) & 7) {
    case 1:     /* one bit, stored in a byte */
    case 2:
        return 1;
    case 3:
        return 2;
    case 4:
        return 4;
    default:
        return 0;
    }
}

size_t ubx_build_valset(uint8_t *out, size_t cap, uint8_t layers,
                        const struct ubx_cfg_item *items, size_t n)
{
    /* Assembled where it will be sent: the payload starts at out[6]. */
    uint8_t *pl = out + 6;
    size_t room = cap > UBX_FRAME_OVERHEAD ? cap - UBX_FRAME_OVERHEAD : 0;
    size_t off = 4;

    if (room < off) {
        return 0;
    }
    pl[0] = 0;          /* version: no transaction */
    pl[1] = layers;
    pl[2] = 0;
    pl[3] = 0;
    for (size_t i = 0; i < n; i++) {
        int sz = key_size(items[i].key);

        if (sz == 0 || off + 4 + (size_t)sz > room || off + 4 + sz > UBX_MAX_PAYLOAD) {
            return 0;
        }
        for (int k = 0; k < 4; k++) {
            pl[off++] = (uint8_t)(items[i].key >> (8 * k));
        }
        for (int k = 0; k < sz; k++) {
            pl[off++] = (uint8_t)(items[i].val >> (8 * k));
        }
    }
    return frame_finish(out, UBX_CLS_CFG, UBX_ID_CFG_VALSET, (uint16_t)off);
}
