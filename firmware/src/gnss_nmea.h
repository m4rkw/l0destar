/*
 * The position, time and satellite count out of the NMEA sentences a u-blox
 * receiver sends by itself, for the external GNSS driver (gnss_ext.c) to fall
 * back on when the receiver can be heard but will not take configuration (its
 * UART RX not reaching it, UBX input switched off).  Plain C with no Zephyr or
 * nRF dependencies, like gnss_ubx.c, so it builds on the host and runs over
 * captured receiver output.
 *
 * What it needs is what a MAX-M10S sends out of the box, once a second:
 * RMC (time, date, position, speed, course) and GGA (fix quality, satellites
 * used, HDOP, altitude), and GSV for the satellites in view.  Everything else
 * is ignored.  The sentences arrive checksummed from the stream parser in
 * gnss_ubx.c, which hands over the text between '$' and '*'.
 *
 * A fix comes out as the same struct ubx_pvt that UBX-NAV-PVT decodes to, so
 * the driver has one path from there to the telemetry whichever the source.
 */

#ifndef GNSS_NMEA_H_
#define GNSS_NMEA_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "gnss_ubx.h"

/* Returned by nmea_feed(), ORed together. */
#define NMEA_F_RMC   0x01   /* an RMC arrived: the receiver is alive and an epoch has begun */
#define NMEA_F_EPOCH 0x02   /* RMC and GGA of one epoch are both in: see nmea_epoch_pvt() */
#define NMEA_F_SATS  0x04   /* the satellite summary of the epoch just ended is in d->sats */

struct nmea_sat_summary {
    uint8_t tracked;        /* satellites with a signal (a C/N0 in GSV) */
    uint8_t best_cno;       /* strongest, dBHz */
};

struct nmea_decoder {
    /* The epoch being assembled.  RMC and GGA are paired by their UTC time
     * field, so a lost sentence cannot pair one epoch with the next; with no
     * fix the field is empty in both and they pair as empty. */
    bool     have_rmc, have_gga;
    char     rmc_utc[16], gga_utc[16];

    struct {
        bool     fix;               /* status A, with a position */
        bool     time_ok, date_ok;
        uint8_t  hour, min, sec;
        uint32_t usec;
        uint8_t  day, month;
        uint16_t year;
        int32_t  lat_e7, lon_e7;    /* 1e-7 degrees */
        bool     has_speed;
        int32_t  speed_kmh_e3;      /* km/h x 1000 */
    } rmc;

    struct {
        uint8_t  quality;           /* 0 none, 1 GPS, 2 DGPS, 6 estimated */
        uint8_t  sats;              /* used in the solution */
        bool     has_hdop;
        uint16_t hdop_e2;           /* HDOP x 100 */
        bool     has_alt;
        int32_t  alt_mm;            /* above mean sea level */
        int32_t  sep_mm;            /* geoid above the ellipsoid; 0 if not sent */
    } gga;

    /* Course over ground is empty while the receiver has no heading to give,
     * so the last one is carried, as NAV-PVT's heading of motion would be. */
    bool     has_course;
    int32_t  course_e2;             /* degrees x 100 */

    /* Satellites, accumulated over the GSV sentences until the next RMC. */
    bool     gsv_seen;
    uint8_t  gsv_tracked, gsv_best;
    struct nmea_sat_summary sats;

    /* Running totals, for the log when something is wrong. */
    uint32_t sentences, ignored, epochs;
};

void nmea_reset(struct nmea_decoder *d);

/* Feed one sentence as the stream parser leaves it: the text between '$' and
 * '*', not NUL terminated.  Returns NMEA_F_* flags; sentences of any other
 * kind return 0. */
unsigned nmea_feed(struct nmea_decoder *d, const char *s, size_t len);

/* The position of the last completed epoch as a NAV-PVT, and its HDOP x 100.
 * False if the receiver did not have a fix then (RMC not active, GGA quality
 * 0, or no satellites), in which case nothing is written. */
bool nmea_epoch_pvt(const struct nmea_decoder *d, struct ubx_pvt *pvt, uint16_t *hdop_e2);

#endif /* GNSS_NMEA_H_ */
