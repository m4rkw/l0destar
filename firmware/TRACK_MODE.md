# Track mode

> **Drive safely and responsibly.**  Track mode is for closed circuits,
> private land and the bench.  On a public road, obey the law and the
> conditions, and keep your eyes on the road and off the dashboard: the
> figures this mode produces are for looking at afterwards, not while
> driving.  You alone are responsible for how you drive and for any
> consequences of using this software.  The author accepts no liability for
> your actions, for any loss or damage, or for any fine, penalty or injury
> arising from its use.

A mode for track days and bench runs: the server switches it on, the
tracker stops the GNSS receiver and streams the ECU and the IMU at the
fastest cadence the K wire and LTE-M sensibly allow, and the tracking page
replaces the map with a live dashboard.  Switching it off puts everything
back.

## Switching it on and off

The switch lives on the server, on the `device` row (`track_mode`).  The
tracking page's **track** link toggles it (`POST /api/1.0/trackmode
{"on": 1}`), and it can be set the same way from anything that can log in.

Every server response to the device ends with `track=<0|1>`, next to the
`fota=<ver>` indication, so the device converges on the setting after a
reboot or a missed reply.  The firmware acts only on a change and raises an
alert ("track mode ON"/"OFF") when it does.  Nothing is persisted on the
device; it starts every boot with the mode off and learns the setting from
the first reply.

How long the switch takes to reach the device depends on when it next reads
a reply:

| Device state | Reads a reply | Latency |
|---|---|---|
| ignition on, engine off | every `APP_IGNITION_ON_SLEEP_INTERVAL` (30 s) | ≤ 30 s |
| driving | every `APP_RESP_POLL_S` (30 s), plus at stops | ≤ 30 s |
| already in track mode | every `APP_TRACK_RESP_INTERVAL_S` (10 s) | ≤ 10 s |
| asleep (ignition off) | on the next timed wake or ignition-on | — |

Driving devices did not read replies at all before this: a send while
moving released the radio immediately so GNSS got the antenna back.  They
now read one at least every `APP_RESP_POLL_S`, which is what makes the
switch usable from the pits.

## What the device does

`do_track()` in `main.c` takes over from every awake state while the switch
is on and the ignition is on.  Per cycle:

1. Impact check, K-wire housekeeping, ignition read.
2. `collect_track_data()` builds one record (below).
3. `send_data()`.  Every `APP_TRACK_RESP_INTERVAL_S` the reply is waited
   for; otherwise the datagram goes out and the loop moves on.
4. Idle out the rest of `APP_TRACK_PERIOD_MS` (default 500 ms).

The K-wire poll is the bulk of a cycle, so the real cadence is about two
records a second.  The transport is put in streaming mode for the duration:
the socket stays open and the RAI hint is `RAI_ONGOING`, so the modem keeps
the RRC connection up between sends rather than releasing and re-acquiring
it for each one.  With GNSS stopped there is nothing else wanting the radio.

The server answers every datagram, whether or not the device waits for
the reply, and with the socket held open those unread replies would queue
up in the modem.  Each is authenticated against the nonce of the request it
answers, so reading the oldest one back when a reply is finally wanted
fails the tag check and hides the reply that was wanted behind it.  The
transport therefore discards whatever is queued before every streaming
send, and a stale reply that was still in flight at that moment is skipped
while waiting for the right one.

Key-off ends the mode the way the other awake loops end: one final record
from the cached position carrying the ignition state, then sleep.  A
switch-off from the server hands back to the normal state machine with the
receiver resumed; the next cycle re-acquires a fix.

### Not overloading the K wire

A normal telemetry record reads thirteen PIDs, about 1.3 s of bus time.
Track mode's `obd_poll_track()` reads the four that move on a timescale a
driver can see every cycle — engine RPM, vehicle speed, throttle, load —
and one of the eight slow-moving ones in rotation: coolant, intake, MAF,
timing, STFT, LTFT, fuel system status, lamp/stored-code count.  Four or
five exchanges a cycle, each slow value refreshing about every four
seconds.  The session is held open for the drive as always; the reopen rate
limiter and the fault-code watch work unchanged.

### The IMU burst

The ASM330 batches accel and gyro at 26 Hz into its FIFO while the tracker
is awake (that ring is what impact forensics drain).  Each track record
drains it with `accel_fifo_drain_samples()` and carries up to
`APP_TRACK_IMU_SAMPLES` (default 16) samples, evenly spaced across the
interval when the cycle ran long, so the stream thins rather than gapping.
That is a 26 Hz accelerometer and gyro trace with no extra thread, no
interrupt, and no extra bus traffic beyond the drain.

## The record

Same CSV head as a normal record so the server needs no second parser:

| Field | Track mode value |
|---|---|
| timestamp | modem clock |
| lat, lon | last fix before GNSS was stopped (`0.000000` if there never was one) |
| speed | the ECU's, km/h; 0 if the ECU is not answering |
| altitude, heading | last fix |
| hdop, sats | 0 |
| battery, ignition, uptime, pon | as normal |

Extras, in addition to the usual `ax/ay/az`, `gx/gy/gz`, `it`, cell fields
and `up`:

| Key | Value |
|---|---|
| `tm=1` | marks the row; stored as `log.track_mode` |
| `o*` | the OBD fields from the fast poll (`orpm`, `ospd`, `oth`, `old` every record, one slow one per record) |
| `acc=` | the IMU burst: `ax/ay/az/gx/gy/gz` per sample, samples joined by `:`, oldest first, accel in milli-g, gyro in bias-corrected LSB at ±250 dps; stored verbatim as `log.imu_burst` |

Not sent, versus a normal record: the modem temperature and VSYS reads (AT
commands costing tens of milliseconds for values that change over minutes),
the RPM min/max/avg accumulator (at this cadence the instantaneous figure is
the resolution), and any GNSS wait.

A record is around 600-700 bytes.  At two a second that is about 1.3 kB/s,
comfortably inside what LTE-M sustains and under the 1200-byte datagram cap
with room for the debug-log lines that ride along.

## Server

- `sql/2026-09-06_track_mode.sql` adds `device.track_mode`, `log.track_mode`
  and `log.imu_burst`.  Apply once.
- `parse_csv_line()` reads `tm=` and `acc=`; `process_record()` stores them
  (the burst is validated against its shape and capped at 4000 bytes).
- `_process_telemetry()` appends `track=<0|1>` to every response.
- `/api/1.0/trackmode` GET/POST, login-protected, reads and sets the switch.
- The carpos endpoint and the `/ws/carpos` stream carry `track_mode` (the
  switch), `track` (whether the row was built in the mode) and `imu` (the
  burst unpacked, gyro in deg/s).  The stream sends every new row rather
  than only the newest, polls every 250 ms while the switch is on, and puts
  the switch on its keep-alive pings so a toggled page changes view even
  with a silent device.

## The page

While the switch is on the map is hidden and the dashboard shown: RPM with
a redline bar, speed, throttle and load bars, a friction circle with a 3 s
trail, a 60 s strip chart of RPM / throttle / peak g, and tiles for the
slow values, yaw rate, battery, IMU temperature, update rate and age of the
newest record.  It shows "waiting" until the first `track=1` row arrives.

The device's orientation in the car is unknown, so the dashboard learns the
forward axis: when the speed between two records changes by a couple of mph
and the burst shows a clear horizontal push, that push lies along the car's
axis, backwards when slowing.  Until it has seen one such event the friction
circle draws the horizontal vector in the sensor's own frame and says so;
afterwards forward is up and longitudinal/lateral g are shown.  The axis is
kept in the browser's localStorage, since the mounting does not change.

## Configuration

| Symbol | Default | Meaning |
|---|---|---|
| `APP_TRACK_MODE` | y | build the mode at all |
| `APP_TRACK_PERIOD_MS` | 500 | lower bound on the record period |
| `APP_TRACK_IMU_SAMPLES` | 16 | samples per record; 0 sends only the instantaneous reading |
| `APP_TRACK_RESP_INTERVAL_S` | 10 | how often a reply is waited for in the mode |
| `APP_RESP_POLL_S` | 30 | how often a reply is waited for while driving normally; 0 restores the old never |
