# Changelog

## Unreleased

### A-GNSS no longer competes with the GNSS search it is meant to help
- **Assistance is fetched before GNSS starts**, while the radio is entirely
LTE's.  It used to run from inside `gnss_collect()` during a cold search, and
GNSS and LTE share one RF front-end on the nRF91: a cold search wants long
uninterrupted windows, leaving a TLS handshake plus a multi-kilobyte download
almost no airtime.  That is the `-116` (`ETIMEDOUT`) with `HTTP 0` — the
request never reached the server at all, so it was never a credentials or API
problem.  The boot-time call asks for full assistance rather than a targeted
request, since the receiver has not started and so has not asked for anything
specific yet.
- **The remaining in-search path yields the radio.**  `agnss_fetch()` now
pauses GNSS around the download and resumes it afterwards, using
`gnss_resume()` rather than `gnss_start()` so a warm receiver is not turned
cold.  `gnss_stop()` only reports success when the receiver was actually
running, which is how the boot-time call knows to leave it alone.
- **A failed fetch is retried.**  `gnss_collect()` cleared the pending request
*before* attempting the fetch, so one timeout cost the assistance for the
whole cold start with no retry until the receiver independently asked again.
The request is now only cleared once the fetch succeeds.

### Accelerometer alert priority backoff (`APP_ACCEL_ALERT_BACKOFF_S`)
- **One physical event no longer produces a string of urgent alerts.** Impact,
tilt/tow and movement all raise the same high-priority alert and routinely
trip together — opening a glovebox the tracker lives in gives a movement
alert, then a tilt alert, then another movement alert. The first now goes out
at `APP_ACCEL_ALERT_PRIORITY` and opens a window; anything inside it is sent
at `APP_ACCEL_ALERT_BACKOFF_PRIORITY` (default 0, normal) instead. Nothing is
suppressed — every alert is still delivered and logged, just not urgent.
- **The window is fixed, not sliding** (default 300 s), so once it expires the
next event is urgent again. Sustained interference still raises one
high-priority alert per window rather than silencing itself by persisting.
- Applies to all six accelerometer-derived alert sites: awake and parked
impact, tilt/tow, tamper orientation change, and movement.

### Unsendable telemetry is held instead of discarded (`src/databuf.c`)
- **A record whose send fails is now buffered rather than thrown away.**
`data_reset()` runs unconditionally after a send attempt, so an outage used to
cost the position data for its whole duration — a twelve-minute coverage drop
on 2026-09-05 left a hundred-mile drive with no track at all for those twelve
minutes, even though the device was up throughout and the data existed.
- **Statically allocated, so the risk is a link-time one.**
`APP_DATABUF_SLOTS` (32) x `APP_DATABUF_REC_MAX` (512) is ~16 KB of BSS; a
size that does not fit fails the build rather than the device, and there is no
allocation to fail in a tunnel. Application RAM went from 66,416 to 84,024
bytes of a 154,264 byte region, leaving ~70 KB free.
- **A full buffer is thinned by half, not truncated.** Dropping the newest
loses the recovery and dropping the oldest loses the start of the outage, so
instead every second record is discarded and the sample interval doubles. 32
slots then cover 2.7 minutes at 5 s, or 21 minutes at 40 s after three
thinnings — a complete track at coarser resolution rather than a truncated
one. That 12-minute gap would have come back at about 20 s spacing.
- **Draining never delays live telemetry**: `APP_DATABUF_FLUSH_PER_CYCLE` (2)
datagrams ride alongside the current record, each packed to stay inside the
transport's packet limit, kicking the watchdog and asking for no reply.
- Ring, decimation, packing and refusal of oversized records are covered by a
host-compiled test (FIFO order under capacity, order and span preserved across
three thinnings, nothing lost when the link is down, oversized records refused
rather than truncated, multi-record buffers split correctly).

### Modem recovery no longer tears the radio down (`src/modem.c`)
- **Losing coverage is now left to the modem**, which handles registration,
cell reselection and RAT reselection autonomously — the same way a phone does.
Previously three consecutive failed sends triggered `lte_lc_offline()`, which
deregisters, and five triggered a full modem-library shutdown. At a five
second cadence that is roughly fifteen and twenty-five seconds of trouble, so
a motorway tunnel was enough to fire it. Both paths discard everything the
modem knows about local cells and force a full band scan afterwards, which on
LTE-M and NB-IoT takes minutes — making the outage substantially longer than
doing nothing, and risking the 3GPP backoff timers on top.
- **Escalation is now time-based and only counts time spent registered but
unable to send**, which is the contradictory state actually worth acting on
(usually a PDP context the network has silently deactivated).
`APP_MODEM_STUCK_CFUN_S` (default 600) and `APP_MODEM_STUCK_RESET_S` (default
1800) replace `APP_GSM_ESCALATION_POWERCYCLE`, `APP_GSM_ESCALATION_SLEEP` and
`APP_GSM_RECOVERY_SLEEP_INTERVAL`. Time spent unregistered is not counted.
- **Send failures while unregistered no longer count.** Registration state
comes from the LTE event handler rather than being inferred from a failed UDP
datagram, which carries almost no information about the radio.
- **GNSS is never stopped for a modem problem.** The old recovery called
`gnss_stop()`, so a radio outage also lost position for its whole duration —
the difference between a gap in the telemetry and a gap in the journey. A
12-minute hole in a drive on 2026-09-05, ending on NB-IoT with no position
data at all, is what that looked like.
- `modem_recover()` lost its failure-count parameter; `modem_is_registered()`
and `modem_send_ok()` are new.

## 0.4.16

### IF MCU: SEVONPEND fix on every power-off path (`ifmcu/patches/`)
- makerdiary/nrf9151-connectkit#19 put the Connect Kit's nRF52820 into
SYSTEM OFF on USB unplug and found that `sys_poweroff()` needs SEVONPEND
cleared first, but applied that only to the unplug path. The charger poll
and the shell `shutdown` still called the bare poweroff, and the poll can win
the race after an unplug (the BQ25180 drops VIN-good at a higher voltage than
the nRF52820's VBUS detect) and hang the chip at ~2 mA with the fixed path
queued behind it on the same workqueue. `ifmcu/patches/0001-ifmcu-system-off-
sevonpend.patch` routes all three sites through one helper; `ifmcu/build.sh`
now applies `ifmcu/patches/*.patch` before building. Drop the patch once it is
merged upstream. See QUICKSTART.md.

### Update inhibit for bench builds (`CONFIG_APP_FOTA_INHIBIT`)
- **New flag, default off.** Leaves the update machinery compiled in but never
uses it: no manifest fetch at power-on, no download, and a server advertising
a newer version or sending a manual `fota` command is ignored.  The power-on
check is unconditional, so without this a local build — version 0.4.0, below
whatever the fleet is on — is swapped out within seconds of booting and the
change under test never runs.
- **Deliberately not `APP_FOTA=n`**, which also stubs out
`fota_confirm_image()`.  An image installed over the air boots on probation
and MCUboot reverts it on the next boot unless that call runs, so disabling
the whole subsystem would make a test build delivered by FOTA roll straight
back.  With the inhibit the image is still confirmed.
- Logged once as a warning so it is obvious from the console why a unit is
not updating.

### K-line vehicle init at boot (`CONFIG_APP_KLINE_DISCOVER`)
- **`kline_vehicle_init()`** in `hw_kline.c`: opens a diagnostic session with
the vehicle's ECU over K and reports the outcome on the console and as an
alert (priority 1 on success), then the tracker starts as normal.  Stages,
each gated by its own Kconfig: the KWP2000 5-baud init (ISO 14230-2) on 0x33,
the ISO 14230-4 fast init (`APP_KLINE_INIT_FAST`), the 5-baud handshake on a
list of known addresses (`APP_KLINE_INIT_ADDRS`, with a raw capture of any
address whose handshake breaks down), and full physical-address sweeps of
both inits (`APP_KLINE_INIT_SWEEP`, ~15 min).  `APP_KLINE_INIT_DIAG` adds an
L-hold and a listen window for meter-and-wire checks.  Nothing is sent over K
beyond the init itself.  See KWIRE.md.
- **10.4 k / 9600 data bytes go through UARTE1** via the nrf HAL (the Zephyr
UART driver only knows a fixed list of rates); GPIO bit-banging is kept for
the 5-baud address, the wake-up pulse, and the bench loopback.  The bit-bang
receiver's few-percent rate error misread the last bit of a real ECU's key
byte, which is what made the handshake fail.  `APP_KLINE_BAUD` sets the
rate (default 10400); the slow init retries at the other of 9600/10400 when
the sync byte does not decode.
- **Result:** a 2006 Toyota Harrier 2.4 (ACU30) answers on ECU addresses
0x13/0x29/0x58/0xB4 with KWP2000 key bytes E9 8F at 9600 baud, on K alone;
the handshake completes on all four.
- **OBD-II telemetry over the K wire** (`APP_KLINE_TELEMETRY`, default off):
each telemetry record carries the mode 01 PIDs the ECU supports — RPM, speed,
coolant and intake temperature, load, throttle, MAF, timing advance, fuel
trims, the malfunction lamp and the stored-code count — polled at the moment
the record is built.  The support bitmap is read once per session and only
advertised PIDs are requested.  The session is opened once and held for the
drive rather than per record, since a 5-baud init holds the bus dominant for
2.4 s; a request that times out reopens it once.  Values travel as scaled
integers so the packet needs no float formatting.  New `obd_*` columns on the
server's `log` table.
- **Fault-code reporting** (`APP_KLINE_DTC_REPORT`, default off): stored codes
are read after ignition-on and whenever the stored-code count in mode 01
PID 01 changes — that byte is already read by every telemetry poll, so a code
appearing or clearing mid-drive is caught immediately with no extra bus
traffic, and there is no periodic re-read.  There is no ignition-off read:
sleep is only entered with the ignition off and the ECU unpowered, so it
could only ever time out.  Codes are sent as a standalone
`D,<code>,...` line carrying the complete current set.  The server reconciles
it against a new `dtc` table (device, code, `raised_at`, `cleared_at`,
`active`), alerting at priority 1 on codes appearing and 0 on codes clearing,
and keeping every occurrence as history.  A failed read sends nothing rather
than an empty set, which would wrongly clear live faults.  Read-only.
- **Review fixes** before any of this ran on a vehicle: a silent ECU was being
reported as "no fault codes", which would have cleared every live fault on the
server (a timeout now returns an error, and the session is dropped at
ignition-off so a stale one cannot be mistaken for a working one); the
mid-drive fault-code trigger was hung off `STATE_IDLE`, which a drive never
enters with `BATCH_SIZE` 1, so it could not fire until the next key-on; the
RPM accumulator's minimum started at zero and so never moved; `obd_close()`
cleared the abort flag one line after it was set; and the per-frame request
tracing written for discovery was running in the poll path, which with
`CONFIG_LOG_MODE_IMMEDIATE` is synchronous console I/O about once a second
forever.
- **A silent mode 03 is disambiguated by the stored-code count.**  Treating
silence as a failure is right in general, but ECUs that never answer mode 03
when they hold no codes — the reference Toyota among them — then never report
at all and retry forever.  Mode 01 PID 01 carries the count independently, so
silence is read as an empty set only when that count is zero.  Failed reads
now back off for 30 s instead of retrying every loop iteration.
- **Fault-code reporting retries until a report lands.**  The count watch only
fires on a change, so a failed ignition-on read would otherwise mean codes the
ECU already had were never reported at all.
- **Fault-code alerts are batched**: one notification per event listing the
codes, not one per code — a single root fault routinely raises three or four.
- **`obd_speed` is stored in mph**, converted server-side with the same
constant and in the same function as GNSS speed, so the two speed columns are
directly comparable.  The wire stays km/h, which is what both sources natively
produce, and firmware-internal use stays km/h because the coast-to-stop
threshold is expressed that way.  The column changed from an integer to
`decimal(5,2)` to match `speed`.
- **The ECU's own figures now drive the tracker's movement and engine-state
logic**, with automatic fallback when it is not answering.  Vehicle speed
(PID 0x0D, km/h by J1979 on any market's vehicle) replaces GNSS speed for
"are we moving": across 106 stationary records the ECU read 0 km/h throughout
while GNSS averaged 0.66 mph and peaked at 5.11 mph.  Engine RPM (PID 0x0C)
replaces the 13 V charging-voltage proxy for `engine_running`, which was
mis-reading a running engine as stopped and dropping the tracker to its 30 s
engine-off cadence mid-drive.  Both are refreshed every 3 s by the keep-alive
rather than only when a record is built, since records are 30 s apart in the
engine-off state.
- **The diagnostic session is kept alive across a whole cycle.**  P3max is 5 s
and a tracker cycle does not naturally stay inside it: the OBD poll happens
after the GPS fix, so the send and idle that follow are unprotected and a slow
send alone can exceed it.  The RPM sampler covers the fix wait; a new
`obd_keepalive()` on the main loop covers the rest, firing only after 3 s of
silence and reading mode 01 PID 01 — which is also the stored-code watch, so
it costs nothing extra.  Reopening is rate limited to two per minute, after
which the firmware stands off for a minute rather than re-initialising the
bus every cycle.
- **Engine RPM is sampled ~1 Hz**, not once per record, from a new
`gnss_set_tick()` callback on the GNSS fix wait — the thread is otherwise
asleep on a semaphore there, and running on it means no locking against the
rest of the K-wire code.  Reported as `obd_rpm_min` / `_max` / `_avg`
alongside the instantaneous value.  The sampler never opens a session.
- **Ignition-off is handled throughout the K-wire runtime path.**  Every entry
point checks it, a poll in flight abandons its remaining PIDs on the first
timeout with the ignition gone instead of burning one per PID, runtime
requests use a 250 ms timeout against discovery's 1 s, and the session is
closed without StopCommunication when the ECU is already unpowered.
- **`src/kline_obd.c`** holds the application layer — which PIDs to ask for,
unit conversion, record formatting — while `hw_kline.c` keeps the wire and
the discovery.
- **Discovery and the runtime session are now separate operations.**
`APP_KLINE_INIT_AT_BOOT` is renamed `APP_KLINE_DISCOVER`: a one-shot
investigation of an unknown vehicle that hunts protocol, rate and addresses,
asks each responder what it supports, and ends with a summary plus the
`local.conf` block that configures the runtime path — so the log only has to
be read once.  Addresses that handshake but answer nothing are listed as
inert.  Alongside it, `kline_session_open()` / `kline_obd_pid()` /
`kline_session_close()` are the polling path: no address hunting, no rate
retry, no capability probing, opening at `APP_KLINE_ECU_ADDR` (new, default
0x33) and `APP_KLINE_BAUD`.
- **ECU identification and fault codes** (`APP_KLINE_IDENT`, `APP_KLINE_DTC`,
both default off): with a session open, ask each address who it is —
StartDiagnosticSession, ReadEcuIdentification, OBD mode 01 supported-PIDs /
MIL and DTC count / RPM / coolant, mode 09 VIN — then read stored, pending and
permanent DTCs (modes 03/07/0A), decoding each to its P/C/B/U form and
following multi-frame responses.  Every request is read-only; mode 04, which
erases codes and the readiness data with them, is deliberately not
implemented.  On the reference vehicle 0x13 is the engine ECU (PID bitmap
BE 1F B8 00, live RPM and coolant, speed on PID 0D); it supports no PID above
0x15 and ignores every non-OBD service, so no VIN is available over K.
- **`APP_KLINE_BAUD` and `APP_KLINE_INIT_ADDRS` are now unconditional.**  They
had `depends on APP_KLINE_DISCOVER`, but `hw_kline.c` is compiled for every
board and reads both as values, so turning the boot init off broke the build.
- **Terminology:** the interface is now called the K-wire (ISO 14230-1
K-line) throughout — Kconfig prompts, board_test.sh, console banners,
comments.  "ISO-9141" was inaccurate: the physical layer is ISO 14230-1 and
the protocol in use is KWP2000 (ISO 14230), not ISO 9141-2.  ISO9141.md is
replaced by KWIRE.md, which covers the bench loopback test, the in-vehicle
session init and the reference-vehicle configuration.

### LTE TX power / brown-out test mode (`src/lte_power_test.c`)
- **New bench rig, `CONFIG_APP_LTE_POWER_TEST`** (build with
`LTE_TEST=1 BUILD_SUBDIR=build_lte_test ./build.sh`). Replaces the tracker:
the modem registers once at boot, then the board idles with LED1 on. An
ignition OFF->ON edge — or ENTER on the serial console, when USB is attached
(a PSU-only run, the point of the test, has none: USB takes over the power
input) — starts a ~10 s burst of uplink UDP datagrams pushed at the
modem as fast as it will queue them, keeping the radio transmitting for the
whole window — the sustained full-power TX load of a unit in a low-signal
area. LED2 joins LED1 for the burst; if the supply carries it, all three LEDs
light and hold until ignition is turned OFF, which returns the rig to LED1
alone and re-arms the trigger. A brown-out resets the board
instead, so LED3 never lights and the unit comes back up at LED1 only. The
burst reports progress and the modem's own VDD reading (`AT%XVBAT`, sampled
under TX load) every 2 s, so sag is visible on the console before it becomes
a reset.
- **`led_mask()`** added to `led.c`: direct steady-state control of LED1-3
(stops every pattern timer first), used by the rig for its fixed LED states.

### Relay support removed (firmware + server)
- **The latching relay is gone from the product, so it is gone from the code.**
`src/hw_relay.c`, `APP_RELAY_CONNECTED`, the four `APP_PIN_RLY_*` assignments,
`APP_BOARD_RELAY_FB_ON_AUX` and its `hw_domain.c` park/release entries, and the
`relay=` server command are all deleted.
- **`always_on` went with it.** Its only effect was holding the relay set
regardless of ignition, so `APP_ALWAYS_ON`, `g_settings.always_on` and the
`ao=` command are removed too.
- **Wire format changed.** The settings-sync group is now `,int=<n>;ma=<n>`
and the server response is `1,<int>,<ma>[,<cmd>]` — one field shorter in both
directions. The server (`/var/www/tracker/main.py`) drops `ao` in lockstep:
`UPDATE_KEYS`, the `ao` extras key, the `/api/1.0/config` field, and the
response builder. **Sequencing matters** — an older image parses the shorter
response with `matched == 2`, which fails its `>= 3` gate and silently discards
both the settings and the `fota=` indication riding the command field. Publish
the firmware first, let the fleet take it, then restart the server.
- The unused `ALWAYS_ON_POWER` macro (a leftover from the Polaris port,
referenced nowhere) is removed as well.
- **Dropped the relay-only battery wake.** `do_sleep()` armed a
`BATTERY_CHECK_INTERVAL` countdown when `loop_interval == 0` so a relay unit
could still wake to check the battery and cut power — but the telemetry block
it fed is gated on `loop_interval > 0`, so it only ever produced a wake that
did nothing. It is gone with the relay.
- The `device.always_on` column is left in place on the server; nothing reads
or writes it now.

### Board test boot noise (`board_test.sh`, `src/main.c`)
- **The boot rail self-test is skipped in board-test builds.** Tests 1 and 2
walk the same rails interactively moments later, so at boot `hw_selftest()`
only cycled them a second time and pushed the start prompt off the screen.
- **Module logs default to warning** in the generated test overlay
(`APP_LOG_LEVEL=2`). Every test prints its own result with `printk`, so the
driver init chatter was pure scrollback; warnings and errors still print, and
`VERBOSE=1 ./board_test.sh` puts them back at info.
- **No more serial port menu** — the console is always the first
`/dev/cu.usbmodem*` (the Connect Kit's DAPLink exposes two). `SERIAL=` still
overrides.

### Demo mode (`APP_DEMO_MODE`)
- New build flag that masks latitude and longitude wherever the firmware
prints them on the console — the telemetry records echoed by `send_data()`, the
`locate`/`tomtom` alert text logged by `alert_enqueue()`, and the board test's
raw-fix line (`board_test.sh` copies the flag into its generated overlay).
`APP_BOARD_TEST_HIDE_COORDS` stays the stricter board-test option — it drops
the field rather than masking it, and wins if both are set. Coordinates are matched by
shape in `demo_mask_coords()` — five or more decimals at the start of a field —
so the timestamp's fractional seconds, the `%.2f` voltages and the `%.1f`
temperatures print as usual. Console only: the record and the alert still carry
the real position to the server.

### v3.3 carrier board (`Kconfig.boards`, `board_test.sh`)
- New `APP_BOARD_L0DESTAR_V3_3` profile, extracted from the KiCad netlist in
`../hardware/l0destar_v3.3/`. Same map as v3.1 — split OBD domain, four
rail-sense inputs, MCP2518FD XSTBY, no INA228 ALRT — except LED1 and LED3
swap pins (P0.28 / P0.26), L_SENSE arrives on P0.14 (AIN1) and the PP3V3_GPS
rail sense moves to P0.05 to free that AIN channel. Selectable from
board_test.sh, where it is now the default entry.

### K-wire L line (`src/hw_kline.c`)
- **Driving the L line is now off by default on every board before v3.3**
(`APP_L_SEND_ENABLED`). Those boards switch the 2N7002 pulldown straight
across the wire, and an L wire shorted to battery is indistinguishable from a
healthy idle one — both sit at 12-16 V — so a 5-baud init saturates the FET
into the short, where it dissipates 1-12 W in a SOT-23 and fails inside the
first address bit. Roughly half of those failures involve the gate, which puts
battery voltage on the L_SEND GPIO, past the nRF9151's absolute maximum. The
pin is still parked low (FET off); `kline_l_send()` is now the only way to
assert it and returns `-EPERM` where the gate is off. Cost is the L half of a
5-baud init, which nothing implements yet.
- **L_SENSE support (v3.3+)**, which is what makes that short detectable.
v3.3 taps the wire through a 1N4148 (cathode to L) and a 47K: the diode blocks
the vehicle's 12 V from the pin, so the line can only be read by sourcing
current into it. `kline_l_sense_mv()` samples it on the SAADC with the
internal pull-up resistor ladder engaged — ~0.7-0.9 V when the line is pulled
low, full scale when it is high, open or shorted. A GPIO input cannot do this
(the nRF's ~13K pull-up against the 47K leaves a grounded line at ~2.7 V,
above VIH) and Zephyr's ADC driver hard-codes the ladder to bypass, so this
goes through nrfx directly. `kline_l_line_probe()` pulses the pulldown for
5 ms and reports whether the line followed; `kline_test()` (board test step
10) prints both readings.
- The SAADC can only sample AIN0-AIN7 = P0.13-P0.20, so `kline_l_sense_init()`
rejects an `APP_PIN_L_SENSE` outside that range and reports the sense
unavailable rather than silently returning garbage.

### Board bring-up test (`src/board_test.c`)
- The tilt and impact steps merged into one accelerometer test: it streams
live roll/pitch instead of demanding 90° on each axis, keeps the impact
interrupt armed throughout, and continues at the first impact (reported with
its metrics). The suite is 11 steps now, not 12.

## 0.4.9

- Rail sensing was boot-only — the README's headline v3.1 feature wasn't
actually protecting anything. hw_domain_request() raised the enable, slept 15 ms
and assumed success. If a load switch failed, hw_can_init() would then drive
CS/SCK/SDI as outputs into an unpowered MCP2518FD — precisely the clamp-diode
backfeed the README's Notes exist to prevent. The request path now waits for the
domain's sense line(s), and on failure re-parks every pin, drops the enable,
alerts once (latched so it can't spam the 5-deep queue), and returns -EIO.
hw_can_init/hw_can_power_on/kline_power_on now bail instead of driving.
hw_domain_request and kline_power_on changed from void to int.

- The self-test's "rail off" check would false-alarm on healthy boards. It
sampled once after a fixed 20 ms. Nothing on these rails is actively discharged:
PP3V3_CAN carries 10.2 µF (S9C5 + two 100 nF) draining through the parked
CAN_CS/CAN_INT pulldowns in series with their 10 K pull-ups once the MCP2518FD
and MAX33041 drop out of regulation — roughly 130 ms to fall below the sense
threshold. PP12V_K is ~100 nF against the 280 K sense divider plus the
TJA1027T's sleep current, which lands in the same tens-of-ms range as the old 20
ms window. Both now poll with a 500 ms timeout, keeping the per-rail alert
labels.

## 0.4.5

### Ignition-off telemetry (`src/main.c`, `src/data.c`)
- **The final ignition-off point is no longer lost to a latching race.**
  `collect_data()` bakes the ignition state into the record, but `STATE_SEND`
  then set `previous_ignition` from a *fresh* read of the line.  Those differ
  routinely: the collect blocks up to `GPS_FIX_TIMEOUT_MS`,
  `handle_ignition_state()` re-reads every loop iteration, and `BATCH_SIZE` is
  1, so turning the key any time between the two left the server marked as
  told "off" while the record it received said "on".  The transition was
  consumed unsent, the state machine slept, and the drive ended on an
  ignition-on point at the parking spot.  `previous_ignition` now tracks what
  the record actually carried, the line is re-read after `send_data()`, and a
  pending change routes back through `STATE_GPS_COLLECT` instead of to sleep
- A no-fix record is no longer discarded outright either.  `collect_data()`
  returned 0 without a fix, and both ignition-off paths advanced
  `previous_ignition` regardless, so that transition was lost too
- `force_record` (set only for an ignition change) builds the record from the
  last known position instead.  At ignition-off that is where the vehicle is;
  `speed` and `satellites` report 0 rather than stale values, and the cell
  fields go out with `cl=1`
- `cl` was previously hardcoded to 0, so the GPS fallback the protocol already
  described could never actually occur
- A transition whose send fails no longer advances `previous_ignition`, so it
  is retried rather than lost to one bad send
- Coast-to-stop now requires a live fix, so a stale speed can't start it
- Still dropped if the unit has had no fix at all since boot: the CSV would
  carry empty lat/lon into the server's `log` row

### Per-device firmware builds (`remote.conf`, `push_fw.sh`, `src/fota.c`)
- Images are built and published per IMEI, from `remote.conf` rather than
  `local.conf`, so a bench session's board config can't ship to a deployed
  unit.  The server resolves `/fw/manifest.txt?imei=` to that device's
  manifest; a device not listed gets no `fota=` and no update
- Manifests carry `board=<APP_BOARD_ID>[+can][+kline][+aio]` and the device
  refuses an image that doesn't match its own build
- `push_fw.sh` refuses to publish a build carrying bench settings, and reads
  the server over HTTPS (`/fw/published.txt`) rather than ssh
- `VERSION` holds `0.4`; the patch number is derived from what is published
  and never committed
- Removed a stray `CONFIG_APP_DEBUG_IGNITION=0` from `makerdiary.conf`, which
  applied to every Connect Kit build including deployed units and was masked
  on the bench by `local.conf`

### Over-the-air updates (`src/fota.c`, [FOTA.md](FOTA.md))
- MCUboot added via `sysbuild.conf`, splitting the 1 MB flash into two 416 KB
  slots. **The first build carrying it must be flashed over SWD** - a unit
  running a pre-MCUboot image has no bootloader to swap slots
- `pm_static.yml` pins the flash map so later builds stay installable by the
  bootloader already on deployed units, and an image that outgrows its slot
  fails the build instead of silently re-laying-out the map
- Zero-polling trigger: every telemetry response carries `fota=<latest>`
  (server reads fw/manifest.txt, cached on mtime); the device compares against
  its running build locally and only fetches the manifest + image when the
  server has something newer.  Steady state costs no extra requests.  One
  unconditional check at power-on; bare `fota` server command forces one
- Served from the telemetry TLS port 65481 - the one TCP path forwarded to
  the server - whose listener protocol-sniffs the first two bytes and answers
  HTTP GET/HEAD on /fw/* with keep-alive + 2 KB range support alongside the
  telemetry framing (`_handle_fw_http` in the server)
- Device trusts the endpoint via a dedicated sec_tag (42, auto-provisioned
  with the same private CA): telemetry tag 1 carries leftover DTLS/PSK
  credential types in modem NVM, and a mixed tag breaks cert-mode TLS connects
- `./push_fw.sh` releases an update: build, stale/non-newer refusal, upload,
  atomic manifest flip, then end-to-end endpoint verification (CA, manifest,
  ranged 206) the way a device fetches.  Manifest URL carries `?imei=&v=` for
  per-device staging
- Failed attempts hold off retries (10 min doubling to 80 min) since the
  server re-advertises on every response
- Verified on the bench (v3.0): 0.4.0 -> 0.4.1 advertised on a sleep
  telemetry wake, downloaded, swapped, rebooted and self-confirmed unattended,
  with `fota:` alerts at both ends of the swap
- Downloads are deferred below `CONFIG_APP_FOTA_MIN_BATTERY_MV` (12.0 V);
  nothing is written to flash before that gate, so an update can't drain a
  weak battery and a brownout mid-download can only spoil the secondary slot
- A swapped image is `BOOT_UPGRADE_TEST` until `main()` finishes bring-up and
  calls `boot_write_img_confirmed()`, so firmware that hangs or faults during
  init is rolled back on the next boot
- Version lives only in the `VERSION` file, feeding `<zephyr/app_version.h>`,
  the MCUboot image header and the comparison in `fota.c` - they can't drift.
  Reported to the server as `fw=` in the settings-sync field and by `config`
- Application flash use 156 KB -> 178 KB of the 320 KB app partition

### v3.1 carrier board (`Kconfig.boards`)
- v3.1 board profile re-extracted from the KiCad netlist in
  `../hardware/l0destar_v3.1/` and corrected: CAN_CS/SDI/SCK/SDO were
  rotated (now P0.15/16/17/18, header J3-23..26) and ACC_INT1 moved
  P0.31 -> P0.30, since P0.31 carries the 12V rail sense
- Split OBD domain (`CONFIG_APP_BOARD_SPLIT_OBD_DOMAIN`): v3.1 has three
  independent load switches, not v3.0's shared OBD_ENABLE.  GPS_ENABLE
  (P0.13) gates the bias tee, CAN_EN (P0.23) gates PP3V3_CAN, K_EN (P0.10)
  gates PP3V3_K + PP12V_K.  New `HW_DOMAIN_CAN`; the K-line moves to
  `HW_DOMAIN_K`, so selecting one OBD interface no longer powers the other
- Four rail-sense inputs instead of three: PP3V3_GPS (P0.14), PP3V3_CAN
  (P0.22), PP3V3_K (P0.0) and PP12V_K (P0.31).  The three 3.3V senses are
  100K/1M dividers off their own rail (high = up, ~3.0 V, ~3 uA); the 12V
  sense is a 2N7002 inverter with a 100K pull-up to the always-on rail and
  reads **low** when PP12V_K is present (`APP_BOARD_RAIL_ST_12V_ACTIVE_LOW`).
  All four are read with no internal pull so they can't fight the dividers
- `hw_selftest()` follows the split: `CONFIG_APP_OBD_MODE=1` cycles CAN_EN
  and checks PP3V3_CAN only, mode 2 cycles K_EN and checks PP3V3_K plus the
  inverted PP12V_K

### Sleep-state power
- Console UARTE is suspended for the duration of the blocking wait in
  `do_sleep()` and resumed on every wake (`CONFIG_PM_DEVICE`).  An enabled
  UARTE holds the nRF91 HF clock even with no traffic (~600-900 µA at 3.3 V),
  which accounted for the bulk of the ~300 µA parked input draw measured on
  v2.5K; the expected parked floor is now ~40-55 µA at the 12 V input.  All
  logging while awake is unaffected - the deferred log queue is drained
  before each suspend, and anything logged during the wait is dropped by the
  suspended driver rather than blocking.

## 0.3.0 - 07/08/2026

### Carrier-board definitions
- One selectable board definition per l0destar PCB (`Kconfig.boards`, chosen
  via `CONFIG_APP_BOARD_*` in local.conf): full GPIO map, fitted-hardware
  flags and power-domain topology for v2.1, v2.1 mini, v2.5C/K/M, v2.6C/K/M
  and v3.0, extracted from the KiCad PCBs and verified against the board
  netlists. Bench (DK / Connect Kit + breadboard) remains the default and
  keeps the previous pin assignments, including the Connect Kit console
  re-park (now a `BOARD_NRF9151_CONNECTKIT`-conditional Kconfig default
  instead of makerdiary.conf, so it no longer overrides PCB pin maps)
- Per-board hardware presence handled in code: relay skipped when the pins
  aren't fitted, K-wire test skipped without the second bench transceiver,
  LEDs active-high on all PCBs (bench stays active-low), external ignition
  pull-up used on PCBs instead of the internal one (~3x lower sense current
  with ignition on), LED4/5 parked on v2.1 mini

### Power-domain sequencing (`src/hw_domain.c`)
- All signals terminating in a switched rail are parked (input + pulldown)
  before that rail drops and released only after it rises: CAN SPI pins
  (MCP2518FD abs max is VDD + 0.3 V - a high pin would backfeed the dead
  rail through the clamp diodes), CAN_INT/CAN_CS whose 10K pull-ups sit on
  the switched rail, K-line pins (TXS0104E A-port / TJA1027T with switched
  pull-ups), and v2.1 relay feedback (supplied from the AUX domain)
- Domains are reference-counted per board topology: v2.x single AUX domain,
  v2.5K/v2.6K AUX + K_EN (pins released only with both rails up), v3.0
  GPS_ENABLE and OBD_ENABLE switched separately - engine-off telemetry
  wakes now power only the GPS bias rail, and sleep wakes re-enable it
  (previously the aux rail stayed off after the first sleep, killing GPS)
- v3.0 TJA1027T put to sleep (SLP_N low) before its rail is cut

### CAN controller power handling (`src/hw_can.c`)
- MCP2518FD verified and configured at boot, then left in sleep mode
  (~10 uA); `IOCON.XSTBYEN` drives the transceiver standby pin from sleep
  state on v2.5C/v2.6C (TCAN334 STB otherwise floats - it has no other
  drive) and v3.0 (MAX33041). On v2.5/v2.6 the CAN rail shares the GPS AUX
  domain, so controller sleep + transceiver standby is the only power-off
  path during engine-off telemetry wakes; on v3.0 the OBD domain switches
  off entirely

## 0.2.1 - 13/06/2026

### Telemetry
- Gyro zero-rate bias auto-zero: while stopped (good GNSS fix, speed below
  `GYRO_REST_KMH`) the device averages a short burst of raw samples and
  EMA-tracks the sensor's temperature-dependent offset, subtracting it from
  every `gx/gy/gz` reading. Bench data showed gy ≈ −132 LSB (~−1.2 dps) at
  rest; logged rates are now honest. A per-burst rotation reject
  (`GYRO_AUTOZERO_REJECT_LSB`) prevents a stale/zero GNSS speed during motion
  from corrupting the offset.

## 0.2.0 - 12/06/2026

### Transport & security
- Replaced the ChaCha20-Poly1305 UDP envelope with DTLS 1.2 offloaded to the
  modem (server cert verification at sec_tag 1, DTLS CID + session caching,
  RAI hints to release the radio after each exchange)
- Server CA generation script (`certs/gen_certs.sh`); CA embedded via
  `src/ca_cert.h`

### GNSS & A-GNSS
- A-GNSS assistance from nRF Cloud REST (`src/agnss.c`), authenticated with a
  per-device JWT - cold TTFF drops from minutes to ~10 s
- Device onboarding to nRF Cloud without external sample firmware: `PROV=1`
  build (`prov.conf`, `APP_PROVISION_MODE`) turns the app into an AT-host
  bridge for `nrfcloud-utils`; credentials persist in modem NVM
- GNSS cold-start handling: extended timeout, periodic priority-mode windows,
  restart on cold-fix timeout

### Telemetry
- Gyroscope enabled (104 Hz ±250 dps) - `gx/gy/gz` (raw LSB) per record
- nRF9151 SiP die temperature - `mt` (°C) per record, via `AT%XTEMP`
- IMU die temperature - `it` (°C) per record
- Accelerometer fields `ax/ay/az` now in milli-g (FS-independent; previously
  raw LSB at ±2 g)
- Record batching support (`BATCH_SIZE`)
- Low-battery warning gated to ignition-off and demoted to normal priority -
  with smart/regenerative charging the rail swings 11.8–14.9 V by design while
  driving (load-shed at idle/under acceleration, boosted on overrun), so an
  instantaneous mid-drive dip is no longer mistaken for a failing battery

### Impact detection
- While awake: accel at ±8 g with a high-g interrupt
  (`APP_CRASH_THRESHOLD_MG`, default 4 g) and a 26 Hz accel+gyro FIFO ring
  (~9 s of history); on impact the ring is drained and the alert carries peak
  g (per-axis), peak rotation rate, disturbance duration and speed, with the
  waveform around the peak dumped to the serial log
- While asleep: FIFO keeps running accel-only at ±2 g; unconfirmed movement
  wakes are classified by true FIFO peak - `parked impact` alert above
  `APP_PARKED_IMPACT_MG` (default 0.8 g) instead of being silently ignored
- 100 ms settle after accel full-scale changes, fixing a spurious wake
  interrupt (and false impact) fired by the sensor's slope filter on every
  sleep entry

### Theft detection (while parked/asleep)
- Tow/jack: gravity vector polled against a sleep-entry reference every
  `APP_TOW_POLL_S` (30 s); sustained tilt past `APP_TOW_TILT_DEG` (6°) raises
  a `tow/jack` alert - catches slow, vibration-free flatbed lifts and jacking
  that never trip the motion wake
- Tamper: the IMU's 6D orientation engine is armed during sleep; a change of
  orientation zone vs. the armed face (unit flipped / pried off its mount)
  raises a `tamper` alert, checked on every wake (zone compare, not the
  transient event flag)

### Modem
- Rel-14 features + RAI requested before network attach; RAI URC logging
- eDRX/PSM disabled for continuous tracking duty cycle

### Configuration & hardware abstraction
- All pin assignments moved to Kconfig (`APP_PIN_*`) with production-PCB
  defaults - any signal remaps from `local.conf`; every pin has a defined
  assignment even when the hardware isn't fitted (relay pins parked)
- Bench debug overrides replace in-code stubs: `APP_DEBUG_IGNITION`,
  `APP_DEBUG_BATTERY_MV`, `APP_RELAY_CONNECTED`; live GPIO ignition sense and
  INA228 battery reading restored as the default paths
- Removed dead code: duplicate `transport 2.c`, orphaned `stubs.c`, unused
  `DK_PIN_WORKAROUNDS` and `NRF_CLOUD_KEY` Kconfig symbols

### Build & docs
- `flash.sh` (auto-picks the connected J-Link); `build.sh` overlay support
  for `local.conf` + `prov.conf`
- README rewritten: architecture, DTLS protocol, nRF Cloud provisioning
  run-book, impact detection, full Kconfig reference
- Corrected IMU part: ASM330LHHX (automotive 6-axis), not LSM6DSO

## 0.1.0 - 29/05/2026

- Initial working l0destar firmware for nRF9151
