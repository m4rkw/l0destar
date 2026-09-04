# K-wire (ISO 14230-1) Interface

**Before building, installing or relying on any of this, read the [project disclaimer](../DISCLAIMER.md).**

The K-wire is the single-wire bidirectional diagnostic line on OBD pin 7,
with the optional L wire on pin 15.  Its physical layer is ISO 14230-1 and
the protocol the tracker speaks over it is KWP2000 (ISO 14230).  Earlier
documentation called this interface "ISO-9141"; that was inaccurate — ISO
9141-2 is a different, older protocol that happens to share the wire and the
5-baud wake-up — and nothing here uses it.  Code and Kconfig symbols keep
the `kline` / `KLINE` names, which are the common shorthand for the same wire.

For a short recipe rather than the full reference, see
[KWIRE_QUICKSTART.md](KWIRE_QUICKSTART.md), which walks through one discovery
run on a real vehicle.

This document covers two things:

1. the **bench loopback test** (`CONFIG_APP_KLINE_TEST`), which verifies the
   transceiver and pins with no vehicle attached;
2. the **vehicle session init** (`CONFIG_APP_KLINE_DISCOVER`), which opens
   a KWP2000 session with a real ECU at boot and reports how, and the
   configuration that does so on the reference vehicle.

## Hardware

| Board | Transceiver | Rails |
|-------|-------------|-------|
| v2.5K / v2.6K | L9637D + TXS0104E level shifter | AUX (shifter A side) + K_EN (L9637D 5 V / 12 V) |
| v3.0 | TJA1027T LIN transceiver | shared OBD domain, SLP_N (K_SLEEP) raised to wake |
| v3.1+ | TJA1027T | own K_EN domain: PP3V3_K + PP12V_K |

On v3.x the K wire has a 510 Ω pull-up to PP12V_K through a diode (S10D1 +
S10R3) and a 33 V TVS; the transceiver's bus pin sits directly on the wire.
L has the same pull-up and a 2N7002 pulldown gated by L_SEND.  Pins are
assigned in `Kconfig.boards` (`APP_PIN_K1_TX`, `APP_PIN_K1_RX`, `APP_PIN_K_SLEEP`,
`APP_PIN_L_SEND`).

The TJA1027T has no TXD dominant time-out, only an initial-TXD-low check on
leaving sleep, so it will hold the bus down for the 200 ms bits of a 5-baud
address (checked against the datasheet — a LIN transceiver with a time-out
could not do this).

## How the firmware drives the wire

- The **5-baud address** and the KWP2000 **fast-init wake-up pulse** are
  bit-banged on the K_TX GPIO, with the echo on K_RX checked every bit.
- Every **data byte** at 9600 / 10400 bps goes through **UARTE1**, driven
  through the nrf HAL (`src/hw_kline.c`, `kline_uart_open()`).  Zephyr's UART
  driver only accepts its fixed list of rates and 10400 is not on it, so the
  BAUDRATE register is written directly.  UARTE1 is otherwise unused and is
  non-secure in the TF-M build.  The pins are handed to the UARTE after the
  address has gone out and returned to GPIO afterwards.
- A bit-banged 10.4 k receiver was tried first and is **not good enough**
  against a real ECU: the busy-wait loop lands a few percent off the rate,
  which is exactly enough to misread the last bit of a key byte.  It is kept
  (calibrated at boot) only for the bench loopback and as a fallback.

## Part 1 — bench loopback test

Verifies the K and L circuits using the transceiver's own bus echo (TX
drives the wire, the same chip's RX reads it back).  On bench boards with
two transceivers it streams bytes K1->K2 and K2->K1 across the wire instead.
No external equipment needed beyond a 12 V supply for the K rail.

1. Enable in `local.conf`:
   ```
   CONFIG_APP_KLINE_TEST=y
   ```
2. Build and flash:
   ```
   ./build.sh && ./flash.sh
   ```
3. Reset the board.  Serial output:
   ```
   *** K-WIRE TEST ***
   K1 static: idle=1 TX=0->RX=0 TX=1->RX=1 OK
   K1 loopback: TX=P0.1 RX=P0.3
   K1 loop: 256/256 bytes OK
   L-line: SKIP (L_SEND disabled — the pulldown FET on this board dies into a short to battery)
   PASS: K-wire test
   *** K-WIRE TEST DONE ***
   K-line test complete - halting.
   ```
4. Comment out `CONFIG_APP_KLINE_TEST=y` when done.

What it proves: K_EN rails, the transceiver, and the TX/RX pins.  What it
cannot prove: anything beyond the transceiver's bus pin.  That pin idles high
on the chip's internal pull-up whether or not the wire goes anywhere, so a
passing loopback with an open, swapped or unconnected harness looks identical
to a healthy one.  Two days of the reference-vehicle bring-up went to K and L
being swapped in the harness; the loopback was clean throughout.

## Part 2 — discovery and the runtime session

These are two separate operations and the split matters.

**Discovery** is a one-shot investigation of a vehicle nobody has profiled
yet.  It hunts for the protocol, the data rate and the ECU addresses, asks
every responder what it supports, and takes minutes while filling the console.
You run it once per vehicle.  It ends with a summary and the `local.conf`
block that configures the runtime path, so nobody has to read the log twice.

**The runtime session** is what polling uses.  It does no probing at all: it
opens at the address and rate discovery settled on, exchanges requests, and
closes.  `kline_session_open()`, `kline_obd_pid()` and `kline_session_close()`
are the whole API, and none of the discovery machinery is reachable from them.

### Discovery

`CONFIG_APP_KLINE_DISCOVER=y` makes the tracker try to open a KWP2000
session once at boot, report the result on the console and as an alert once
the modem is up (priority 1 if a session opened, 0 otherwise), and then start
as normal — including the power-on FOTA check, so a bench build with a lower
version than the fleet's swaps itself back to the production image straight
after reporting.  Nothing is sent over K beyond the init itself; the session
is left to time out on the ECU (P3max, 5 s) when the rails drop.

Stages, each only if the previous got no reply, each gated by Kconfig:

| Stage | Option | What it sends |
|-------|--------|---------------|
| 5-baud init on 0x33 | always | address 0x33 at 5 baud on K (+L if `APP_L_SEND_ENABLED`); expects 0x55, two key bytes, sends ~KB2, expects ~0x33 |
| fast init on 0x33 | `APP_KLINE_INIT_FAST` (default y) | 25 ms low / 25 ms high, then StartCommunication `C1 33 F1 81 66`, once with 5 ms between bytes and once back to back |
| known addresses | `APP_KLINE_INIT_ADDRS` (e.g. `"13,29,58,B4"`) | the 5-baud handshake on each; if one breaks down partway, a raw hex capture of what that address sends, with inter-byte gaps (`APP_KLINE_INIT_ACK` controls whether ~KB2 is sent during capture) |
| sweeps | `APP_KLINE_INIT_SWEEP` (default y) | fast init, then 5-baud init, on every address 0x01-0xFE — up to 15 min |
| identify | `APP_KLINE_IDENT` (default n) | for each address that handshakes: StartDiagnosticSession in five modes, ReadEcuIdentification (0x1A) in seven variants, OBD mode 01 supported-PIDs / MIL+DTC count / RPM / coolant, mode 09 VIN, then StopCommunication |
| fault codes | `APP_KLINE_DTC` (default n, needs `APP_KLINE_IDENT`) | OBD mode 03 stored, 07 pending, 0A permanent; multi-frame responses read until the bus goes quiet, each code decoded to its P/C/B/U form |

`APP_KLINE_IDENT` and `APP_KLINE_DTC` are the only options that send anything
beyond the init.  Every request they make is read-only.  **Mode 04, which
erases stored codes and the emissions readiness data with them, is not
implemented anywhere in this firmware and must not be added.**

`APP_KLINE_BAUD` (default 10400) is the rate the ECU's reply is read at and
the tester's bytes sent at.  If the ECU answers but its sync byte does not
decode cleanly, the slow init sends the address again and reads at the other
of 9600 / 10400, so a wrong setting costs one extra attempt.

`APP_KLINE_INIT_DIAG` adds two operator checks before the init for the things
the echo cannot prove: L is held low for 5 s so pin 15 can be metered (expect
~0 V), then K is watched for 20 s while the operator grounds pin 7 to pin 4 by
hand — any edge counted proves a low on the wire reaches the receiver.

The run ends like this:

```
=== K-WIRE DISCOVERY SUMMARY ===
  protocol       ISO 14230-4 KWP2000, 5-baud init
  data rate      9600 baud
  L wire         not needed
  addresses      13
  engine ECU     0x13
  mode 01 PIDs   01 03 04 05 06 07 0C 0D 0E 0F 10 11 13 14 15
  engine RPM     yes (PID 0C)
  vehicle speed  yes (PID 0D)
  more PIDs      no, nothing above 0x15
  fault codes    mode 03 silent, mode 07 yes, mode 0A silent
  identification none (no 0x1A, no mode 09 VIN)
  diag session   not needed / not accepted

  Suggested local.conf:
    CONFIG_APP_KLINE_BAUD=9600
    CONFIG_APP_KLINE_ECU_ADDR=0x13
    CONFIG_APP_KLINE_INIT_ADDRS="13"
    CONFIG_APP_KLINE_INIT_FAST=n
    CONFIG_APP_KLINE_INIT_SWEEP=n
    CONFIG_APP_KLINE_DISCOVER=n
=== END ===
```

Addresses that complete the handshake but never answer a request are listed
as `inert`, because they are exactly what the runtime config should leave out.

Discovery parks the board when it finishes.  The console log is the whole
product of the run, and continuing into the tracker would let the power-on
FOTA check swap the image out mid-investigation.

### The runtime session

`APP_KLINE_ECU_ADDR` and `APP_KLINE_BAUD` are all the runtime path reads, and
both are defined unconditionally because `hw_kline.c` is compiled for every
board.  The API is three calls:

```c
int  kline_session_open(void);              /* rails up, 5-baud init, UART ready */
int  kline_obd_pid(uint8_t pid, uint8_t *buf, int max);   /* one mode 01 request */
void kline_session_close(void);             /* StopCommunication, rails down */
```

`kline_obd_pid()` returns the PID's value bytes with the `41 xx` echo already
stripped, or a negative errno.  Any request resets the 5 s P3 timer, so
polling at 1 Hz keeps the session alive with no separate TesterPresent.

### Part 3 — OBD-II telemetry

`CONFIG_APP_KLINE_TELEMETRY=y` polls the ECU each time a telemetry record is
built and appends the values to that record, so they belong to the same
instant as the position they ride with.  Only PIDs the ECU advertises in its
mode 01 PID 00 bitmap are asked for; the bitmap is read once per session.

The session is opened once and held for the drive.  A 5-baud init holds the
bus dominant for 2.4 s, so re-initialising every cycle would be far more
disruptive than the polling itself.

Keeping it open takes deliberate work, because P3max is 5 s and a tracker
cycle does not naturally stay inside that.  The poll happens *after* the fix,
so the gap that matters is the send and the idle that follow it, and a slow
send alone can exceed 5 s.  Two things bridge it:

- the **RPM sampler** covers the GPS fix wait, which is most of a cycle; every
  sample is a request and so resets P3;
- **`obd_keepalive()`**, called from every loop that can run for a while —
  the main one and the ignition-sleep one — covers the rest.  It is a no-op unless the line has been idle for 3 s, and when it does
  fire it reads mode 01 PID 01 — the cheapest request the ECU is guaranteed to
  answer, and the one that carries the stored-code count, so the keep-alive
  costs nothing that was not already wanted.

If the session drops anyway, the next request times out and it is reopened
once and retried.  That path is **rate limited**: more than two reopens in a
minute and the firmware stands off for a minute, reporting no OBD data for
those records rather than hammering the bus with 2.4 s initialisations.  The
limiter is cleared only by a poll that never had to reopen — clearing it on
any successful poll would defeat it in exactly the case it exists for, a
session that dies every cycle, reopens and completes.  The session is closed
at ignition-off, before the K rails drop.

Values travel as integers with a fixed scale, so the packet never carries a
decimal point and the firmware needs no float formatting.  The server
unscales them (`OBD_FIELDS` in `main.py`) into the `obd_*` columns on `log`.

Speed is the one unit conversion.  The ECU reports km/h and the firmware
sends km/h, matching what it already does for GNSS speed; the server applies
the same `0.6213712` in the same function for both, so `speed` and
`obd_speed` are both mph and directly comparable.  Firmware-internal use
stays in km/h, because that is what the coast-to-stop threshold is expressed
in.

| Key     | PID  | Column            | Scale | Unit  |
|---------|------|-------------------|-------|-------|
| `orpm`  | 0x0C | `obd_rpm`         | 1     | rpm   |
| `ormin` | 0x0C | `obd_rpm_min`     | 1     | rpm   |
| `ormax` | 0x0C | `obd_rpm_max`     | 1     | rpm   |
| `oravg` | 0x0C | `obd_rpm_avg`     | 1     | rpm   |
| `ospd`  | 0x0D | `obd_speed`       | 1     | km/h on the wire, stored as mph |
| `ocl`   | 0x05 | `obd_coolant`     | 1     | deg C |
| `oit`   | 0x0F | `obd_intake`      | 1     | deg C |
| `old`   | 0x04 | `obd_load`        | 10    | %     |
| `oth`   | 0x11 | `obd_throttle`    | 10    | %     |
| `omaf`  | 0x10 | `obd_maf`         | 100   | g/s   |
| `otim`  | 0x0E | `obd_timing`      | 10    | deg   |
| `ostft` | 0x06 | `obd_stft`        | 10    | %     |
| `oltft` | 0x07 | `obd_ltft`        | 10    | %     |
| `ofs`   | 0x03 | `obd_fuel_status` | 1     | raw   |
| `omil`  | 0x01 | `obd_mil`         | 1     | 0/1   |
| `odtc`  | 0x01 | `obd_dtc_count`   | 1     | count |

Every field is independently optional.  A record from a vehicle with no K
interface, or built with the ignition off, carries none of them and the
columns stay null; nothing is carried forward from the previous row.

**Resolution.** Most of these move slowly and one reading per record is
plenty: coolant, intake, load, throttle, trims and the lamp.  Engine RPM does
not, and a single sample every few seconds says nothing about how the car was
driven, so RPM is also sampled on its own about once a second and reported as
`ormin` / `ormax` / `oravg` alongside the instantaneous `orpm`.

The sampling runs from a callback on the GNSS fix wait (`gnss_set_tick`),
which is where most of a cycle is spent and where the thread would otherwise
be asleep on a semaphore.  Because it runs on that same thread there is no
locking anywhere: nothing else touches the K wire while the wait is in
progress.  The sampler never opens or reopens a session — a 5-baud init takes
2.4 s with the bus dominant, which has no business happening inside a GPS
wait — so if there is no session it simply skips and the next poll sorts it
out.  A successful sample also resets the ECU's P3 timer, so sampling keeps
the session alive through a long fix.

Vehicle speed is deliberately not sampled this way: most ECUs update PID 0x0D
only about once a second internally, so faster polling buys nothing.

### The ECU's figures drive the tracker's own logic

Two of these values are better than the proxies the tracker used before, so
they are preferred when the ECU is answering and fall back automatically when
it is not (`obd_rpm()` and `obd_speed_kmh()`, both negative when there is no
fresh reading).

**Vehicle speed (PID 0x0D) decides whether we are moving.**  It comes from
the wheel speed sensors and reads exactly zero at a standstill.  GNSS speed is
Doppler-derived and does not: across 106 stationary records on the reference
vehicle, with the ECU reporting 0 km/h throughout, GNSS averaged 0.66 mph and
peaked at 5.11 mph.  The ECU figure is not better in every respect — vehicle
speed sensors typically over-read by a couple of percent and are affected by
tyre size — but for "moving or not" the exact zero is what matters.

**PID 0x0D is km/h**, on any vehicle.  SAE J1979 defines it as a single
unscaled byte of km/h regardless of market or of what the dashboard displays,
so a JDM import reports the same units as anything else.  The server converts
it to mph on the way in, so both speed columns read the same way — worth
confirming once there is moving data by checking that `obd_speed` and `speed`
agree to within a few percent in the same row.

**Engine RPM (PID 0x0C) decides whether the engine is running.**  The tracker
previously inferred this from charging voltage crossing 13 V, which is only a
proxy and a poor one: a tired battery or a low alternator output reads as
"engine off" and drops the tracker to its 30 s engine-off cadence while the
car is being driven.  RPM above zero is a direct measurement.

Because both feed decisions rather than the record, they are refreshed by
`obd_keepalive()` every 3 s rather than only when a record is built — in the
engine-off state records are 30 s apart, which is far too stale to notice the
engine starting.  That costs three exchanges, about 250 ms, every 3 s.

### Part 4 — fault codes

`CONFIG_APP_KLINE_DTC_REPORT=y` reads the stored codes (OBD-II mode 03) at
two moments, and neither of them is a timer.

**At ignition-on**, after `CONFIG_APP_KLINE_DTC_ON_DELAY_MS` (default 5000)
to let the ECU boot.  This is what the ECU has persisted from earlier drives.
It runs *after* the ignition-on position has been sent, not before: the read
costs that boot delay plus a session open, and the position is the
time-sensitive part.  If it fails — ECU still booting, session refused — the
firmware keeps trying on later cycles until a report lands, because the count
watch below only fires on a *change* and would never report codes that were
already there.

**When the stored-code count changes.**  Mode 01 PID 01 carries the
malfunction lamp and the stored-code count in one byte, and every telemetry
poll already reads it, so a code appearing or clearing mid-drive is visible
immediately at no extra cost on the bus.  A change is what triggers the mode
03 read.  That is strictly better than the periodic re-read most fleet
systems use: same latency as polling every few seconds, none of the traffic.
With `APP_KLINE_TELEMETRY` off, the main loop reads PID 01 on its own each
cycle to keep the watch armed.

There is deliberately **no read at ignition-off**.  Sleep is only ever entered
with the ignition off, and the engine ECU is unpowered then, so the read would
time out — and since a failed read must send nothing, it would achieve
nothing even when it appeared to work.  Codes raised during a drive are caught
by the count watch while the engine is still running.

The device sends its **complete current set** as its own line:

```
D,P0133,P0420          two codes stored
D,                     none stored
```

It goes as a standalone datagram rather than folded into a telemetry record,
because the server treats it as authoritative and must not infer an empty set
from a record that merely lacks the field.  If the read fails, nothing is
sent at all: an empty report would wrongly clear codes that are still stored.

That distinction is load-bearing and easy to get wrong in both directions.

A silent ECU and an ECU with nothing stored look identical at the wire, so
`kline_obd_dtcs()` treats a timeout or a negative response as a failure and
reports zero only when the ECU actually answered mode 03.  For the same
reason the session is dropped as soon as the ignition goes off rather than
left for the next key-on to find: a stale session is accepted, times out on
every request, and would otherwise read as "no faults".

But that alone breaks reporting on ECUs which never answer mode 03 when the
answer would be "none" — the reference Toyota is one — and the retry then
runs forever.  The arbiter is mode 01 PID 01, which carries the stored-code
count independently.  A silent mode 03 is read as an empty set **only** when
PID 01 has said the count is zero; with a non-zero count, silence is a real
failure and nothing is sent.  Failed reads back off for 30 s, because the
trigger is checked every time round the main loop.

The server reconciles that set against the `dtc` table, which carries
`device_id`, `code`, `raised_at`, `cleared_at` and `active`.  A code in the
report that is not already active is inserted with `active = 1`.  A code
active in the table but absent from the report is marked `active = 0` with
`cleared_at` set.  Rows are never deleted, so the table is a history of when
each fault appeared and disappeared, and a fault that recurs gets a fresh
row.  Raised codes send a priority 1 Pushover alert, cleared ones priority 0.
Repeat reports of an unchanged set do nothing.

Malformed codes are dropped and logged rather than stored; the server
validates against `[PCBU][0-3][0-9A-F]{3}`.

**Ignition off during a session.**  Nothing on the K wire works with the
ignition off, so every entry point checks it first.  A poll already in flight
abandons its remaining PIDs the moment a request times out with the ignition
gone, rather than spending a timeout on each — a dozen PIDs at the discovery
timeout would stall the main loop for twelve seconds.  Runtime requests use a
250 ms timeout for the same reason, against the 1 s discovery uses.  The
session is then closed without the StopCommunication courtesy, since the ECU
is no longer there to answer it, and the K rails drop.

Nothing here writes to the vehicle.  Mode 04, which erases stored codes and
the emissions readiness data with them, is not implemented anywhere in this
firmware and must not be added.

### Reference vehicle: 2006 Toyota Harrier 2.4 L (ACU30, JDM)

OBD socket populated on pins 4, 7, 9, 12, 13, 15, 16 — no CAN.  `local.conf`:

```
CONFIG_APP_BOARD_L0DESTAR_V3_1=y
CONFIG_APP_BOARD_HAS_CAN=n
CONFIG_APP_BOARD_HAS_KLINE=y

CONFIG_APP_KLINE_DISCOVER=y
CONFIG_APP_KLINE_BAUD=9600
CONFIG_APP_KLINE_INIT_ADDRS="13"
CONFIG_APP_KLINE_INIT_FAST=n
CONFIG_APP_KLINE_INIT_SWEEP=n
CONFIG_APP_KLINE_INIT_DIAG=n
# L not needed on this vehicle; leave APP_L_SEND_ENABLED at its default (off).

# Runtime: poll OBD each cycle, read fault codes at ignition transitions.
CONFIG_APP_KLINE_ECU_ADDR=0x13
CONFIG_APP_KLINE_TELEMETRY=y
CONFIG_APP_KLINE_DTC_REPORT=y
```

Only 0x13 is listed because it is the only address that answers a request.
Use `"13,29,58,B4"` to reproduce the handshake on all four; every request to
the other three times out, which costs about 20 s each.

What the vehicle does:

- Four ECU addresses answer the 5-baud init — **0x13, 0x29, 0x58, 0xB4** — with
  the ISO 14230-4 key bytes **E9 8F**, each with its own W1 (about 90, 120,
  145 and 95 ms), so they are most likely four separate ECUs.  Which is the
  engine ECU is not yet established.
- Replies are at **9600 baud**, not the standard 10400.
- The OBD functional address **0x33 gets nothing**, and the fast init gets
  nothing on any address.  All four responding addresses have odd parity;
  0x33 does not.
- **K alone is sufficient.**  The L wire is not needed for the init.

Serial output with that configuration:

```
*** K-LINE INIT ***
slow init: address 0x33 on K only, reply at 9600 baud
  no sync byte within 400 ms
slow init: address 0x13 on K only, reply at 9600 baud
  0x13: sync 0x55 after 89 ms at 9600 baud
  key bytes: KB1=0xE9 KB2=0x8F — ISO 14230-4 KWP2000
  ack: 0xEC (expect 0xEC)
...
addresses: 4 of 4 replied, 4 handshakes completed
PASS: K-line communication established via 5-baud init, ECU 0x13 (ISO 14230-4 KWP2000)
*** K-LINE INIT DONE ***
```

The alert that follows reads
`K-line: session via 5-baud init at 9600 baud, ECU 0x13 KB E9 8F (ISO 14230-4 KWP2000) +3 more: 29 58 B4`.

### Identifying which ECU is which

`APP_KLINE_IDENT=y` asks each address that handshakes to describe itself.  The
engine ECU is the one that answers OBD mode 01, since no other module does.
On the reference vehicle:

- **0x13 is the engine ECU.**  It answered the supported-PID bitmap
  `BE 1F B8 00`, an RPM of 1250 with the engine running, and a coolant
  temperature of 28 C.
- That bitmap decodes to mode 01 PIDs 01, 03, 04, 05, 06, 07, 0C, 0D, 0E, 0F,
  10, 11, 13, 14 and 15, and the PID 20 continuation flag is clear, so nothing
  above 0x15 exists.  Vehicle speed (0D) and RPM (0C) are both present, which
  is what a tracker needs; there is no fuel level, oil temperature or
  distance-since-clear.
- It ignores StartDiagnosticSession in all of modes 81, 85, 86, 89 and C0,
  every ReadEcuIdentification variant, and the mode 09 VIN request.  So it
  exposes the OBD-II service set and nothing else, and **there is no VIN
  available over K on this vehicle**.
- **Fault codes work.**  Mode 07 answered `47 00 00 00 00 00 00`, no pending
  codes, and mode 01 PID 01 reported the lamp off with no stored codes.  Mode
  03 stayed silent, which on this ECU means an empty list rather than an
  unsupported service, since PID 01 agrees there are none; that reading is
  worth re-checking once the car actually has a fault stored.  Mode 0A is not
  supported, as expected for 2006.
- **0x29, 0x58 and 0xB4 complete the handshake and then answer nothing at
  all**, not even StopCommunication.  They are genuinely address-filtered, as
  the full sweep found only these four of 254, so they are most likely real
  ABS, airbag or body ECUs speaking a Toyota-private request format.

Do not blind-sweep service identifiers at those three to find out.  Unknown
service identifiers on ABS and airbag modules can start actuator tests or
alter stored fault codes.  Read-only candidates only.

### Polling while driving

Read-only polling is safe and is what commercial OBD dongles do continuously.
One request and response at 9600 baud is about 13 ms of wire time, and the
mandated gaps stretch a full exchange to roughly 100 ms, so a 1 Hz poll of
speed and RPM is a small fraction of a wire nothing else uses while driving.
Any request resets the 5 s P3 timer, so polling at 1 Hz keeps the session
alive without a separate TesterPresent.

The risks worth engineering against are not the polling itself:

- **A stuck-dominant line after a firmware fault.**  The TJA1027T has no TXD
  dominant time-out, so a hang with TX low holds the wire low indefinitely and
  no scan tool can talk to the car.  The 32 s task watchdog bounds it, and on
  reset the pins park as inputs and the K rail drops, releasing the line.
  Prefer powering the K domain only for each poll burst: then every failure
  path, watchdog reset or brown-out included, releases the wire with no
  firmware involvement.
- **Re-initialisation storms.**  Each 5-baud address holds the line dominant
  for 200 ms per bit, about 2.4 s per attempt.  A session that keeps dropping
  and re-initialising in a tight loop is the worst thing this firmware can do
  to the bus; it needs a failure counter and a backoff of minutes.
- **Collision with a garage scan tool.**  Two testers on one wire corrupt each
  other.  The bus-idle check before initialising helps; also abandon the
  session and stay quiet for a while on traffic the tracker did not cause.
- **Keeping modules awake when parked.**  A live session can stop ECUs
  sleeping and drain the battery.  Only poll with the ignition on; the sleep
  path already powers the K domain off.

Never run the address sweeps on a moving vehicle: they send 254
initialisations and monopolise the bus for a quarter of an hour.

### Bringing up a new vehicle

1. Start with the defaults: `APP_KLINE_DISCOVER=y` and nothing else set.
   That tries 0x33 slow and fast, then sweeps every address at 10400 with a
   9600 retry.  Up to 15 minutes; ignition on.
2. Any address that replies is printed as it happens.  Put the responders in
   `APP_KLINE_INIT_ADDRS`, set `APP_KLINE_BAUD` to whatever the sync line
   reported, turn the sweeps and (if unanswered) the fast init off.  A run
   then takes about 20 s.
3. Only enable `APP_L_SEND_ENABLED` if K alone gets no reply *and* the L wire
   has been checked for a short to battery — see "The L line" below.

## Troubleshooting

**No reply on any address, echo clean** — the echo cannot see past the
transceiver.  Check the harness before touching the protocol: continuity from
the board's K terminal to socket pin 7 (and L to 15), and that they are not
swapped.  With the tracker booted and idle, pin 7 to pin 4 should read close
to battery voltage.  `APP_KLINE_INIT_DIAG` lets you ground pin 7 by hand and
see whether the receiver notices.

**Sync byte reads 0xB5 / 0xA5 / 0xC9 with a framing error, key bytes E9 0F or
C9 0F** — wrong data rate.  Those are exactly what a 9600-baud reply looks
like read at 10400.  Set `APP_KLINE_BAUD=9600`.

**Sync 0x55 and key bytes fine, no acknowledge** — the tester's ~KB2 was not
accepted.  Either it was built from a misread KB2 (check for the symptoms
above) or the bit-banged fallback is in use instead of the UARTE.

**"echo: bus stayed high for N ms of the low bits"** — the transceiver is not
driving K.  K_EN rails, SLP_N high, and on v3.x remember the TJA1027T's
initial-TXD-low check: TXD must be high when SLP_N rises, which
`kline_power_on()` arranges.

**"bus busy before init"** — something else is transmitting: a previous
address's ECU still talking, or another tester on the wire.

**Loopback K1 static FAIL (TX=0 -> RX=1)** — RX doesn't follow TX through the
transceiver: K domain not up ("K domain on" in the log), correct board
variant selected, transceiver populated and powered.

**Loopback byte mismatches** — bit-timing drift in the bit-banged path;
check that no high-priority interrupt is starving the loop.

## Protocol notes (ISO 14230-2, 5-baud init)

| Window | Meaning | Spec | Used here |
|--------|---------|------|-----------|
| W5 | bus idle before the address | ≥ 300 ms | 300 ms, verified high |
| — | address byte | 5 baud, 200 ms/bit, 8N1 | bit-banged, echo checked |
| W1 | end of address -> sync byte | 60-300 ms | wait up to 400 ms |
| W2 | sync -> KB1 | 5-20 ms | 50 ms timeout |
| W3 | KB1 -> KB2 | 0-20 ms | 50 ms timeout |
| W4 | KB2 -> ~KB2, and ~KB2 -> ~address | 25-50 ms | 30 ms, 60 ms timeout |
| P3max | idle before the ECU drops the session | 5 s | session left to expire |

Key bytes: KB2 = 0x8F identifies ISO 14230-4; KB1 (0xE9, 0x6B, 0x6D, 0xEF)
lists the header formats and timing the ECU supports.  08 08 or 94 94 would
mean an ISO 9141-2 ECU, which this firmware recognises but does not talk to.

Fast init (ISO 14230-2): Tinil 25 ms low, Twup 50 ms from the falling edge to
the first request bit, then StartCommunication with P4 5-20 ms between bytes
and the response within P2 25-50 ms.

## The L line

Driving the L line is gated by `CONFIG_APP_L_SEND_ENABLED`, and it is **off
by default on every board before v3.3**. Those boards switch the L pulldown
FET (2N7002) straight across the wire: an L wire shorted to battery looks
exactly like a healthy idle one (both sit at 12-16 V), and asserting L_SEND
then saturates the FET into the short, where it dissipates 1-12 W in a SOT-23
and fails inside the first address bit of a 5-baud init - sometimes gate-first,
putting battery voltage on the L_SEND GPIO and taking the nRF9151 with it.
`../hardware/l0destar_v3.2/README.md` has the full write-up. With the gate off
the pin is still parked low (FET off) and `kline_l_send()` returns `-EPERM`, so
nothing in firmware can assert it; the cost is the L half of a 5-baud init.

v3.3 fixed the hardware - an AL5809-90 in series limits the pulldown to 90 mA
with thermal shutdown - and added **L_SENSE**, a tap on the wire through a
1N4148 (cathode to L) and a 47K. The diode blocks the vehicle's 12 V from ever
reaching the pin, so the line can only be read by sourcing current into it:
firmware samples it on the SAADC with the internal pull-up resistor ladder
engaged (~400K to VDD).

| L wire | Reading |
|--------|---------|
| pulled low (our FET, or an ECU) | diode conducts, ~0.7-0.9 V |
| high, open, or shorted to battery | diode reverse biased, runs to the 3.6 V full scale |

`CONFIG_APP_L_SENSE_LOW_MV` (default 1500) splits the two. A plain GPIO input
cannot: the nRF's ~13K internal pull-up against the 47K leaves even a grounded
line at ~2.7 V, above VIH. Zephyr's ADC driver hard-codes the ladder to bypass,
so `src/hw_kline.c` drives the SAADC through nrfx.

`kline_l_line_probe()` pulses the pulldown for 5 ms and reports whether the
line actually went low - a line that stays high is being held up by something
low-impedance, i.e. shorted to battery, and the init must not be attempted.

**Pin requirement:** the SAADC can only sample AIN0-AIN7, which are P0.13 to
P0.20 on the nRF9151. v3.3 puts L_SENSE on P0.14 (AIN1) and moves the
PP3V3_GPS rail sense, which only ever needed a digital read, to P0.05.
`kline_l_sense_init()` rejects any `CONFIG_APP_PIN_L_SENSE` outside that
range ("L_SENSE on P0.x has no SAADC channel") and the sense reports
unavailable.

## Signal path (v2.6K)

```
nRF P0.0 (K1_TX) --> TXS0104E A2/B2 --> L9637D TxD --> K-line
                                                          |
nRF P0.1 (K1_RX) <-- TXS0104E A1/B1 <-- L9637D RxD <----+
```

Both AUX (TXS0104E VCCA) and K (L9637D Vs, TXS0104E VCCB) domains must
be powered for the signal path to work.
