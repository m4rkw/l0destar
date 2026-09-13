# Changelog

## Unreleased

### A failed A-GNSS fetch no longer leaves a cold start unassisted
- **The in-search fetch is retried.**  When the receiver asks for assistance
and the fetch at the start of a cold search fails, `gnss_collect()` now
tries `AGNSS_RETRIES` (2) more times, `AGNSS_RETRY_INTERVAL_MS` (15 s) apart,
from inside the fix wait and only while the modem is registered; the
receiver keeps searching between attempts.  It used to make one attempt and
then wait out the search unassisted, with nothing asking again until the
next cold collect.  Observed on 2026-09-13: one fetch timed out at key-on
(-116, HTTP 0), the search took 2 min 23 s, and no telemetry went out
meanwhile.
- **GNSS priority mode is only requested when the receiver is starved.**
Every cold collect turned it on straight away and re-armed it every 30 s,
holding it for the whole search — over two minutes in that same cold
start, during which the modem dropped off the network.  It is now requested
only after `GNSS_PRIO_STARVED_EPOCHS` (5) consecutive epochs flagged
`NOT_ENOUGH_WINDOW_TIME` — Nordic's documented trigger, and the count its
location library uses — and not again while the modem's 40 s window may
still be running.  The first request in a search logs at WRN, so it reaches
the server's device log.

## 0.4.42

### A lost datagram no longer silences a parked unit
- **A reboot always leaves something to wake on.**  Settings live in RAM, so
every boot starts at the compiled `APP_ENGINE_OFF_LOOP_INTERVAL` — 0 on
these builds, meaning "never wake to report" — and only a server response
restores the real value.  One lost record was therefore enough to silence a
parked unit indefinitely.  Observed on 2026-09-12: the unit booted into
0.4.39 at 19:21:18, sent its post-boot record (the modem accepted the
datagram; the server never saw it, and logged no rejection), got no
response, and then had no telemetry timer for 90 minutes until the key
turned — uptime 5519 s on the next record, so it had neither rebooted nor
crashed.  `ENGINE_OFF_BOOT_INTERVAL` (900 s) is the cadence until the first
response, which then replaces it, including with 0.
- **`fw=` and `rst=` are cleared by the reply, not by the send.**
`s_fw_pending` was cleared as soon as `transport_send()` accepted the
datagram, so the once-per-boot diagnostics were discarded even when nothing
received them — precisely the case where the reset cause is worth having.
They now clear on a decodable response, the only proof the record arrived,
and ride the next record until then.  It is why the boot above is
unexplained: its `rst=` went out once and was lost.

## 0.4.39

### A waiting update installs at key off
- **`fota_check()` runs on the way into sleep**, so an update the server has
advertised is installed when the engine stops rather than at the next
telemetry wake.  The device already knew one was waiting — `fota=<version>`
rides every response, including the reply to the ignition-off record it has
just sent — but the key-off path reached `STATE_SLEEP` without passing any
of the three `fota_check()` call sites, so the pending flag sat there for
up to an hour.
- Key off is also the right moment for it: the radio is still registered
from that last record, the battery has just come off the alternator, and
the vehicle is not about to be driven.  The alternative was a download an
hour later on a colder battery, and only on a unit that still had an
engine-off interval to wake on.  A no-op when nothing is pending, so an
ordinary key off costs no traffic.

### The "updated to" notification comes from the server
- **The device no longer raises it; it only logs it.**  It could only ever
raise it on the one boot that writes the MCUboot confirm flag — every later
boot returns at `boot_is_img_confirmed()` — and on that boot it was an alert
queued in RAM that had to survive a working link and no reset before the
next send.  When it did not, nothing re-raised it, so an update that worked
perfectly could look silent and a missing notification meant nothing either
way.
- The server raises it instead, in `fw_check_running()`, when the version a
device reports in `fw=` matches the version it was told to stage.  That is
state rather than a one-shot event: it survives every reboot and arrives
with the first record that gets through.

### A failed update is remembered, reported, and not retried forever
- **The device knows whether its own update took.**  What it stages is
recorded in `__noinit` RAM, which survives the reboot that applies it, and
the next boot compares it against what is actually running: the same
version means success, anything else means MCUboot reverted it.  Until now
the device had no memory of having tried, so a bad image was downloaded
again on every wake — 300 KB an hour with the engine off, which is a
flattened car battery rather than a missed update.
- **A reverted version is blocked locally** after `FOTA_MAX_ATTEMPTS` (2),
and the block is version-scoped: the `fota=<ver>` advert that rides on every
response stops meaning anything for that version, so no manifest is fetched
either, while a newer version is still picked up automatically.
- **Two new lines tell the server what happened**: `F,fota,staged,<new>,
<old>` before the reboot, and `F,fota,failed,<staged>,<running>` from the
boot that finds itself running the old image.  Sent as their own datagrams
like the DTC report, queued until there is a link.
- **`status=blocked` in the manifest is understood** as the server
withholding an image from this device, with an optional `reason=`.  Logged
once, not retried, and scoped to that version.
- **The bare `fota` command is the manual retry**: it clears the local
block, the attempt record and the failure holdoff, so an operator can force
another go at a version the device has given up on.

## 0.4.37

### The tracker stopped tracking mid-journey on a car with charging control
- **The vehicle sheds its alternator on purpose, and that read as "engine
off".**  Once the battery is topped up this car's ECU cuts alternator output
to save drag, and two days of logs show the rail sitting at 12.2-12.5 V for
up to 140 s at a stretch — at any engine speed, including 2000-2500 rpm —
before climbing back to 14.3 V on overrun.  Parked, the same battery rests
at 12.0-13.1 V.  The driving-with-charge-cut band therefore overlaps the
engine-off band and is frequently *below* it, so no voltage threshold
separates the two.  `ENGINE_RUNNING_VOLTAGE` (13 V) was being crossed dozens
of times per drive and each crossing dropped the unit to its engine-off
cadence in the middle of a journey.
- **The debounce that was supposed to cover this was not in the path.**
`engine_is_running()` was a bare compare with no hysteresis at all, and the
`ENGINE_STOPPED_COUNT` counter lived only in `data.c`'s sampler — so
`main.c` demoted on a single sub-13 V reading and never consulted it.  Both
call sites now go through one function, which is the only place the voltage
verdict is formed.
- **Demotion needs corroboration, promotion does not.**  Nothing but an
alternator puts the rail above `ENGINE_RUNNING_VOLTAGE`, so voltage still
promotes on its own.  Going the other way now needs the rail low *and* the
vehicle standing still *and* both sustained for `ENGINE_STOPPED_HOLD_S`
(300 s); any GNSS speed above `ENGINE_MOVING_KMH` restarts the hold, so a
charge-cut episode cannot accumulate towards a stop however long it runs.
Key-on-engine-off while rolling — a tow, a coast — holds the running
cadence, which is the side to err on for a tracker.  Replaying both versions
over the logged drives: 39 false "engine stopped" calls mid-drive, now none.
- **Unchanged where the ECU answers.**  A fresh RPM figure still settles it
outright, so this only bites builds with no K wire, no session, or a reading
gone stale.  `ENGINE_STOPPED_COUNT` is replaced by `ENGINE_STOPPED_HOLD_S`,
`ENGINE_MOVING_KMH` and `ENGINE_FIX_MAX_AGE_S` — a wall-clock hold rather
than a sample count whose real duration depended on which state was polling.
- **The averaging in `battery_read_voltage()` was miscredited.**  Its
comments claimed it existed to suppress "~12 V readings mid-drive on a car
with no charging fault".  Those readings are real and no amount of averaging
removes them.  The eight-conversion average stays — it rejects alternator
ripple, worth a few tens of mV — and the comments now say so.

## 0.4.36

### A fatal error halted the unit instead of rebooting it
- **The firmware defines its own `k_sys_fatal_error_handler()`.**  Zephyr's
default calls `arch_system_halt()`, which locks interrupts and spins
forever, and `CONFIG_RESET_ON_FATAL_ERROR` (Nordic's opt-in reboot handler)
was not enabled.  So any fatal error anywhere — bus fault, failed assert,
stack overflow, a fault in any thread — killed the whole device: no
telemetry, no console, no ignition wake and no accelerometer alerts, because
interrupts were off, and nothing short of pulling power recovered it.  That
is the state a unit was found in after a drive on 2026-09-12, dead for hours
with the key having been cycled several times.
- **The handler leaves a note before rebooting.**  Reason, PC and LR go into
a `__noinit` struct that survives the warm reset, and dbglog folds them into
the `rst=` field of the next record that reaches the server:
`rst=sw+fatal:4@0x2a1c8` instead of a bare `rst=sw` that reads like a
commanded reboot.  Enough to place the fault in the map file.

### The watchdog is armed at the top of main(), not the end of bring-up
- **Arming it last caused a FOTA revert loop.**  The nRF watchdog cannot be
stopped once started and is not cleared by a soft reset, so after any
reboot — a FOTA swap, a fatal error — the previous image's watchdog is
still counting down its 12 s window while the new image boots.  With
`watchdog_init()` at the end of bring-up, behind the modem connect and the
A-GNSS fetch, the app did not take ownership until about eleven seconds in
and the watchdog fired first, at the same point every boot.  A swapped-in
image therefore never reached `fota_confirm_image()`, MCUboot reverted it,
the old image downloaded it again, and the unit spent 300 KB a cycle going
nowhere.  Armed at the top of `main()` the app takes ownership within a
couple of hundred milliseconds — task_wdt allocates the same hardware
channel, so the reload feeds the running watchdog — and every slow step in
bring-up already kicks as it waits.
- **The reset cause is logged at boot and cleared there.**  RESETREAS bits
are sticky until something reads them, and until now that only happened
when the first telemetry record was built.  A unit resetting during
bring-up never got that far, so the bits accumulated and a reading of
`sw+wdt` could not distinguish "rebooted, then watchdogged" from "one of
these happened three boots ago".  Reading it at boot makes each line the
cause of that one reset.  A unit already in a watchdog loop needs a pin
reset or a power cycle, which is what stops the watchdog.

### The watchdog was never armed
- **`task_wdt_init()` is given the hardware watchdog, and the timeout
callback lets it fire.**  Neither layer was active: Zephyr only configures
the hardware fallback when `task_wdt_init()` is passed a device, and this
passed NULL, so `CONFIG_TASK_WDT_HW_FALLBACK=y` did nothing; and task_wdt
only reboots by itself for a channel registered with no callback, so the
empty callback replaced that reboot with nothing.  A thread that stopped
feeding was therefore never noticed — which is how a unit that blocked
inside a modem call sat dead in a car for over an hour, ignoring the
ignition and the accelerometer, until it was power-cycled by hand.  The
callback deliberately does not call `sys_reboot()`: leaving the hardware
watchdog unfed resets the SoC a couple of seconds later and RESETREAS then
records a watchdog reset, so the next record carries `rst=wdt` rather than
the `rst=sw` of an ordinary commanded reboot.
- **`CONFIG_TASK_WDT_MIN_TIMEOUT` 100 ms -> 10 s.**  That symbol is how
often the background timer feeds the hardware watchdog, so with the fallback
finally armed it is also how often the CPU wakes during an engine-off sleep.
The default would have been ten wakes a second for the whole sleep; at 10 s
(the maximum the symbol allows) the cost is a rounding error against 134 uA.
- **The sleep wait is sliced.**  An engine-off sleep waits up to an hour in
one `k_sem_take`, far past the 32 s window, so it now waits in 20 s slices
and feeds between them.  A slice that expires does not take the semaphore,
so wake behaviour is unchanged.  Two operations that cannot be sliced — the
A-GNSS REST fetch and the modem's DNS lookup — are bracketed with kicks
instead, and the A-GNSS request timeout is 20 s rather than 30 s so that a
full timeout still fits inside the window.

### A modem crash recovers itself
- **`CONFIG_NRF_MODEM_LIB_ON_FAULT_RESET_MODEM=y`** replaces the
`DO_NOTHING` default.  A modem fault was logged and then ignored, and every
modem call returned `-NRF_ESHUTDOWN` from then on: the unit stayed awake,
fed and tracking GNSS, but mute.  Whether it ever came back depended on
`s_connected` still reading true at the moment of the crash — if it did,
`modem_recover()` reached its 30-minute restart branch eventually; if it
did not, the escalation timer never started and nothing recovered it short
of a reboot.  The library's reset thread now reinitialises within a second,
and unlike a reboot it keeps the databuf backlog.
- **`CONFIG_NRF_MODEM_LIB_FAULT_STRERROR=y`** so the fault line names the
reason (`NRF_MODEM_FAULT_BUS`) instead of printing a bare `0x4`.  It is an
ERR, so dbglog carries it to the server.
- **STATE_IDLE brings the radio back up.**  A reinitialised modem comes back
at CFUN=0 with no APN, no `%REL14FEAT` and no `%RAI`, and nothing awake put
those back — so the fix above on its own would have traded "mute until
reboot" for "mute until the next key cycle".  While unregistered, the idle
poll now calls `modem_radio_up()` (settings + CFUN=1, no wait) every
`APP_NETWORK_RETRY_INTERVAL`, which until now was defined and referenced by
nothing.  Not a blocking connect: the one-second poll stays in charge, so
the ignition line and the K-wire keep-alive keep being serviced.

### A late reply no longer costs the socket
- **A response that fails the tag check is skipped, not treated as the end
of the exchange.**  The response AAD binds a reply to the request nonce,
which is fresh random bytes per send, so a reply the server sent to an
earlier datagram cannot authenticate against the current one — by design.
Track mode already knew that and kept reading; the normal path did not, and
logged `psa_aead_decrypt: -149` / `response decrypt failed: -13` and closed
the socket instead, so the next send had to build a new RRC connection,
which made the next reply later still.  Both paths now read on until the
caller's deadline and report the skipped count with the fresh reply (INF) or
with the failure (WRN), which also distinguishes "the reply was late" from
"the only reply we got would not decrypt".  Nothing that fails to
authenticate is ever acted on; this only changes how long the socket
listens.
- **The response window is 4 s rather than 2 s** (`RESPONSE_TIMEOUT_MS`).
RAI releases the radio after each datagram, so a reply arrives after an
idle-to-connected transition and possibly a paging cycle, which two seconds
does not reliably cover on LTE-M.  Only paid when a reply is late or lost,
and only on the sends that ask for one.

### Modem recovery no longer blocks the whole firmware
- **`lte_lc_connect()` is gone from `modem_connect()` and
`modem_recover()`.**  It ends in a semaphore take of up to
`CONFIG_LTE_NETWORK_TIMEOUT` — 600 s by default, and this build never
overrode it.  Both escalation branches called it, so a unit that reached the
CFUN cycle or the modem restart in bad coverage stopped doing everything
else for up to ten minutes at a time: no ignition read, no accelerometer
service, no K-line keep-alive, no console output, and no watchdog kick.
From outside it was indistinguishable from a dead unit.
- Recovery now brings the radio up with `lte_lc_normal()` and returns.
STATE_IDLE already polls registration once a second while servicing
everything else, and records an ignition change that happens during the
outage, so waiting inside the recovery call bought nothing.
`modem_connect()` waits with the same polled helper `modem_rescan_plmn()`
uses, bounded by `APP_NETWORK_REGISTRATION_TIMEOUT` (60 s) and feeding the
watchdog each second, and leaves the radio searching when it gives up.

## 0.4.32

### Batch size is a build-time option
- **`CONFIG_APP_BATCH_SIZE` (default 3, range 1-16) replaces the hardcoded
`BATCH_SIZE` in `config.h`.**  Every other cadence and threshold in that
header already comes from Kconfig, so the one knob that decides how many
records ride in a datagram was the odd one out: changing it for a bench run
or a per-device build in `remote.conf` meant editing tracked source.  The
trade-off it sets is unchanged - a larger batch raises the record rate
(0.33/s at 1, ~0.6/s at 3) because each send costs an RRC connection and a
GNSS re-acquisition, at the price of the page updating every N records
instead of every cycle.  Values above three mostly hand the decision to
`BATCH_FLUSH_BYTES`, which flushes a datagram before it can overflow;
ignition changes, settings syncs and send failures still flush at once.

## 0.4.31

### Rail-fault alerts name the rail, not the domain
- **`RAIL:<net> fail` replaces `RAIL:<domain> rail fail`.**  K_EN switches two
rails (PP3V3_K and PP12V_K) and CAN_EN one, but the alert only carried the
domain name, so a K-line fault said `RAIL:K rail fail` with no way to tell
the 3.3V shifter supply from the 12V K output.  Each sense line now carries
its schematic net name, and the alert and the error log list the sense
line(s) that were not up when the timeout expired: `RAIL:PP12V_K fail`, or
`RAIL:PP3V3_K+PP12V_K fail` when both are down.  A domain with no named
senses still falls back to the domain name.

### Network mode is LTE-M only
- **`CONFIG_LTE_NETWORK_MODE_LTE_M_GPS` replaces
`CONFIG_LTE_NETWORK_MODE_LTE_M_NBIOT_GPS`.**  NB-IoT is no longer enabled in
`%XSYSTEMMODE`, so the modem will not fall back to it when LTE-M is
unreachable.  Registration has one RAT to scan instead of two; the cost is
that a site with no LTE-M coverage now has nothing to fall back to.  GNSS is
unaffected and stays enabled.
- **`CONFIG_LTE_MODE_PREFERENCE_LTE_M` dropped.**  Every
`CONFIG_LTE_MODE_PREFERENCE_*` symbol depends on one of the two dual-mode
network choices, so keeping it alongside a single-RAT mode is an unmet
dependency, and Zephyr's warnings-as-errors turns that into a failed build.
lte_link_control now sends preference 0 (auto), the value `%XSYSTEMMODE`
takes when one RAT is enabled.
- **Set in `remote.conf.example`'s `[common]` as well as `prj.conf`.**  A
per-device build layers only `[common]` and the IMEI section over `prj.conf`,
so that is where a fleet-wide RAT change belongs.

### Registration outages: visible, and the key-off record no longer waits
- The 16-minute silence on 2026-09-12 (08:51-09:07) was the modem losing
LTE-M and searching until it found NB-IoT.  Registration changes were logged
at INF, so the captured log showed nothing, and STATE_IDLE waits for
registration before it looks at anything else, so the ignition-off record
was built when the network came back rather than when the key turned: right
position, 15 minutes late.  Losing and regaining registration now logs at
WRN with the outage length, and the idle wait records an ignition change
straight away from the current fix (or last known position) and sends it
the moment registration returns instead of collecting it again.  The wait
also polls every second rather than every five, so the K-line keep-alive
stays inside P3max and the session is not dropped each time round.
- **"dbglog: N lines dropped" every datagram** was the deferred log core
overflowing its 1 KB buffer on every send (three ~300-byte record lines
logged back to back, and the log thread only wakes on a 1 s timer or ten
queued messages).  That count is global and level-blind, so the marker was
reporting lost console INF lines as if they were lost warnings.  The buffer
is now 4 KB with a wake threshold of 3, and dbglog keeps the core's count
apart from its own, reported as "console lines dropped (log buffer
overflow)".  Bench builds with `CONFIG_LOG_MODE_IMMEDIATE=y` warn that the
two settings have no effect; that is expected.

### IF MCU patch dropped - both fixes are upstream
- makerdiary/nrf9151-connectkit#20, the follow-up that routes every IF MCU
power-off path through the SEVONPEND-clearing helper, merged on 2026-09-10.
`ifmcu/patches/` is gone and `ifmcu/build.sh` builds the Makerdiary clone
unmodified; it now refuses a checkout that predates #20 rather than one that
merely predates #19. Neither PR is in a published Makerdiary release (v2.0.0
is from June 2026), so Connect Kits still need the IF MCU firmware built from
current `main` and flashed once by hand, as QUICKSTART.md describes. An
existing clone with the old patch applied needs `git checkout -- .` before
`git pull`.

### L-line sense streamer (bench)
- **`CONFIG_APP_L_SENSE_TEST`** (v3.3+ only) samples L_SENSE at 5 Hz from
boot and prints each reading in millivolts with its LOW/HIGH classification
against `CONFIG_APP_L_SENSE_LOW_MV`, marking transitions.  It opens by
asserting L_SEND for 2 s so the pulldown and the sense can be verified
against each other, then releases the FET and only observes the wire.
Loops forever like the accel and voltage streamers.
- **`CONFIG_APP_L_SENSE_LOW_MV` default raised from 1500 to 2800.**  The
SAADC pull-up ladder measures ~65K to VDD on v3.3, not the ~400K the sense
code assumed, so a grounded L wire reads ~2.0 V rather than 0.7-0.9 V and
the old threshold never classified it LOW.  A high, open or shorted line
still pins at the 3.6 V full scale; 2800 sits midway.  This also fixes
`kline_l_line_probe()`, which would have reported every healthy L wire as
shorted to battery.

### GPS rail fault no longer switches the bias tee off
- **A GPS rail that fails its rail-sense check at power-up now stays
enabled.**  `hw_domain_request()` used to treat every sensed rail the same:
if the sense line did not report the rail up within 50 ms it parked the
domain's pins and drove the enable low, which for the AUX/GPS domain meant
no antenna bias for the whole session.  That protection exists to stop the
nRF backfeeding a dead rail through a peripheral's clamp diodes, and on
v3.x no signal pin terminates in the GPS domain, so there is nothing to
protect.  A pin-less domain now keeps GPS_ENABLE high through the fault,
counts as on, and returns success; the `RAIL:AUX rail fail` alert and log
line are unchanged.  Domains with signal pins (CAN, K) behave as before.
- `hw_domain_faulted()` exposes the sticky fault, and the boot self-test
uses it to report the GPS rail without queueing a second alert.

## 0.4.26

### Faster telemetry while driving: one poll shape, records batched three to a datagram
- **The K-wire poll is the rotating one everywhere.**  RPM, speed, throttle
and load every call and one slow PID in turn, four or five exchanges instead
of thirteen, merged into a live snapshot; a slow value keeps its last
reading for up to 30 s.  The GNSS fix-wait tick now runs that poll (it
sampled RPM alone before), so the record built after the fix takes the
snapshot and normally costs no bus time; it polls itself only if the
snapshot is more than a second old.  The tick still never opens or reopens
a session.
- **`BATCH_SIZE` is 3.**  Each send costs an RRC connection and, because
GNSS and LTE share the antenna, a fix re-acquisition; with a record now
costing about a second, three per datagram roughly doubles the record rate
(~0.6/s from ~0.33/s) for a page update every five seconds or so.  Ignition
changes, settings syncs and send failures still flush at once.
- **The batch flushes against the datagram cap**, not the record buffer:
`BATCH_FLUSH_BYTES` leaves room for one more record plus log lines under
`UDP_PACKET_SIZE`.  The old threshold sat above what the transport can
send, which a larger batch would have hit as `-EMSGSIZE`.
- Track mode defaults move to a steady one record a second
(`APP_TRACK_PERIOD_MS` 1000) carrying the full 26 Hz IMU burst
(`APP_TRACK_IMU_SAMPLES` 24); 500 ms still works for two a second.

### Sleep-state power: wake interrupts moved off GPIOTE IN channels
- The two sleep wake sources (ignition sense and the accelerometer's INT1)
were edge-triggered, which on the nRF91 allocates a GPIOTE IN channel and
keeps the pin-detect logic clocked for the whole sleep, ~20-45 uA on the SiP
by Nordic's figures.  Both are now level-triggered (`GPIO_INT_LEVEL_LOW` on
the active-low ignition sense, `GPIO_INT_LEVEL_HIGH` on INT1), which routes
through the PORT/sense path and costs nothing while waiting.  Detection is
unchanged: sleep is only ever entered with the ignition off, so the one
transition that can matter is the sense pin going low, and INT1 is an
active-high pulse that idles low.  nrfx re-arms a level trigger after every
callback while the level persists, so both ISRs now disarm themselves and
the sleep loop re-arms the ignition wake before each wait (after the
semaphore reset, so a level already present is not swallowed).  The
awake-side impact trigger keeps its edge configuration.
- Measured on a v3.2 board across a 10K series resistor: sleep current down
from ~140.8 µA to 134 µA.

## 0.4.24 - track mode and minor bugfix

### Track mode (`APP_TRACK_MODE`, [TRACK_MODE.md](TRACK_MODE.md))
- **A server-side switch that turns the tracker into a live data logger.**
While it is on and the ignition is on, GNSS is stopped and a record goes
out every ~500 ms with the ECU's fast PIDs (RPM, speed, throttle, load every
cycle, the slow ones one per cycle in rotation, so the K wire sees four or
five exchanges a cycle rather than thirteen) and a burst of up to sixteen
IMU samples drained from the sensor's 26 Hz FIFO (`acc=`).  The transport
holds the socket and keeps the RRC connection up between sends
(`RAI_ONGOING`) instead of releasing the radio after each one.  Key-off
ends it with the usual final record; a switch-off from the server resumes
GNSS and the normal state machine.
- **The switch rides on every server response** as `track=<0|1>` beside
`fota=`, so a rebooted device converges on it.  The firmware acts only on a
change.  Records built in the mode carry `tm=1`.
- **The mode ends with the drive.**  The server clears the switch on the
ignition-off record, and after an hour of silence from a device that never
sent one; the firmware drops the mode on ignition-off and ignores a
`track=1` that arrives with the ignition off.  A switch left on by a page
that was never revisited no longer puts the next drive, or a device
rebooting after an update, into the mode.
- **Driving devices now read a server reply at least every
`APP_RESP_POLL_S` (30 s).**  Before, a send while moving never waited for
one, so a setting changed on the server did not reach the device until it
stopped.
- Server: `device.track_mode`, `log.track_mode`, `log.imu_burst`
(migration in `remote_tracker/sql/`), `/api/1.0/trackmode`, the live
stream sends every row and polls at 250 ms in the mode.  The tracking page
gains a **track** toggle and, while on, replaces the map with a dashboard:
RPM and redline, speed, throttle and load bars, a friction circle with a
learnt forward axis, a 60 s strip chart, and the slow values as tiles.

### No low-battery alert from a crank
- **The low-battery warning waits for the ignition to have been off for
`BATTERY_WARN_SETTLE_S` (60 s) and re-checks the line before alerting.**
Cranking pulls the rail to 9-10 V for a second or two, and the ignition
sense can read "off" for a moment inside that sag; the firmware then built
an ignition-off record at the bottom of it and raised "low battery: 9.8V"
on a healthy car.  A flat battery is still flat a minute later, so nothing
real is delayed by more than that.

## 0.4.22

### Warnings and errors reach the server (`src/dbglog.c`, `APP_DEBUG_LOG`)
- **Every WRN/ERR log line is kept in RAM and sent with the next telemetry
record that gets through.**  A deployed unit has no console, so until now a
stall in telemetry left nothing to read but the gap.  A second log backend
captures the lines (4 KB static: a 1 KB head that keeps the onset of an
incident and a 3 KB tail that keeps its most recent lines, evictions
counted), and `send_data()` appends as many as fit beside the record as
`L,<uptime_ms>,<E|W>,<module>: <text>` lines, at most
`APP_DEBUG_LOG_PER_RECORD` bytes per record.  They are freed only once the
datagram has left, so an outage's lines wait it out and arrive with the
first record after recovery.  Consecutive identical lines are collapsed into
one plus a "repeated N times" marker.  Nothing goes on the air while the log
is quiet.
- **The reset cause is reported on the first record after boot** as
`rst=<cause>` (`wdt`, `sw`, `pin`, `por`, `bor`, `lockup`, ...), read from
RESETREAS and then cleared so each boot reports only its own.  The server
already understood the field; the firmware never sent it.
- **Known-benign lines are not sent.**  `GNSS blocked by LTE` and the
A-GNSS `JWT needs modem time` wait were logged at WRN but are routine, and
now log at INF.  Lines the firmware does not own are excluded by a short
table in `dbglog.c` (module name plus message prefix): nrf_cloud's
once-a-second `Modem does not have valid date/time` during that wait, and
the modem's ERROR reply to `AT%REL14FEAT` on every boot.  The console still
shows all of them.
- Server: `L,` lines are appended on receipt to `/var/log/tracker/device.log`,
each stamped with the wall-clock time the device logged it (derived from the
`up=` field of the record they arrived with, or the receipt time marked
`(rx)` when there is none) plus the raw device uptime.

### An update announces itself before the download, not only after it
- **`fota: x -> y available, downloading` is sent as the transfer starts**,
standalone, once every gate (version, board, battery, NB-IoT) has passed.  A
multi-minute download followed by a reboot was otherwise a silent gap in
telemetry, and one that never completed showed nothing until the failure
alert.  Raised once per advertised version, like that failure alert, so a
download retried on later wakes does not repeat it.  The socket the send
brings up is closed again before the download opens its own.

### Battery voltage is averaged, and the ECU's RPM decides "engine running"
- **Every battery read now averages 8 INA228 conversions** 3 ms apart instead
of trusting one.  Alternator ripple and ignition noise on the 12 V rail can
drop a single ~1 ms conversion a volt or so low, which is how a car with no
charging fault reported 12.01 V while being driven — and the engine-running
logic took that at face value.  Bus and shunt conversions run back to back
(~2.1 ms per cycle), so the spacing guarantees each sample is a fresh
conversion.  A NACK drops that sample rather than failing the read; a read
only returns -1 when no sample succeeded.  When the samples span 0.5 V or
more the min, max and average are logged (`VBUS noisy: ...`) as evidence.
- **A fresh RPM figure from the ECU now overrides the voltage proxy
everywhere.**  The main loop already preferred `obd_rpm()`; the telemetry
builder in `data.c` still set `engine_running` from voltage alone every
record, so on K-wire builds the two disagreed whenever the rail dipped.
`engine_is_running()` has moved to `data.c` and is exported, and the
per-record check follows the same precedence: RPM > 0 is running, RPM 0 is
stopped, and only with no fresh RPM (no K wire, no session, stale reading)
does the 13 V threshold with its 10-reading hysteresis apply.

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
