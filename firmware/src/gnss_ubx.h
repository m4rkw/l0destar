/*
 * u-blox UBX framing and the few messages the external GNSS driver
 * (gnss_ext.c) speaks to a MAX-M10S.  Plain C with no Zephyr or nRF
 * dependencies, so the same file can be built on the host and run over
 * captured receiver logs.
 *
 * Message layouts are those of the u-blox M10 interface description
 * (UBX-NAV-PVT, UBX-NAV-DOP, UBX-NAV-SAT, UBX-ACK, UBX-CFG-VALSET).  All
 * multi-byte fields are little-endian and unaligned in the frame, so they are
 * read byte by byte.
 */

#ifndef GNSS_UBX_H_
#define GNSS_UBX_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Largest payload kept.  UBX-NAV-SAT is the big one at 8 bytes plus 12 per
 * space vehicle; this holds 63 of them, more than the receiver lists.  A
 * longer frame is counted and skipped. */
#define UBX_MAX_PAYLOAD     768
/* Sync (2) + class + id + length (2) in front, checksum (2) behind. */
#define UBX_FRAME_OVERHEAD  8
/* A NMEA sentence is at most 82 characters; anything longer is not one. */
#define UBX_NMEA_MAX        100

/* -- message classes and ids ------------------------------------------------ */
#define UBX_CLS_NAV         0x01
#define UBX_ID_NAV_STATUS   0x03
#define UBX_ID_NAV_DOP      0x04
#define UBX_ID_NAV_PVT      0x07
#define UBX_ID_NAV_SAT      0x35

#define UBX_CLS_ACK         0x05
#define UBX_ID_ACK_NAK      0x00
#define UBX_ID_ACK_ACK      0x01

#define UBX_CLS_CFG         0x06
#define UBX_ID_CFG_VALSET   0x8A

#define UBX_CLS_MON         0x0A
#define UBX_ID_MON_VER      0x04

/* Assistance (AssistNow): the receiver is told the time, roughly where it is,
 * and the orbits.  INI-POS and INI-TIME share an id and differ in the first
 * payload byte. */
#define UBX_CLS_MGA         0x13
#define UBX_ID_MGA_ANO      0x20
#define UBX_ID_MGA_INI      0x40
#define UBX_ID_MGA_ACK      0x60
#define UBX_MGA_INI_POS_LLH 0x01
#define UBX_MGA_INI_TIME_UTC 0x10

/* The receiver's chip id; what AssistNow registers a unit by. */
#define UBX_CLS_SEC         0x27
#define UBX_ID_SEC_UNIQID   0x03

/* -- configuration keys (CFG-VALSET) ------------------------------------------
 * Built as size << 28 | group << 16 | item.  The size field is 1 or 2 for a
 * flag or single byte, 3 for two bytes and 4 for four. */
#define UBX_KEY_UART1_BAUDRATE        0x40520001u   /* U4 */
#define UBX_KEY_UART1INPROT_UBX       0x10730001u   /* L  */
#define UBX_KEY_UART1OUTPROT_UBX      0x10740001u   /* L  */
#define UBX_KEY_UART1OUTPROT_NMEA     0x10740002u   /* L  */
#define UBX_KEY_RATE_MEAS             0x30210001u   /* U2, ms between measurements */
#define UBX_KEY_RATE_NAV              0x30210002u   /* U2, measurements per solution */
#define UBX_KEY_MSGOUT_NAV_PVT_UART1  0x20910007u   /* U1, output every n epochs */
#define UBX_KEY_MSGOUT_NAV_DOP_UART1  0x20910039u   /* U1 */
#define UBX_KEY_MSGOUT_NAV_SAT_UART1  0x20910016u   /* U1 */
#define UBX_KEY_NAVSPG_DYNMODEL       0x20110021u   /* E1 */
#define UBX_KEY_NAVSPG_ACKAIDING      0x10110025u   /* L, UBX-MGA-ACK for every assistance message */

#define UBX_DYNMODEL_AUTOMOTIVE       4

/* CFG-VALSET layer bits: the active (volatile) configuration, the battery
 * backed one that survives a VCC cycle while V_BCKP is held, and flash. */
#define UBX_LAYER_RAM       0x01
#define UBX_LAYER_BBR       0x02
#define UBX_LAYER_FLASH     0x04

/* -- stream parser ------------------------------------------------------------ */

enum ubx_event {
    UBX_EV_NONE = 0,
    UBX_EV_FRAME,    /* a UBX frame with a good checksum: cls/id/len/payload */
    UBX_EV_NMEA,     /* a NMEA sentence with a good checksum: the text between '$' and '*'
                      * is in payload, NUL terminated, and its length in len */
};

struct ubx_parser {
    uint8_t  state;
    uint8_t  cls;
    uint8_t  id;
    uint16_t len;
    uint16_t pos;
    uint8_t  ck_a;
    uint8_t  ck_b;
    uint8_t  nmea_cs;
    uint8_t  nmea_len;
    int8_t   nmea_hi;
    uint8_t  payload[UBX_MAX_PAYLOAD];
    /* Running totals, for the log when something is wrong. */
    uint32_t frames;
    uint32_t nmea;
    uint32_t bad_cks;
    uint32_t oversize;
};

void ubx_parser_reset(struct ubx_parser *p);

/* Feed one byte.  UBX_EV_FRAME leaves the frame in cls, id, len and payload
 * until the next call, UBX_EV_NMEA a sentence in payload and len.  Garbage
 * between frames, a bad checksum and a frame longer than UBX_MAX_PAYLOAD are
 * all dropped, and the next sync pattern starts a new frame.  A receiver that
 * has not been configured yet sends NMEA: that tells it from one that is
 * silent, and if it will not take configuration the sentences are all there
 * is (gnss_nmea.h). */
enum ubx_event ubx_parse_byte(struct ubx_parser *p, uint8_t b);

/* -- decoded messages -------------------------------------------------------- */

struct ubx_pvt {
    uint32_t itow_ms;
    uint16_t year;
    uint8_t  month, day, hour, min, sec;
    bool     valid_date, valid_time;
    int32_t  nano;            /* ns, may be negative */
    uint8_t  fix_type;        /* 0 none, 1 dead reckoning, 2 2D, 3 3D, 4 GNSS + DR, 5 time only */
    bool     fix_ok;          /* gnssFixOK: within the receiver's accuracy and DOP masks */
    uint8_t  num_sv;          /* satellites used in the solution */
    int32_t  lon_e7, lat_e7;  /* 1e-7 degrees */
    int32_t  height_mm;       /* above the WGS-84 ellipsoid */
    int32_t  hmsl_mm;         /* above mean sea level */
    uint32_t hacc_mm;
    int32_t  gspeed_mms;      /* ground speed */
    int32_t  head_mot_e5;     /* heading of motion, 1e-5 degrees */
    uint16_t pdop_e2;
};

struct ubx_dop {
    uint32_t itow_ms;
    uint16_t hdop_e2;
    uint16_t pdop_e2;
};

struct ubx_sat_summary {
    uint32_t itow_ms;
    uint8_t  listed;          /* space vehicles in the message */
    uint8_t  tracked;         /* of those, with a signal (C/N0 > 0) */
    uint8_t  used;            /* of those, used in the solution */
    uint8_t  best_cno;        /* strongest, dBHz */
    uint8_t  eph;             /* of those, with an ephemeris on hand (ephAvail) */
    uint8_t  alm;             /* ... with an almanac (almAvail) */
    uint8_t  ano;             /* ... with AssistNow orbit data (anoAvail) */
};

/* UBX-MGA-ACK, the receiver's answer to one assistance message when
 * CFG-NAVSPG-ACKAIDING is on: `accepted` is the type field, `info` says why
 * not (1 = it does not know the time yet, so no UTC time assistance came
 * first; 5 = not ready), `msg_id` which message it answers. */
struct ubx_mga_ack {
    bool    accepted;
    uint8_t info;
    uint8_t msg_id;
};

bool ubx_decode_nav_pvt(const uint8_t *pl, uint16_t len, struct ubx_pvt *out);
bool ubx_decode_nav_dop(const uint8_t *pl, uint16_t len, struct ubx_dop *out);
bool ubx_decode_nav_sat(const uint8_t *pl, uint16_t len, struct ubx_sat_summary *out);
/* class and id the ACK or NAK answers; false if the payload is not one */
bool ubx_decode_ack(const uint8_t *pl, uint16_t len, uint8_t *cls, uint8_t *id);
bool ubx_decode_mga_ack(const uint8_t *pl, uint16_t len, struct ubx_mga_ack *out);

/* -- text the rest of the firmware wants ---------------------------------------- */

/* "%.6f" of an angle held in 1e-7 degrees, in integers (rounded half away
 * from zero).  out needs 13 bytes for the widest value, "-180.000000". */
void ubx_fmt_deg6(int32_t e7, char *out, size_t sz);

/* "DD/MM/YY,HH:MM:SS.uuuuuu+00", the timestamp format of the telemetry
 * record.  Empty and false if the receiver does not yet know the date and
 * time. */
bool ubx_fmt_time(const struct ubx_pvt *p, char *out, size_t sz);

/* -- messages to the receiver ----------------------------------------------------- */

struct ubx_cfg_item {
    uint32_t key;
    uint32_t val;
};

/* A complete frame, or 0 if it does not fit in cap. */
size_t ubx_build_frame(uint8_t *out, size_t cap, uint8_t cls, uint8_t id,
                       const uint8_t *payload, uint16_t len);
/* A poll request: a frame with no payload. */
size_t ubx_build_poll(uint8_t *out, size_t cap, uint8_t cls, uint8_t id);
/* CFG-VALSET for the given layers.  Value sizes come from the keys; 0 if
 * a key has a size this does not handle or the frame does not fit. */
size_t ubx_build_valset(uint8_t *out, size_t cap, uint8_t layers,
                        const struct ubx_cfg_item *items, size_t n);

/* -- assistance ------------------------------------------------------------------ */

/* A calendar time in UTC, with its accuracy. */
struct ubx_utc {
    uint16_t year;
    uint8_t  month, day, hour, min, sec;     /* month and day from 1 */
    uint32_t ns;
    uint16_t tacc_s;                         /* accuracy: seconds ... */
    uint32_t tacc_ns;                        /* ... and nanoseconds */
};

/* Unix time in milliseconds to a calendar time, no accuracy filled in.
 * False before 2020, which is a clock that was never set. */
bool ubx_utc_from_unix_ms(int64_t unix_ms, struct ubx_utc *out);

/* UBX-MGA-INI-TIME_UTC: the time as of the moment the receiver reads it
 * (reference "none"), the leap second count unknown.  0 if it does not fit. */
size_t ubx_build_mga_ini_time_utc(uint8_t *out, size_t cap, const struct ubx_utc *t);

/* UBX-MGA-INI-POS_LLH.  The accuracy is a standard deviation in centimetres
 * and must not be optimistic: a position wrong by more than it does harm. */
size_t ubx_build_mga_ini_pos_llh(uint8_t *out, size_t cap, int32_t lat_e7,
                                 int32_t lon_e7, int32_t alt_cm, uint32_t pos_acc_cm);

/* The frame at the start of buf: its length if it is complete and its
 * checksum good; 0 if more bytes are needed to tell; -1 if buf does not start
 * with a frame (no sync, too long, bad checksum), whereupon the caller skips
 * a byte and tries again.  For walking a downloaded run of assistance messages. */
int ubx_frame_scan(const uint8_t *buf, size_t avail);

#endif /* GNSS_UBX_H_ */
