/*
 * NMEA decoding for the external GNSS driver's fallback.  See gnss_nmea.h;
 * this file has no dependencies beyond the C library.
 */

#include <string.h>

#include "gnss_nmea.h"

#define MAX_FIELDS 24
/* The stream parser keeps at most this much of a sentence. */
#define MAX_TEXT   UBX_NMEA_MAX

/* Split in place at the commas.  At most max fields; the rest is dropped. */
static int split(char *s, char **f, int max)
{
    int n = 0;

    f[n++] = s;
    for (; *s; s++) {
        if (*s == ',') {
            *s = '\0';
            if (n == max) {
                break;
            }
            f[n++] = s + 1;
        }
    }
    return n;
}

/* A field, or the empty string past the last one. */
static const char *fld(char **f, int n, int i)
{
    return i < n ? f[i] : "";
}

/* [-]digits[.digits] as an integer scaled by 10^frac, the first digit beyond
 * that rounding half away from zero.  False for an empty field or anything
 * in it but digits and one point, or for a number too big to be a sentence's. */
static bool parse_dec(const char *s, unsigned frac, int64_t *out)
{
    bool neg = false, point = false, any = false, up = false;
    unsigned got = 0, extra = 0, ndig = 0;
    int64_t v = 0;

    if (*s == '-') {
        neg = true;
        s++;
    }
    for (; *s; s++) {
        if (*s == '.') {
            if (point) {
                return false;
            }
            point = true;
            continue;
        }
        if (*s < '0' || *s > '9') {
            return false;
        }
        any = true;
        if (!point) {
            if (++ndig > 9) {
                return false;
            }
            v = v * 10 + (*s - '0');
        } else if (got < frac) {
            v = v * 10 + (*s - '0');
            got++;
        } else if (extra++ == 0) {
            up = (*s >= '5');
        }
    }
    if (!any) {
        return false;
    }
    for (; got < frac; got++) {
        v *= 10;
    }
    if (up) {
        v++;
    }
    *out = neg ? -v : v;
    return true;
}

static bool parse_u8(const char *s, uint8_t *out)
{
    int64_t v;

    if (!parse_dec(s, 0, &v) || v < 0 || v > 255) {
        return false;
    }
    *out = (uint8_t)v;
    return true;
}

/* ddmm.mmmm (latitude) or dddmm.mmmm (longitude) and its hemisphere letter to
 * 1e-7 degrees.  False for an empty or malformed field, minutes of 60 or
 * more, an angle past the pole or the antimeridian, or a hemisphere that is
 * neither letter. */
static bool parse_angle(const char *s, const char *hemi, char pos, char neg,
                        int32_t max_deg, int32_t *out)
{
    int64_t v;

    if (hemi[0] != pos && hemi[0] != neg) {
        return false;
    }
    if (!parse_dec(s, 7, &v) || v < 0) {
        return false;
    }

    const int64_t one_deg = 100LL * 10000000LL;      /* "100" of ddmm, scaled */
    int64_t deg = v / one_deg;
    int64_t min_e7 = v % one_deg;

    if (min_e7 >= 60LL * 10000000LL || deg > max_deg) {
        return false;
    }

    int64_t e7 = deg * 10000000LL + (min_e7 + 30) / 60;

    if (e7 > (int64_t)max_deg * 10000000LL) {
        return false;
    }
    *out = (int32_t)(hemi[0] == neg ? -e7 : e7);
    return true;
}

/* hhmmss.ss */
static bool parse_utc(const char *s, uint8_t *h, uint8_t *m, uint8_t *sec, uint32_t *usec)
{
    int64_t v;

    if (!parse_dec(s, 6, &v) || v < 0) {
        return false;
    }

    int64_t whole = v / 1000000;
    unsigned hh = (unsigned)(whole / 10000);
    unsigned mm = (unsigned)((whole / 100) % 100);
    unsigned ss = (unsigned)(whole % 100);

    if (hh > 23 || mm > 59 || ss > 60) {            /* 60: a leap second */
        return false;
    }
    *h = (uint8_t)hh;
    *m = (uint8_t)mm;
    *sec = (uint8_t)ss;
    *usec = (uint32_t)(v % 1000000);
    return true;
}

/* ddmmyy; the century is this one */
static bool parse_date(const char *s, uint8_t *day, uint8_t *mon, uint16_t *year)
{
    int64_t v;

    if (strlen(s) != 6 || !parse_dec(s, 0, &v)) {
        return false;
    }

    unsigned dd = (unsigned)(v / 10000);
    unsigned mm = (unsigned)((v / 100) % 100);

    if (dd < 1 || dd > 31 || mm < 1 || mm > 12) {
        return false;
    }
    *day = (uint8_t)dd;
    *mon = (uint8_t)mm;
    *year = (uint16_t)(2000 + v % 100);
    return true;
}

static void copy_utc(char *dst, size_t sz, const char *src)
{
    size_t n = strlen(src);

    if (n >= sz) {
        n = sz - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void nmea_reset(struct nmea_decoder *d)
{
    memset(d, 0, sizeof(*d));
}

/* An epoch is the RMC and the GGA that carry the same UTC time. */
static unsigned epoch_done(struct nmea_decoder *d)
{
    d->have_rmc = false;
    d->have_gga = false;
    d->epochs++;
    return NMEA_F_EPOCH;
}

static unsigned on_rmc(struct nmea_decoder *d, char **f, int n)
{
    unsigned flags = NMEA_F_RMC;
    const char *utc = fld(f, n, 1);
    bool active = fld(f, n, 2)[0] == 'A';
    bool mode_ok = fld(f, n, 12)[0] != 'N';         /* an older receiver has no mode field */
    int64_t v;

    /* The next epoch has begun, so what the satellite sentences said since
     * the last one is final. */
    if (d->gsv_seen) {
        d->sats.tracked = d->gsv_tracked;
        d->sats.best_cno = d->gsv_best;
        d->gsv_seen = false;
        d->gsv_tracked = 0;
        d->gsv_best = 0;
        flags |= NMEA_F_SATS;
    }

    memset(&d->rmc, 0, sizeof(d->rmc));
    d->rmc.time_ok = parse_utc(utc, &d->rmc.hour, &d->rmc.min, &d->rmc.sec, &d->rmc.usec);
    d->rmc.date_ok = parse_date(fld(f, n, 9), &d->rmc.day, &d->rmc.month, &d->rmc.year);
    d->rmc.fix = active && mode_ok &&
                 parse_angle(fld(f, n, 3), fld(f, n, 4), 'N', 'S', 90, &d->rmc.lat_e7) &&
                 parse_angle(fld(f, n, 5), fld(f, n, 6), 'E', 'W', 180, &d->rmc.lon_e7);
    if (parse_dec(fld(f, n, 7), 3, &v) && v >= 0) {             /* knots */
        d->rmc.has_speed = true;
        d->rmc.speed_kmh_e3 = (int32_t)((v * 1852 + 500) / 1000);
    }
    if (parse_dec(fld(f, n, 8), 2, &v) && v >= 0 && v < 36000) {
        d->has_course = true;
        d->course_e2 = (int32_t)v;
    }

    copy_utc(d->rmc_utc, sizeof(d->rmc_utc), utc);
    if (d->have_gga && strcmp(d->gga_utc, d->rmc_utc) == 0) {
        return flags | epoch_done(d);
    }
    d->have_rmc = true;
    d->have_gga = false;                            /* one from another epoch */
    return flags;
}

static unsigned on_gga(struct nmea_decoder *d, char **f, int n)
{
    const char *utc = fld(f, n, 1);
    int64_t v;

    memset(&d->gga, 0, sizeof(d->gga));
    (void)parse_u8(fld(f, n, 6), &d->gga.quality);
    (void)parse_u8(fld(f, n, 7), &d->gga.sats);
    if (parse_dec(fld(f, n, 8), 2, &v) && v >= 0 && v <= 65535) {
        d->gga.has_hdop = true;
        d->gga.hdop_e2 = (uint16_t)v;
    }
    if (parse_dec(fld(f, n, 9), 3, &v)) {
        d->gga.has_alt = true;
        d->gga.alt_mm = (int32_t)v;
    }
    if (parse_dec(fld(f, n, 11), 3, &v)) {
        d->gga.sep_mm = (int32_t)v;
    }

    copy_utc(d->gga_utc, sizeof(d->gga_utc), utc);
    if (d->have_rmc && strcmp(d->rmc_utc, d->gga_utc) == 0) {
        return epoch_done(d);
    }
    d->have_gga = true;
    d->have_rmc = false;
    return 0;
}

/* GSV: total sentences, this one, satellites in view, then four fields per
 * satellite (id, elevation, azimuth, C/N0) and, on a newer receiver, a signal
 * id.  C/N0 is empty for a satellite the receiver knows of but does not hear. */
static void on_gsv(struct nmea_decoder *d, char **f, int n)
{
    for (int k = 4; k + 3 < n; k += 4) {
        int64_t cno;

        if (parse_dec(f[k + 3], 0, &cno) && cno > 0) {
            if (d->gsv_tracked < 255) {
                d->gsv_tracked++;
            }
            if (cno > 255) {
                cno = 255;
            }
            if (cno > d->gsv_best) {
                d->gsv_best = (uint8_t)cno;
            }
        }
    }
    d->gsv_seen = true;
}

unsigned nmea_feed(struct nmea_decoder *d, const char *s, size_t len)
{
    char buf[MAX_TEXT + 1];
    char *f[MAX_FIELDS];

    /* "GNRMC" and a comma is the shortest thing worth looking at. */
    if (len < 6 || len > MAX_TEXT) {
        d->ignored++;
        return 0;
    }
    memcpy(buf, s, len);
    buf[len] = '\0';

    int n = split(buf, f, MAX_FIELDS);
    const char *tag = f[0];

    /* Two letters of talker and three of sentence type; "P..." is a
     * proprietary sentence, which is not for this. */
    if (strlen(tag) != 5 || tag[0] == 'P') {
        d->ignored++;
        return 0;
    }
    d->sentences++;
    tag += 2;
    if (strcmp(tag, "RMC") == 0) {
        return on_rmc(d, f, n);
    }
    if (strcmp(tag, "GGA") == 0) {
        return on_gga(d, f, n);
    }
    if (strcmp(tag, "GSV") == 0) {
        on_gsv(d, f, n);
        return 0;
    }
    return 0;
}

bool nmea_epoch_pvt(const struct nmea_decoder *d, struct ubx_pvt *o, uint16_t *hdop_e2)
{
    if (!d->rmc.fix || d->gga.quality == 0 || d->gga.sats == 0) {
        return false;
    }

    memset(o, 0, sizeof(*o));
    o->year = d->rmc.year;
    o->month = d->rmc.month;
    o->day = d->rmc.day;
    o->hour = d->rmc.hour;
    o->min = d->rmc.min;
    o->sec = d->rmc.sec;
    o->valid_date = d->rmc.date_ok;
    o->valid_time = d->rmc.time_ok;
    o->nano = (int32_t)(d->rmc.usec * 1000u);
    o->fix_type = d->gga.has_alt ? 3 : 2;
    o->fix_ok = true;
    o->num_sv = d->gga.sats;
    o->lon_e7 = d->rmc.lon_e7;
    o->lat_e7 = d->rmc.lat_e7;
    o->hmsl_mm = d->gga.alt_mm;
    /* GGA gives the height above mean sea level and, apart, how far the geoid
     * stands above the ellipsoid; NAV-PVT's height is the ellipsoid's. */
    o->height_mm = d->gga.alt_mm + d->gga.sep_mm;
    o->gspeed_mms = (int32_t)(((int64_t)d->rmc.speed_kmh_e3 * 5) / 18);   /* km/h to mm/s */
    o->head_mot_e5 = d->has_course ? d->course_e2 * 1000 : 0;
    *hdop_e2 = d->gga.has_hdop ? d->gga.hdop_e2 : 9999;
    return true;
}
