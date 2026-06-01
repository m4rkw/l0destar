/*
 * Hardware stubs for missing peripherals.  Each function returns a value
 * that lets the rest of the firmware exercise its real code paths.
 *
 *   accel_read    : zeros — no movement events.  Wake-on-motion is a no-op.
 *   ignition_read : 0 = ON (active-low / inverted, matching the Polaris
 *                    convention).  The DK doesn't have an ignition sense pin,
 *                    so we report continuous ignition-on to drive the
 *                    active code paths during bring-up.
 *   battery_read_voltage : 12.3V — above SLEEP_SAFETY_VOLTAGE so timer wakes
 *                    don't get skipped, and below ENGINE_RUNNING_VOLTAGE so
 *                    the firmware picks the "ignition on, engine off" cadence
 *                    (IGNITION_ON_SLEEP_INTERVAL, 30s) rather than 1Hz GPS.
 *
 * Relay-related calls are compiled out via RELAY_CONNECTED=0.
 */

#include "app.h"

int accel_read(int *ax, int *ay, int *az)
{
    if (ax) *ax = 0;
    if (ay) *ay = 0;
    if (az) *az = 0;
    return 0;
}

int ignition_read(void)
{
    return 0;   /* inverted: 0 = ON */
}

float battery_read_voltage(void)
{
    return 12.3f;
}
