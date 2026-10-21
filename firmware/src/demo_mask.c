/*
 * Demo mode's coordinate masker for the console.  Used by whichever receiver
 * driver is built (gnss.c or gnss_ext.c); moved here from gnss.c unchanged.
 */

#include <stddef.h>
#include <ctype.h>

#include "app.h"

/*
 * Demo mode: swap coordinates for DEMO_COORD_MASK on their way to the console.
 * Only the log is masked — the telemetry record and the alert text still carry
 * the real position to the server.
 *
 * Coordinates are recognised by shape rather than by position, so one pass
 * serves a CSV telemetry record, an alert string, or anything else that prints
 * one: an optional sign, digits, a dot, and five or more decimals, starting a
 * field.  lat/lon are the only "%.6f" the firmware emits, so demanding five
 * decimals leaves the "%.2f" voltages and "%.1f" temperatures alone, and
 * demanding a field start (never a ':') keeps the timestamp's fractional
 * seconds readable.  The masked copy is never longer than the input: a
 * six-decimal coordinate is at least 8 characters and the mask is 7.
 */
const char *demo_mask_coords(const char *in, size_t len,
                             char *out, size_t out_sz)
{
    size_t o = 0;

    if (out_sz == 0) return out;

    for (size_t i = 0; i < len && o < out_sz - 1; ) {
        bool field_start = (i == 0) || in[i - 1] == ',' ||
                           in[i - 1] == ' ' || in[i - 1] == '=';
        size_t j = i, int_digits = 0;

        if (field_start) {
            if (in[j] == '-' || in[j] == '+') j++;
            while (j < len && isdigit((unsigned char)in[j])) { j++; int_digits++; }

            if (int_digits > 0 && j < len && in[j] == '.') {
                size_t k = j + 1, decimals = 0;
                while (k < len && isdigit((unsigned char)in[k])) { k++; decimals++; }

                if (decimals >= 5) {
                    for (const char *m = DEMO_COORD_MASK;
                         *m && o < out_sz - 1; m++) {
                        out[o++] = *m;
                    }
                    i = k;
                    continue;
                }
            }
        }
        out[o++] = in[i++];
    }
    out[o] = '\0';
    return out;
}
