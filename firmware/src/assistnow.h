/*
 * u-blox AssistNow for the external receiver (assistnow.c).
 *
 * The receiver driver (gnss_ext.c) decides when assistance is wanted and owns
 * the UART; assistnow.c fetches the data from the l0destar server and has the
 * driver pass it on.  Only the external receiver is assisted: the nRF9151's
 * own gets its assistance from nRF Cloud (agnss.c).
 */
#ifndef ASSISTNOW_H_
#define ASSISTNOW_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What reached the receiver, from its UBX-MGA-ACK replies. */
struct gnss_assist_result {
    uint16_t sent;          /* assistance messages written to the receiver */
    uint16_t accepted;      /* ... that it acknowledged as taken */
    uint16_t refused;       /* ... and as not used (UBX-MGA-ACK type 0) */
    uint8_t  first_info;    /* infoCode of the first refusal; 1: it did not know the time */
};

#ifdef CONFIG_APP_ASSISTNOW

/* The driver's side of it, in gnss_ext.c. */

/* The receiver's replies to the chip id (UBX-SEC-UNIQID) and version
 * (UBX-MON-VER) polls, byte for byte as hex, which is how the service wants
 * them.  0, or -EAGAIN while either has yet to arrive, -ENOSPC if a buffer is
 * too small (the version reply is about 240 bytes). */
int  gnss_ext_ids(char *uid_hex, size_t uid_sz, char *ver_hex, size_t ver_sz);
/* The receiver is up, configured, has no fix and has not been assisted this
 * power-up. */
bool gnss_ext_assist_useful(void);
/* Send the UBX-MGA messages in `data` to the receiver, a few at a time with
 * the acknowledgements watched, and wait for the last of them.  Only
 * assistance goes through: any other frame in the data, and anything that
 * does not check out, is skipped.  Blocks for a few seconds.  0, or -ENODEV
 * if the receiver went away, -EIO if the UART refused. */
int  gnss_ext_assist_inject(const uint8_t *data, size_t len, struct gnss_assist_result *res);
/* Assistance is done for this power-up (given, or given up on). */
void gnss_ext_assist_done(void);

/* From the driver's thread: the receiver has come up without ephemerides.
 * Does not block; the work is done on assistnow.c's own thread. */
void assistnow_request(void);

/* One line on where it stands, for the log and the board test. */
const char *assistnow_status(void);

#else

static inline void assistnow_request(void) {}

#endif /* CONFIG_APP_ASSISTNOW */

#endif /* ASSISTNOW_H_ */
