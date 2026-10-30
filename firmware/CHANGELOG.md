# Changelog

## Unreleased

### A radio the network holds no longer costs a restart or a reset
On 2026-10-24, driving to Cornwall, the network held the car's radio for up to
347 s after a send on EE's rural cells in Somerset, Devon and Cornwall
(tracking areas 26371 to 26771): the modem stayed RRC connected with nothing
to send, GNSS saw no satellites, and 18 gaps ran past two minutes, 17 of them
on EE.  That is the carrier's, and nothing here shortens it.  What the
firmware did while the radio was held was its own.  A fix wait that timed out
restarted the receiver, 64 times in the day and 60 of them with the radio
held, and each restart made the next wait cold; a cold wait asks nRF Cloud for
assistance 15 s in, with the radio still held.  At 19:30 the name lookup of
one of those fetches sat on its query for 38 s.  The REST client compares its
20 s timeout with the clock only after `getaddrinfo()` has returned, so the
lookup was never bounded: the main loop went past the 32 s watchdog window
("no kick for 32s"), and the call returned about four seconds before the
hardware reset.

- **The A-GNSS name lookup has a deadline.**  `agnss_fetch()` asks it first,
from a thread of its own (1.5 KB of stack, about 600 bytes used), and waits in
kicked one-second slices for `AGNSS_DNS_WAIT_MS` (10 s).  On a miss the
attempt is dropped before the receiver has been paused; the thread finishes
its query in its own time, and an attempt made meanwhile fails at once with
`-EBUSY`.  The boot-time fetch has the same bound.
- **The retry interval counts time the radio was free.**  `gnss_collect()`
asks nRF Cloud 15 s into a cold wait; those 15 s now restart while the network
holds an RRC connection, read from the LTE events (`modem_rrc_connected()`).
The receiver's own blocked events are no use for this: they are edges, and a
stop and start with the radio still held, which a fetch is, raises none.  That
is also why `gap_warn` read "GNSS blocked 78 s, RRC connected 238 s" at 17:48.
- **A fix wait the network held is not restarted.**  When the radio was held
for at least half the wait and still is, the receiver is left running instead,
up to `GNSS_KEEP_MAX` (3) times in a row, and `radio held by LTE for 30 of 30
s, GNSS left running` is logged.  A restart cannot give the receiver the radio.
After the third it restarts as before, in case the receiver has stopped
hearing the radio's events.
- On the bench, with the radio held by a packet a second to a DNS server, a
lookup made to stall, and the receiver asking for assistance: the 0.4.79 logic
restarted the receiver 30 s into the hold, fetched 15 s after that, tripped
the watchdog 32 s later and reset ten seconds after that.  This build gave up
on a stalled lookup after 10.1 s with the receiver untouched, refused a second
attempt in 0.14 s, and in a 150 s hold left the receiver running at 30, 60 and
90 s, restarted it at 120 s, made no fetch, and had a fix 12 s after the
network let go.  The hold itself is the carrier's and was not reproduced on the
road.  The car has not been updated.

### LTE can be taken away from a network that will not release the radio
What the entry above leaves alone is the hold itself.  On EE's rural cells the
radio stayed RRC connected for up to 347 s after a send, 71 % of the trip's
blind time, and GNSS saw nothing for all of it.  Release assistance does not
move those cells, and GNSS priority mode cannot: Nordic's header says it "will
not stop LTE connections".  `AT+CFUN=4` takes the receiver down with LTE and it
does not come back.  `AT+CFUN=20` turns LTE off and leaves the receiver alone,
and `AT+CFUN=21` turns it back on.  On the bench that cycle (off, register,
one send, off again) left the receiver blind for 3.3 to 6.5 s, 12.8 s once
across an O2 to Vodafone change, against 14 to 20 s for every send on the
Vodafone cells there.  On O2, where the network lets go after 2.8 s by itself,
it gains nothing, so it only acts on a connection that has outlived that.

`CONFIG_APP_HOLD_BREAK` (default n, and not with Traccar) does it while the
vehicle is on the move.  The fix wait runs a probe once a second while no fixes
arrive: with the connection still up `APP_HOLD_BREAK_PROBE_S` (5 s) after our
last datagram, at `APP_HOLD_BREAK_MIN_KMH` (10) or more, the replies due are
taken in, anything unanswered goes back to the backlog, the socket is closed
and LTE is switched off.  The receiver then fixes at 1 Hz, and what
`STATE_SEND` would have sent is pushed to the backlog and the next batch
collected, with the receiver left running.  After `APP_HOLD_BREAK_WINDOW_S`
(20 s), at the next batch, LTE is switched on and given `APP_HOLD_BREAK_REG_S`
(15 s) to register, then the backlog goes out oldest first and the batch after
it, so the server meets the track in the order it was driven.  The next
`APP_HOLD_BREAK_STICKY` (4) sends cut LTE straight after their replies instead
of waiting for the probe; the one after that waits for the network, so a unit
that has driven out of the holding cells goes back to ordinary operation by
itself.

Nothing is lost to a cut and nothing waits on it for long.  The loop ends it
early for anything that cannot wait: the key off, the engine stopping, a stop
(below half the speed limit), track mode, an alert or an impact to send, a
setting to report, a failed send, and a backlog three-quarters full.  A restore
that does not register in time leaves the radio searching, as in any outage,
and switches the feature off until the modem has registered again by itself and
for five minutes after that; a modem that refuses `CFUN=20` switches it off for
the boot.  While LTE is cut `transport_send()` and the A-GNSS fetch refuse at
once, STATE_IDLE does not count it as an outage, and the PSM grant the modem
gives up and gets back is not reported each time.  `modem_radio_up()` and
`modem_power_off()` also end a cut.

What it costs is a registration per cycle (2.8 to 5 s on the bench) and a
track that reaches the server in bursts every 20-odd seconds while a cut runs,
rather than every three.  Records parked for ten seconds or more carry `age=`,
as backlog always has.  A WRN line at each probe-cut says where, for how long
the connection had been held, and the cycles, failures and re-attach times so
far, so a drive shows what it did.

The first drives on the car (2026-10-25, 0.4.80 and then 0.4.81, which gives
the registration 30 s in its `remote.conf` instead of 15 because one re-attach
took 15 s) showed it doing what it is for: of about 45 restores on 0.4.81, 44
registered in about 3 s on average, and the share of the moving time spent in
fix gaps of 20 s or more fell from 86 % on the same cells the day before to
14 %.  The 45th never registered.  LTE was off for 21 s and then neither it,
nor an offline/online cycle, nor NB-IoT found the network for ten minutes, at a
spot that read -87 to -101 dBm before and after; the car was silent for 42
minutes (telemetry stalled) and had no fix for 97 s while the search starved
the receiver, and the log could not say whether the modem had found a cell and
been refused or had found none.  So a restore that does not register now keeps
the feature off until the modem has registered again by itself and for five
minutes after that (`hb_registered()`, `HOLD_BREAK_HOLDOFF_MS`; parking clears
it, `hb_new_trip()` from `do_sleep()`), where it used to try again after ten
minutes whether or not the network had come back, and says what the modem makes
of it.  A cut needs a registration, so staying off while the modem is
unregistered holds nothing back, and a dead zone ends when the vehicle leaves
it; the five minutes are for the case where the cycling had a hand in it, which
the log will show.  The registration status changes since the `CFUN=21` are
logged with their times when the cap is reached, and each change after it while
the search lasts (at most 20).  The function mode, the registration status with
the reject cause if it was refused, the RRC state and the signal (`CFUN`,
`CEREG`, `CSCON`, `CESQ` on one `hold break:` line) are logged 5 s and 15 s
into the wait, at the cap, and every 30 s after it (at most 20 times, with the
key off: from the main loop and from the sleep loop once parked) for as long as
the radio is still searching.  255 in the last two `CESQ` fields is no cell at all;
a figure there is a cell that is not letting the unit in.

Off by default because it is new.  The rules are in `holdbreak.c` with 39 host
tests (`firmware/tests/test_holdbreak.py`), the car profile builds without
application warnings, and the car has run it, but the re-attach time at speed
on a long drive over EE's rural cells and the cause of that one failure are not
known yet.  The car's `remote.conf` turns it on.  With the option off the
application image is byte-identical to one built without this code (compared
for the car's and the bench's builds, the signature on the signed image aside),
so nothing else changes.

### A watchdog reset says where the hold breaker was
At 13:10 on 2026-10-26 the car cut LTE on a drive and was reset by the watchdog
about 130 s later (`rst=wdt` on the first record after the boot; the last
record before it is the cut's, the next is 201 s on, with no row between and
about 1.3 km of the route missing).  The log lines that would have said where it
was were in RAM, and no stall address was captured, nor in the car's three
earlier watchdog resets: the software callback that records them probably
never ran, and the hardware fallback fired on a wedge below thread level.  Only
a note written to RAM before the reset needs nothing to run after it.

- **The breaker keeps a note of where it is** (`crumb.c`, 20 bytes of noinit
RAM): the phase, when the cycle began, when the phase began, and when the main
loop last kicked the watchdog.  The phases (`holdbreak.h`) are `C` cutting
(from the decision to the answer to `CFUN=20`), `O` LTE off, `R` restoring
(`CFUN=21` and the wait for a registration), `Q` the four AT queries of the
search report, which have no timeout of their own and now say so in the note,
`F` the restore did not register, and `D` delivering the backlog; none is no
cycle under way.  `watchdog_kick()` is the sign of life, so the last kick says
when the loop stopped and the phase says what it had last begun.
- **The next boot reports it after a reset nobody asked for**, in `rst=`:
`wdt+hb:Q98/0` is a watchdog reset in phase `Q`, last alive 98 s after the cut
and 0 s into the phase.  Only for a watchdog reset, a CPU lockup or a crash: a
FOTA install or any commanded reboot reports nothing, and memory that was never
written (a power-on) fails the note's magic and reads as nothing.  The note is
taken in `SYS_INIT(APPLICATION)`, before `main()`, so no kick of the new boot
writes over the old one's time.
- **`rst=` is kept to 47 characters.**  The server's column is `varchar(48)` in
strict mode: a longer value is refused, and the record with it, which is the
first after a boot and carries `fw=`.  Nothing capped it before: `pin+sw+wdt+dbg`
and four stall addresses already come to 53.  With the breaker built in, the
parts are fitted by `hb_rst_compose()` in the order of what each is worth: the
cause (cut short if it must be), the crumb (whole or not at all), then as many
whole stall addresses as fit.  Without it `rst=` is assembled as before.

The field's format and the budget are in `holdbreak.c`, host-tested with 14 new
cases (53 in `firmware/tests/test_holdbreak.py`).  With the option off the
application image is byte-identical to one built without this code (the sha256
of `zephyr.bin` is the same before and after); with it on, the car profile
builds without application warnings, 0.9 KB larger (flash 74.50 % of the
application partition, RAM unchanged at 53.58 %), and the note lands in
`.noinit` with the watchdog's and the crash handler's.  Not run on hardware:
the first real test is the next watchdog reset on a drive.

### A reset on the road no longer starts an update
At 14:37 on 2026-10-26 the car announced `fota: 0.4.83 -> 0.4.84 available,
downloading` while it was being driven, took 88 s over the download and
rebooted: no record exists from 14:32:28 to 14:40:16 (7 min 48 s), and the
reboot that installed it came on top of whatever had gone wrong first.  The
first manifest fetch was 8.2 s after the attach and before any telemetry, which
is what a boot looks like (the car's other boots fetch 7.9 to 10.5 s after
their first attach; 180 ordinary breaker re-attaches sent telemetry first, a
median of 1.1 s later), so the unit most likely reset about 305 s after the
cut.  Why is not known: nothing from that boot survived the install reboot.
What the boot did next is this code's: the power-on check is the one
unconditional update check and, unlike every check that follows a server
advert, nothing in `main()` held it back for the engine.

- **The power-on check waits for key-off.**  `fota_check()` does nothing and
leaves the check pending while that is the only one waiting and the ignition
line is on (`ignition_read()`, in `fota_check()` so every call site is covered
and not only the one in `main()`).  The first call after key-off makes it: the
key-off site, or a timed wake.  A newer version advertised by the server is as
before, serviced where the engine is not running.  A unit that is switched on
with the key off, or flashed on the bench with it off, checks at power-on as it
did.  Applies to every build with updates on, not only the car's.

Not run on hardware (the next reset on a drive, or a boot with the key on, is
the test: `key on — the power-on update check waits for key-off` in the log).

### The search report waits for the key to go off
Both resets on the road on 2026-10-26 (13:10, and 14:37 as above) came with the
modem's search report running: four AT queries (`CFUN?`, `CEREG?`, `CSCON?`,
`CESQ`) every 30 s while a restore that did not register leaves the radio
searching, none of them with a timeout of its own.  Parked, the same report ran
21 times in a row without trouble (10:55).  The car has also had watchdog resets
with no breaker in the firmware (18 Sep, 11 Oct, 13 Oct), so this is a lead and
not a finding; the breadcrumb above will say if the next reset is in phase `Q`.

- **Every 30 s only with the key off** (`hb_watch_due()`).  With the key on the
reports are held back and are not used up, and a drive does not leave them all
due at once when it ends: the first comes within 30 s of key-off and the rest
30 s apart.  The reports at 5 s, 15 s and the cap, inside the restore, are
unchanged: they say, at the moment of the failure, whether a cell is heard and
refusing the unit or there is none.

Six more host cases for the schedule (59 in `firmware/tests/test_holdbreak.py`),
each failing against a version that does not hold back, does not re-arm, or
spends the reports while the key is on.

### The hold breaker is off for the car
On 2026-10-26 at 16:50 a restore after a cut never registered, and the car went
dark for 54 minutes.  The unit was alive the whole time (no reset, the same boot
as the 0.4.85 install).  The modem could hear a cell at -105 to -100 dBm in all
11 readings and said "searching"; the network refused nothing and saw nothing:
no attach reached it from 16:50:19 to 17:44:29.  What the unit did meanwhile
changed nothing: the key-off at 16:55, an NB-IoT restart at about 16:55, a
key-on at 17:19 for the drive home (11 minutes, still nothing), the modem off
and on again.  It connected within 3.6 s at the key-on at 17:44.  Nothing was
lost, but it all arrived 54 minutes late and the drive home is five points.

It was the third failure on the road that day, after 13:10 and 14:32, and by
far the worst, and the pattern is not random.  All four times the modem did not
register after a restore (25 Oct 14:52, 26 Oct 10:55, 14:32 and 16:50) it came
after 33 to 46 reconnect bursts in the previous 20-30 minutes, and none of the
seven drives of 30 or fewer had one.  The 13:10 watchdog reset came at only 15
bursts and is a different failure.

The bench found the cause on 2026-10-27.  A rig built in a scratch copy of the
tree did nothing but the breaker's own cut and restore every 25 s, parked, on
the bench SIM (O2), with the receiver left running.  The 30th restore did not
register, after 29 clean ones in 12.5 minutes (re-attach 2.4 to 4.9 s), and the
modem searched for 47 min 29 s, hearing the cell with no reject, before it
registered by itself.  After a 5 minute hold-off it managed exactly 29 clean
cycles again and the 30th failed again, for 46 min 51 s.  1NCE logged a Create
PDP, an Update location and a Delete PDP for every cycle and nothing at all
during either search.  The car's four had 29 attaches each in the hour before,
by the same log, and the busiest stretch without a failure had 23 in 15
minutes.  So it is not EE, the road or a cell: every cut is a detach and every
restore a new attach, and the 30th in an hour is refused until an hour after
the first cut of that hour (both bench refusals ended 3601 and 3603 s after it,
so the 47 minutes was 60 less the 12.5 the cycles had taken; the car's 25 Oct
14:52 one, which came back after 43 minutes, ended about an hour after the
first attach of its burst).  That is the shape of the modem's own Radio Policy
Manager, the GSMA TS.34 signalling limit that Nordic's DevZone answers (old
nRF9160 threads) put at 30 attaches an hour.  The modem reports it as
`+CGEV: RESTR 1,2` with the seconds left and this firmware has never enabled
`+CGEREP`, so it is not confirmed, and no way to disable it is documented.
Modem firmware 2.0.4, which the bench runs, is the newest Nordic publishes and
has it, so an update will not fix this.  With the breaker off the car made no
attach at all between the 0.4.86 install and 11:20 on 27 Oct, through a 50
minute drive on the same cells.  Bringing it back means a cap that counts every
cut from the hour's first and stays well under 30.

- **`CONFIG_APP_HOLD_BREAK` is commented out of the car's remote.conf** (with the
reason beside it).  The code stays in the tree, inert when off, and the
Kconfig help says what happened.  Uncomment both lines to turn it back on.
- Not changed: the update check at power-on waits for key-off, and the rest of
0.4.85.
- Before it comes back, the log buffer needs a fix.  The ~4 KB ring filled with
the 30 s search readings by 17:01, so 19 later lines were dropped, including the
ones that would have said what the key-on at 17:19 did.

### The v4.0 board streams a record a second while the engine runs
A drive was batched three records to a datagram, each send released at once,
because the built-in receiver shares its antenna with LTE and every send cost
it a fix: about 0.6 records a second reaching the server, in groups.  The
external receiver has an antenna of its own, so on the v4.0 build
(`APP_DRIVE_STREAM`, on with `APP_BOARD_HAS_EXT_GNSS`; the car's v3.x image is
unchanged) the engine-running cycle is now one record per fix, sent as it is
built, `APP_DRIVE_STREAM_HZ` of them a second (default 1).

- **The connection is held.**  A send tells the modem more is coming
(`RAI_ONGOING`) instead of releasing after one reply, so the radio stays up
between records.  That is also what keeps the modem out of PSM while driving:
PSM is negotiated at attach and cannot be turned off under a registration (the
note in `modem.c` says as much), but the modem enters it only after the
network's 10 s active time with nothing to send, and a unit sending every
second never gets there.  A fix that does not come within
`APP_DRIVE_STREAM_FIX_WAIT_S` (5) gets a record from the last position
(dead reckoned, `cl=1`) all the same, so a tunnel does not end in silence and
then PSM.  Until a boot's first fix there is no position to use, so the stream
waits for it as an ordinary collection does and builds no record to drop.
- **Nothing is dropped from the record.**  It is the normal record: position,
speed, battery, accelerometer, gyro, temperatures, cell and signal, and the
ECU's figures from the K wire when it is polled.  The records are still held
until the server's reply comes back, and sent again if it does not
(`stream_keeps_records`, where track mode keeps none), so a lossy cell leaves
late records and not holes.
- **The K wire is polled between sends.**  The fix-wait tick that refreshed
the OBD snapshot only fires when no fix arrives within a second, which a
receiver delivering one every second never lets happen; the poll (about
half a second of bus time) would then have run inside each record's build and
pushed the cycle past the period.  `obd_sample_now()` polls right after the
send, so the next record finds the figures a fraction of a second old.
- **Nothing waits.**  No reply is waited for except across a change of ignition
state, and the 200 ms pause after a send is skipped; replies are read as they
come, which also carries the settings and commands.  The cycle is paced by
the receiver: the record is built when its fix is, so the rate cannot
exceed the receiver's.  The log is silent while the stream keeps up; when a
30 s window falls short, or leans on the last position, it says so
(`drive stream short of 1/s: 27 records in 30 s = 0.90/s (2 fixes let go by,
0 from the last position)`).  The stream's short fix wait running out is its
normal fallback, so the receiver driver no longer warns about it (it would
have, every cycle, on the console and in the server's device log, in a car
park or on a bench indoors); a wait for an ignition change still does.
- **The loop says little.**  A record a second is five more console lines each
in the old wording (`sending`, `sent`, `resp #`, `cmd: track=0`, `dead reckoning
stopped: no ECU speed 0 s after the fix`), and with `CONFIG_LOG_MODE_IMMEDIATE`
every character is UART time in the loop (about 47 ms of each second at 115200).
While streaming, `sending N records` and `sent N bytes` are not logged (a send
that takes 2 s still warns), a reply is logged only when it differs from the
last, the `track=<0|1>` the server puts on every reply is not logged as a
command when it says what the device already has (`cmd_run` already acted only
on a change), and the dead reckoning stop that came before any ECU speed was
read is said once until the ECU speaks.  What is left per record is its own
line.
- **Above 1 Hz.**  The receiver is asked for fixes at the target rate
(`gnss_set_rate_hz`; its satellite list stays at one a second) and put back to
1 Hz when the engine stops or the rail drops, and the modem's temperature and
supply, AT commands, are read once a second and not for every record.  The
bench built 3.4 to 3.6 records a second at best (2026-10-30), so 4 and 5 will
fall short.
- **Cost.**  About 300 bytes a record and a reply: 1.4 MB for an hour's
driving at 1 Hz, so 350 hours or so of a 500 MB SIM.  When the engine stops
the unit goes back to batching, with the key off to one record and sleep as
before.
- Not run on hardware yet; built and reviewed only.

### The v4.0 board's receiver gets orbits from the server (u-blox AssistNow)
A receiver that has sat unpowered for hours holds no usable ephemerides and
reads them from the satellites: 25 to 30 s with a clear sky, minutes with a
poor one (the bench's cold starts took 124 to 170 s with three to six
satellites at 27 to 33 dBHz).  u-blox's AssistNow Predictive Orbits, free, give
it orbits for the coming days.  Only the v4.0 build has this
(`APP_ASSISTNOW`, on by default with `APP_BOARD_HAS_EXT_GNSS` and
`APP_FOTA`); the car's v3.x image is unchanged.

- **The firmware never talks to u-blox.**  The l0destar server registers each
receiver with u-blox's service, downloads the orbits once a day and keeps them,
so a reboot or a firmware update loses nothing, u-blox's per-device request
limit (300 a month on an evaluation profile, 2 a day on a commercial one) is
spent on the server's schedule, and the credential for the registration
service stays on the server and out of the firmware image.  The unit asks the
update server over the same TLS as the manifest (`/assist/data.bin`), and the
server's reply is passed to the receiver as it is.
- **When it asks.**  Once per power-up, when the first three satellite lists
(UBX-NAV-SAT, whose flags now give the ephemerides, almanacs and AssistNow
orbits on hand) show fewer than five ephemerides and there is no fix.  A hot
start, where the backup supply kept them, asks for nothing, and orbit data is
never sent to a receiver that has fresher.
- **How it is given.**  On a thread of its own (`assistnow.c`), so a slow name
lookup cannot hold the main loop past the watchdog: the time from the modem's
clock first (UBX-MGA-INI-TIME_UTC, three seconds of accuracy), then the data,
a few messages at a time with the receiver's UBX-MGA-ACK for each counted
(`CFG-NAVSPG-ACKAIDING` is now set).  Only assistance messages go through: the
data is walked frame by frame, checksums checked, and anything else a response
carried, a configuration or a reset included, is dropped.
- **Registering.**  A 404 from the server means it has not met this receiver.
The unit then sends the receiver's replies to the chip id (UBX-SEC-UNIQID) and
version (UBX-MON-VER) polls, byte for byte as hex, and the server does the rest.
Both replies are kept by the driver for that.
- Costs about 14 KB of RAM (the app was at 78 %, now 88 %): a 10 KB download
buffer and the thread.  The server cuts what it sends to the buffer a whole
constellation at a time (`APP_ASSISTNOW_GNSS`, default GPS and Galileo).
- Tests: `tests/test_assistnow_ubx.py` builds the message builders, the
frame scanner, the NAV-SAT counts and the calendar conversion on the host and
checks them against `struct` layouts, `datetime` and, where installed,
pyubx2.  The server side is `server/tests/test_assistnow.py`.
- First run on the bench (2026-10-30): the registration worked.  The first
real reply, 7 KB written with one `sendall`, was a single 7.4 KB TLS record,
which the modem cannot decode: a record above about 2 KB fails the read with
`EMSGSIZE`, logged as `http_client_req() error: -122` (it is why firmware comes
down in 2 KB ranges).  The server now writes such a reply in 1,400-byte slices,
each a record of its own (`assistnow.send_response`); the firmware is
unchanged.  Not yet seen on the board: the data reaching the receiver.

### The v4.0 board's receiver: the real pin map, its real baud rate, and NMEA when it will not listen
The first v4.0 board on the bench (2026-10-30) showed three things the draft
had wrong, and one it did not allow for.  Nothing here is built into the car's
v3.4 image: the receiver driver is only built where `APP_BOARD_HAS_EXT_GNSS` is.

- **The pin map is the PCB's of 2026-10-25.**  The design swapped two nets after
the 18 Oct one the draft was read from: `L_SENSE` is on P0.13 (AIN0) and
`GNSS_BCKP_EN` on P0.5.  The old map put the L-line sense on P0.5, which no ADC
channel reaches (`L_SENSE on P0.5 has no SAADC channel` at every boot), and
drove the sense pad as the backup-supply enable.  `APP_PIN_L_SENSE` is 13 and
`APP_PIN_GNSS_BCKP_EN` 5 on v4.0; the L-line short check is available there as
on v3.3, and the "cannot be read" warning is gone from the hardware reference.
- **The receiver comes up at 38400, not 9600.**  That is what the module on the
bench sent (a NMEA burst a second, no UART errors), and the driver found it only
because 38400 was the last rate it tried.  It tries 38400, then the configured
rate, then 9600.  The comments, the Kconfig help and the two reference pages
said 9600.
- **A receiver that is heard but takes no commands is read, not declared
dead.**  The module's UART RX pad was open on the first board: it sent NMEA the
whole time and never acknowledged anything (no reply to the baud change or to
the configuration), so three attempts timed out, bring-up failed twice and the
alert said "GNSS receiver not responding (UART)", which was the one thing it
was not.  Now, when NMEA has been heard and the configuration still fails, the
driver carries on with the sentences the receiver sends by itself
(`src/gnss_nmea.c`): RMC and GGA make the position, speed, heading, altitude
(above the ellipsoid, as the UBX path reports it), HDOP and time, GSV the
satellites tracked and the strongest signal.  A fix comes out as the same
`struct ubx_pvt` NAV-PVT decodes to, so everything after it is the one path.
It is `APP_GNSS_EXT_NMEA_FALLBACK`, on by default.
- **The alert says what is wrong.**  In that mode the alert is "GNSS receiver
takes no commands, using NMEA (check UART TX)" at priority 0, since positions
are still arriving; "not responding (UART)" is kept for a receiver that is
silent.  The board test names the mode in its receiver line.
- What the fallback does not have: the receiver stays at its own defaults, one
fix a second and the portable dynamic model rather than the automotive one, and
its NMEA is left on.  Both come back on the next power-up with the commands
getting through.
- **The receiver gets a second to take the new baud rate.**  With its RX pad
joined, the same board's MAX-M10S took the baud-rate item and moved to it
about 940 ms later, at its next NMEA burst.  The driver listened for 600 ms,
went back to the old rate, met noise (24 to 26 UART errors in each 500 ms
window), took it for a receiver that hears nothing, fell back to NMEA at the
wrong rate and queued the "takes no commands" alert; the five-second silence
check found the receiver at 115200 and configured it 12 s into the boot, and
the alert went out anyway, as the first fix wait began.  The driver now asks
again every 250 ms for 1.5 s (`receiver moved to 115200 baud after 942 ms`),
finds the receiver once more when the configuration meets noise and not a
receiver talking at the old rate, and raises the NMEA alert only while the
fallback is still in force when it is asked.  Bring-up on that board is now
about a second.
- Tests: `tests/test_gnss_nmea.py` builds the NMEA decoder and the stream
parser on the host and replays the burst the board's receiver sent, checks 60
random fixes against `pynmea2`, and fuzzes both under address and undefined
behaviour sanitizers.

## 0.4.79

### A tracker on the backup module reports at its own interval
`int` was written for a parked car on its own battery.  On the inline backup
module the tracker is spending a cell that does not recover, and the cadence
wanted there is not necessarily the same: a longer one spares the cell, a
shorter one follows a car whose power has been cut more closely.  Until now
the only interval on backup power was `int`.

- **The server holds a second interval, `device.backup_int`, and every reply
carries it as `backup=<seconds>`.**  It rides every reply as `track=` does, so
a unit that restarted or missed one converges on it, and the firmware applies
it as it reads the reply and takes it out of the commands, so it never reaches
`cmd_run()`.  1 to 9 become 10, as for `int=`.  Unlike `int` the unit never
reports it back: nothing on the unit changes it.  0, which is what every
device has, means `int` applies on backup power too, so nothing changes until
it is set.  The name is `backup_interval` as a server command and not
`backup_int`, because the firmware finds `int=` anywhere in a command string:
a command ending in it, queued by a server that did not know it, would have
set the engine-off interval.
- **The sleep loop uses it while the module is carrying the rail.**
`report_interval()` is the backup interval while the loop has seen the module
take over (`s_backup_seen`) and `int` otherwise; the countdown, the no-signal
backoff, the owed-report test and the quiet-pass test all go through it.  The
report that announces the cut still goes out at once; the next is one backup
interval later, and the report that announces car power back puts `int` back.
It works with `int` 0 as well: a unit that makes no timed reports on car power
reports on a schedule while it is on the cell.  `on backup power: next timed
report in 90s` is logged after each.
- **Movement keeps its ceiling.**  A confirmed movement already cuts the next
report to four hours when `int` is 0 or longer.  On the module the interval in
force is the backup one, so a backup interval over four hours is cut the same
way, for that countdown only and without lowering the setting.  A backup
interval of four hours or less is left alone, and the old rule no longer
applies on the module when a backup interval is set, because it would have
lengthened a shorter one to four hours.
- **`APP_BACKUP_LOOP_INTERVAL`** (default 0) is the value from boot until the
first reply, as `APP_ENGINE_OFF_LOOP_INTERVAL` is for `int`; it is also all a
Traccar build has.  `config` shows `backup=<n>` and the boot log prints it.
- **The server** gets the column (`schema.sql`,
`migrations/2026-10-18_device_backup_int.sql`), `backup` in
`GET`/`POST /api/1.0/config`, `backup_interval=<seconds>` as a command
(written to the column, not queued, since every reply carries it),
`tools/device.py show`, and the reply field in `docs/PROTOCOL.md`.  Older
firmware passes over `backup=`, and a server without it sends none, so either
can be updated first.
- On the bench, with a build whose default was 90 s against a server that
sent no `backup=` and an `int` of 300: cutting the 12 V supply at 15:11:05
was reported at 15:11:16 (the module's pulse, then the rail in the band 7.8 s
after it), and the next timed report came at 15:12:48, 90 s later, with the
battery gates bypassed, and set the one after it at 90 s again.  With the
supply back, car power restored was reported at 15:13:21 and the next timed
wake was 5 min later, the `int`.  The reply
parsing is tested on the host (`reply_take_int`, 22 cases under the
sanitizers) and the server side by its own tests; a reply that carries
`backup=` has not been through a real server and unit yet.  The car has not
been updated.

## 0.4.77

### Gaps of up to two minutes are filled
The motion through a gap between fixes (`mv=`) went on gaps of up to 60 s.
On 2026-10-11, after the car moved from O2 to Vodafone at 18:45, the network
held the radio for 67, 84 and about 88 s after a send, three times in ten
minutes.  GNSS saw no satellites for the whole of each, and the first two
were drawn as straight lines 250 and 420 m long.  Since 0.4.68 the car has
had 8 gaps of 60-120 s on the move, none of them filled.

- **`DR_MAX_GAP_MS` is 120 s.**  The heading log keeps 128 s and the ECU
speed log 192 readings.  A two-minute field is 249 bytes, which fits the
server's 255-byte column (`deadreckon.MOTION_MAX`), so the server needs no
change.  A `BUILD_ASSERT` holds the field to that.
- **The gyro's drift comes out on the device.**  The server removes drift in
a straight line only while it comes to 45 degrees or less.  Over two minutes
an uncorrected 0.5 dps is past that, and the fill was no better than the
straight line.  The field now takes drift out as the run past the last fix
does: the whole of a small turn in a second the ECU reads standing still,
and the learned rate in the rest.  In a host simulation of 65-119 s gaps,
with turns and a stop, the fill came within 15 m of the true path at 0.2
and 0.6 dps of residual drift, against 140-220 m for a straight line.  At 1
dps with little standing time beforehand to learn the drift from, it was
105-130 m.  Gaps of 4-50 s were as before.
- **A long field ends its batch a record early.**  Fields up to 128 bytes
still ride outside the batch's byte budget.  A longer one is counted,
because next to three records it would overflow the datagram and be left
off.
- **`APP_DATABUF_REC_MAX` is 768** (was 512): a record has to fit a backlog
slot with its field, or the field is left off.  That is 8 KB more BSS at 32
slots; the app is at 80% of RAM.

## 0.4.76

### The bench's deliberate send hang is out
0.4.75 was committed and published with a bench-only test still in
`transport_send()`.  It hung the third send after a boot for 45 s, so that
the watchdog would fire and test the stall note.  On the car that is one
watchdog reset about a minute after each boot that is not itself a stall
reset: once after the 0.4.75 install, then quiet until the next reboot.
Removed; nothing else changed.  The test did its job: the bench reported
`rst=wdt+stall:4b116/551ac/350b0/3da04`, which the map puts in `k_sleep()`
called from `transport_send()`, so noinit RAM survives a watchdog reset.

## 0.4.75

### An ignition change with no fix is stamped when it happened
An ignition-change record that cannot get a fix is sent from the last known
position, and it was also stamped with that fix's time.  The server orders
records by that time.  In a car park on 2026-10-11 the last fix was at
15:17:42.  The ignition went off at 15:18, back on at 15:19 and off again at
15:20.  Both ignition-off records carried 15:17:42, but the ignition-on came
through the cached-position path, which stamps the modem clock, so it carried
15:19:29.  It sorted last, and the map said "engine on" for a parked car.  A
record without a fresh fix is now stamped from the modem clock, like the
cached-position records.  The position is still flagged as not fresh (`cl=1`).

### A send can no longer hold the loop past the watchdog
At about 14:56 on 2026-10-11 the car reset mid-drive with `rst=wdt`, its
first watchdog reset since 0.4.47.  It was in the middle of an operator
change.  At 14:53:59 a cause 9 on Vodafone (TAC 6191) re-registered in
9 s, and the last records reached the server at 14:54:43.  The unit came
back on O2 B20 at -112 dBm with a new public IP.  Nothing in between reached
the server, so the warnings that would say what blocked went down with the
RAM.  What the cycle can block in without a kick: the modem's AT commands,
and the UDP send.  The modem library's `SO_SNDTIMEO` defaults to no limit,
and the socket never set one, so a send the modem would not take while it
changed networks could wait until the watchdog fired.
- **Sends time out after 10 s** (`SEND_TIMEOUT_MS`).  A timed-out send fails
like any other: the records go to the backlog.  A send normally returns in
milliseconds.
- **A kick before each send,** the retry included, so two sends at their
limit and the 4 s reply wait still fit the 32 s window.
- **A send that takes 2 s or more logs a WRN** with its time, so a stall
that stops short of the watchdog shows on the server.
- **A watchdog reset now says where it was stuck.**  The timeout callback
scans the main thread's stack for return addresses and keeps the first
four in noinit RAM, as the fatal handler does.  The next boot reports them
in `rst=` as `wdt+stall:<addr>/<addr>/...`, to look up in the map file.
It is a scan, not an unwind, so an old address left on the stack can show
up.  It fits the server's `varchar(48)`.  Whether noinit RAM survives a
watchdog reset (and not only a soft one) is not yet checked on the bench.

### A car park's key-off is sent from the bay, not the entrance
The dead reckoning (0.4.68) filled gaps between two fixes, and a car park
has only one: GNSS goes at the entrance, and the key-off record was sent
from there.  On 2026-10-11 the map put the car at the entrance at 15:17:42,
and nothing showed where it went after.  The ECU's speed and the gyro's
heading now also run the position on past the last fix, second by second
(`motion_position()`, `src/motion.c`).  A record with no fix of its own is
sent from there, flagged `dr=1`, which the server already stores in
`dead_reckoning`.  The position goes into the stored one too, so the timed
wakes while parked carry it as well.
- **The heading is the last fix's at 10 km/h or more,** carried on by the
gyro.  Slower than that the GNSS heading is mostly noise, and the last fix
at a car park entrance is usually at a crawl.
- **Standing still, the gyro's drift comes out.**  While the ECU reads 0, a
heading change under 1.5°/s is drift.  It is taken out of the heading and
learned for the seconds on the move, so a queue at a barrier does not bend
the rest.
- **The speedometer's error is learned** from fixes at 20 km/h or more.
- **It gives up rather than guess** until the next fix if: the ECU goes quiet
on the move, a hole in the heading log falls while moving, or there have
been 5 min of driving without a fix.  The record is then sent from the last
fix, as before.  A hole while stood still (the key off, the FIFO undrained)
keeps the heading, so a second key-on and re-park carries on from the bay.
- **With the key off, the run goes to now.**  A last ECU reading of 3 km/h or
less winds down to 0.  A faster one means the ECU went quiet on the move.
- **Limits:** it is 2D only, so a ramp counts its full length and nothing says
which level.  Reversing reads as forward, because the ECU speed has no sign.
It needs the ECU speed, so it does nothing on a unit without one.
- Checked on the host with the real `motion.c` in a simulated entry: a 75 m
crawl with a hairpin and a turn into a bay, gyro bias up to 1.2°/s, ECU
reading 3% high, and a 90 s queue.  The bay came out 1.4-9 m from the truth,
where the entrance fix was 19-23 m away.  A key-on, re-park and key-off came
out 2-9 m off, where the entrance was 30-34 m away.  An ECU going quiet on
the move gave no position.  Not yet checked on the car.

## 0.4.74

### Records name the operator they were actually sent on
The operator (mcc/mnc) was read only when the unit woke or booted, while
the cell and tracking area came from the modem's cell events as they
happened.  A drive that moved from one operator to another kept the first
one's code on every record after.  At 17:48 on 2026-10-10 the car came out
of the underground car park on EE B3, and its 125 records there went to the
server as O2.  The DB shows the same for most of the car's history.
Records on the Vodafone cells around Morden (TAC 12296 and its
neighbours, some 2,300 of them) were stored as O2.  Records on O2's TAC
4096 after a Vodafone stint (about 1,100) were stored as Vodafone.
The PLMN is now tagged with the cell it was read on (`plmn_cid`).  When a
record's cell group goes out on a different cell, one `%XMONITOR` reads it
again first.  That is one AT command per cell change; it does not use the
radio, so GNSS loses nothing.  The periodic signal reading picks up the PLMN
from the same `%XMONITOR` line, and an operator change is logged.  Track mode
runs no AT commands and is unchanged.

## 0.4.73

### The ignition-off record no longer waits for GPS underground
At 17:28 on 2026-10-10 the car drove into the same underground car park as
that morning and went dark for the whole 18-minute stay.  The records came
in as it drove out, none of them lost, but the ignition-off record was 18
minutes late.  A bench replay of the stay showed why:
- **A fix wait ran on past the key-off.**  The ignition is only looked at
between collections, so a key-off during a cold search waited out its
300 s.  Then the ignition-off record waited out another 300 s cold search
before falling back to the last known position.  The bench sent it 10
minutes after the key went off.  Now a fix wait ends when the ignition
changes, once the change has held past the crank dropout (5 s) or the
backup module's wake pulse (1.5 s).  A record that has to go out gives the
fix 15 s (`TRANSITION_FIX_WAIT_MS`).
- **A six-minute-old GPS speed started coast-to-stop.**  The car's last fix
before going underground said 5.3 km/h.  At key-off the ECU had no power,
so that speed was taken as the current one, and coast-to-stop began.  The
coast only ends after a send, which a collection with no fix never
reaches, so the unit stayed awake with GNSS running, never reaching the
sleep path that resends.  The ignition-off record, lost with the
registration, was held until the key came back on, though the modem had
registered again within a minute.  The GPS speed now only counts while the
fix is under `SPEED_FIX_MAX_AGE_MS` old, and a collection with no fix ends
the coast.
- **A key-off whose record cannot be sent no longer keeps the unit awake.**
With the modem registered but sends failing locally (a dead socket, a data
connection gone under a live registration, a failed lookup), the
ignition-off record was collected and sent again and again until modem
recovery restarted the radio, 10 minutes on.  Each pass added another copy
of it to the backlog.  The record is in the backlog after the first
failure, so the unit now sleeps, and `do_sleep()`'s delivery and the timed
wakes carry it from there.

## 0.4.72

### The server's address is looked up again when the server goes quiet
The address of `APP_SERVER_HOST` is still looked up once per boot and kept,
so a send costs no lookup.  Kept for good, though, a moved server would be
sent to until the next reboot, and the update notice that could have forced
one rides on the replies that no longer come.  Now three sends in a row with
no reply, all made with the modem registered and the first over 5 minutes
ago, close the socket and look the name up again.  Sends made without a
registration don't count, so a dead zone or a TAU reject never triggers it,
and 5 minutes is well past the 38 s stall seen on a registered link.  If the
lookup fails or finds the same address, the address in hand is kept and the
number of sends before the next try doubles, up to 48.  Any reply resets it.

## 0.4.71

### A parked unit falls back to NB-IoT where LTE-M does not reach
At 09:44 on 2026-10-10 the car drove into an underground car park at 7 km/h
and went silent: no ignition-off record, and nothing could reach it until it
was driven out.  LTE-M only is still the rule.  But when a parked search on
LTE-M runs out its `APP_NETWORK_SEARCH_TIMEOUT`, the modem now switches to
LTE-M plus NB-IoT, LTE-M preferred, and gets a fresh window with the report
still owed.  This happens on a timed wake, or at sleep entry when the search
had already run out on the way in.  The fallback stays on through later timed
wakes and goes off at key-on, so a drive never runs on NB-IoT.
`APP_NBIOT_PARKED_FALLBACK` (default y).

At home the bench attaches on EE NB-IoT (B3) and Vodafone NB-IoT (B20).
NB-IoT copes with about 8 dB more loss than LTE-M; whether that is enough
for that car park is for the car to show.
- **Key-on cost only after a dead-zone stay.**  `%XSYSTEMMODE` needs the
radio offline, so going back to LTE-M with the modem up is CFUN=4, the mode,
then a full bring-up.  Measured at 3.3-5.3 s with AT commands, and 4.1 s
from key-on to GNSS start in the firmware.  Otherwise a key-on still resumes
from PSM at no cost.  With the radio off the mode only applies at the next
bring-up, for nothing.
- **The CFUN=4 is waited for in watchdog-fed slices** (`nrf_modem_at_cmd_async`),
capped by `APP_NBIOT_OFFLINE_CAP_S` (90 s).  The detach took about 60 s once
on the bench.
- **Bench test** with `APP_DEBUG_NBIOT_TEST` (LTE-M held to an empty band
until the fallback is used, NB-IoT preferred, a simulated key-on 90 s after
the report): after 301 s on LTE-M the fallback came on.  It took 24 ms to go
offline, then 6 s to register on EE NB-IoT.  The owed report reached the
server with `rat=NBIOT`, and the modem stayed in PSM on NB-IoT.  At the
simulated key-on it went offline in 1.2 s, registered on LTE-M 2.7 s later
and started GNSS 4.1 s after the key, with the first ignition-on record at
the server 8 s after the key.

## 0.4.70

### Band 3 is back, on trial
EE's LTE-M is on B3 only here, and B3 had been locked out since EE kept
winning operator selection over O2 and Vodafone on B20.  On 2026-10-10 the
bench, unlocked at home, scanned all three.  O2's home cell 135829634 (B20) is
the strongest at -102 dBm, but with an SNR of -4 dB the modem needs every
downlink four times and rates it poor (`%CONEVAL` energy estimate 6).  EE's
cell 3167234 (B3) reads -110 dBm, but at +3 dB SNR, with no repetitions and a
normal estimate (7).  `CONFIG_LTE_LOCK_BAND_LIST` is now `"3,8,20"`, so the
modem can choose EE.  Every move between EE and O2 costs a full reattach after
an EMM cause 9 reject (the bench saw one switching O2 -> EE), so this is a
trial: if rejects or gaps on the road go up, set it back to `"8,20"`.

### A record lost with the registration is resent when the network is back
At 23:14 on 2026-10-09 the parked car's hourly report went out, and 6 s later
the registration was lost: the modem had moved to TAC 12296 cell 289044, where
the network rejected it (cause 9) and discarded the queued datagram.  The
record went back into the backlog after 8 s unanswered.  The wake had already
checked for a registration before waiting for replies, found one and owed
nothing, so it powered the modem down.  The record reached the server with the
00:15 report, an hour late.  Now, if records are still in the backlog once
the replies have been waited for and the network has gone, the report is owed
like an unsent alert.  The loop keeps the modem searching through
`APP_NETWORK_SEARCH_TIMEOUT` and sends as soon as it registers.

## 0.4.69

### An update check on a dead link no longer runs the watchdog out
At 13:05 on 2026-10-04 the car's hourly wake found 0.4.68 on offer and asked
for the manifest.  The link was dead, so the request ran its full 30 s
timeout and failed (-116), and the failure handling after it took the time
since the last kick past 32 s.  The task watchdog fired ("no kick for 32s —
resetting"), and the main loop kicked again inside the two seconds before the
hardware fallback would have reset the SoC.  Nothing reset, and the update
went in at the next key-off, but that was two seconds from a reboot.
- **A kick straight after the request,** whichever way it ends, so what
follows starts a fresh window.  `fota_check()` already kicked just before it.
- **`APP_FOTA_MANIFEST_TIMEOUT_S` is 25 s, at most 25.**  The request holds
the loop for its whole timeout, DNS included, with no kick inside, so 30 s
left two seconds of a 32 s window.  A `BUILD_ASSERT` keeps 7 s.  Not lower:
a check over a working link took 15.6 s on the bench.

### The track through a gap is measured, not guessed
GNSS gets nothing while LTE holds the radio, and every send holds it.  Most
cells here let go about 2.5 s after the reply; the TAC 12296 cells keep it for
about 10 s, and no cell here honours release assistance (every `%RAI` report
the bench has logged reads `as_rai=0, cp_rai=0`, so `SO_RAI` changes nothing).
On those cells a drive is three one-second fixes every 14 s joined by straight
lines 100-180 m long: all of the 22:49 drive on 2026-10-03, at half the record
rate of a drive on TAC 4096.  A rejected tracking-area update blinds the
receiver for longer still: at 22:37 the same evening it was 24 s, then 15 s
more for the re-attach's own connection, and the replay drew 214 m in one
straight line across it.  The firmware cannot shorten either.

So it now measures what it cannot see.  The record that ends a gap of 2.5-60 s
on the move carries `mv=<gap>:<v0>:<steps>:<end>`: the ECU's road speed and
the gyro's heading change for each second of the gap.  The server fits that
path between the two fixes and the replay draws it dashed.  See `src/motion.c`.
- **Heading change from the gyro.**  The IMU's FIFO already batches the gyro
at 26 Hz whenever the unit is awake.  Every drain now feeds the samples,
projected onto gravity (the accel words averaged over ~20 s), into a running
heading noted at each whole second.  The fix wait's tick and the top of every
main-loop pass drain it, so the ring (about 7.4 s at 26 Hz, measured on the
bench) does not overrun in an ordinary cycle.  One that does — the loop held
up for longer than that, as by the key-on fault-code read — or a restarted
FIFO starts the log again, and a gap across the hole gets no field.  Checked against the car's records before it was written:
over 2,084 one-second pairs the GNSS heading change was -1.0 times the
projected gyro, the sign a right-handed gyro gives with gravity pointing up.
The car's unit sits 19 degrees off level with Y up, so Z alone would not do.
- **Speed from the ECU.**  Every K-line speed reading is kept with its time,
and each second's is interpolated between the readings either side, which
bridges the poll's pause for a send.  Without an ECU the field carries
headings alone and the server takes speeds from the fixes.
- **The fix's real moment.**  `gnss_fix.epoch_ms` is the uptime the receiver
delivered the fix at.  `fix_uptime_ms` is when the loop picked it up, which a
K-line reopen can make 4 s later.
- **Two bytes a second, never at a record's expense.**  A 3-4 s gap between
batches costs 15-17 bytes, a 12 s one 33, about 1.5 KB per ten minutes of
driving.  The batch's flush threshold leaves the field out
(`data_motion_bytes()`), so it never makes a batch go early and cost fixes.
The field is built to fit a backlog slot (`APP_DATABUF_REC_MAX`) with the
record, and `send_data()` drops it from a batch that would not fit a datagram.
- **An impact's profile stays in the FIFO.**  The FIFO gives each sample once,
so nothing drains it for the dead reckoning while an impact is waiting for
`crash_check()`; the impact's own drain feeds the dead reckoning instead.
The pre-impact history is now whatever has come in since the last drain, a
second or so in a cycle, rather than the whole ring; the peak and everything
after it are there as before.

`APP_DEAD_RECKONING` (default y) turns it off.  A host harness ran the real
`motion.c` through simulated drives with the car's mounting, gyro noise and
bias, a 1.5% IMU clock error, irregular drains and holes in the ECU readings,
and decoded the fields with the server's `deadreckon.py`: the fitted path was
within 0.3-4.5 m of the truth where the straight lines it replaces were
21-38 m off.

### A long gap between fixes says why
At 20:28 on 2026-10-03 the car went 31 s between fixes at 24 mph and left no
trace of why: no registration loss, no starved-receiver warning.  A warm fix
wait that LTE blocks outright never logged anything.  Now fixes 20 s or more
apart (`APP_FIX_GAP_WARN_S`), with the key on and a fix at either end moving
at 10 km/h or more, get one WRN line: how long the loop waited for the fix,
how long GNSS was blocked by LTE and an RRC connection held, how many epochs
the receiver flagged as short of radio time, the cell before and after, and
whether registration was lost.  That separates a radio-blocked gap from one
the loop spent elsewhere.  At most one a minute, the next saying how many were
held back.  Run over the car's records from 2026-09-25 it would have fired 14
times in eight driving days, never more than five in a day.  Drive starts are
not counted: the record at key-on is the stored position, not a fix.

## 0.4.67

### An unfitted interface's rail senses are left disconnected
The parts that drive each rail-status line belong to its interface: S7R3/S7R4
for CAN, and S7R8/S7R9 plus the S7Q1/S7R7 inverter for the K line.  A board
built with one interface leaves the other's sense lines as bare nets, but
`hw_domain_init()` made all four connected inputs on every v3.1+ board.  So a
CAN-only board had P0.00 and P0.31 (PP3V3_K_ST, PP12V_K_ST) floating with
their input buffers on.  A K-line board without the CAN parts had P0.22
(PP3V3_CAN_ST) the same way.  Nothing reads them on such a board, and a
floating connected input can only cost current: Nordic give no figure for
it, and it depends on where the node drifts.  A sense line is now an input
only when `APP_BOARD_HAS_CAN` or `APP_BOARD_HAS_KLINE` says its interface is
fitted.  Otherwise it is set to the reset state (input disconnected), written
through the HAL because `GPIO_DISCONNECTED` leaves alone a pin the driver has
not configured.  It is not driven low or pulled down: on a board with both
interfaces fitted, S7R7 holds PP12V_K_ST at 3.3 V and either would draw ~30 uA.
The other K pins needed nothing.  On a CAN build K_TX, K_RX, K_SLEEP and
L_SEND are never configured, and K_EN is driven low, so the K pull-downs
missing on such a board (S1R5, S10R5, S10R6, S10R9) cost nothing.

## 0.4.66

### Setting off is not a tow
At 10:01 on 2026-10-03 the car sent "tilt 24.5deg - possible tow/jack" as it
pulled away, and at 23:37 on 2026-10-01 it sent "movement: 17.6deg tilt" the
same way.  Both times the next record had the key on and the car rolling, at
21 km/h and 4 km/h on the ECU.  A sleep pass checked tilt, then the ignition,
then movement, and the tilt debounce (2 s) and the movement confirm (up to
10 s) can each hold a pass that long.  A driver who gets in, starts up and
drives off inside one pass was measured as a parked car being tilted or
moved: the tilt check compares single accelerometer reads with the parked
attitude, and pulling out of a junction is many degrees of that.
- **The ignition comes first in every pass.**  A pass that starts with the key
on goes straight to the drive without looking at tilt or movement.
- **A key that comes on during the pass ends it.**  After the tilt debounce,
and during and after the movement confirm, the line reading on (or an edge
the interrupt latched) drops the alert and leaves the key to the next pass.
The confirm now stops at the first 100 ms poll that sees it, rather than
running out its 10 s.  No parked-impact fallback is sent from such a pass
either.
- **A key turned straight through to the starter is a key.**  The car's sense
line drops out while the starter turns.  If that happened inside the 200 ms a
wake waits for, the pass logged "too short for a key" and went on to the tilt
and movement checks.  A short pulse now gets `IGN_CRANK_MAX_MS` (5 s) for the
line to come back before it is ignored.

### A crank is not a low battery
At 10:16 the same day the car sent "low battery: 9.82V" as it was started
after an 11-minute stop.  The key-on woke it, and when IDLE first looked a
second or two later the starter was turning and the sense line was out.
IDLE took that for a short key cycle: it built an ignition-off record at the
bottom of the sag and went back to sleep, and the same key woke it again
four seconds later.  The 60 s settle guard (`BATTERY_WARN_SETTLE_S`) did not
hold the alert back, because it counted from the last record that said the
ignition went off, the key-off eleven minutes earlier.
- **IDLE gives the line `IGN_CRANK_MAX_MS` to come back** before it reports a
short key cycle after a key-on wake.  A crank no longer produces a record, a
sleep and a second wake.
- **The settle time also counts from the last time the line read on**,
wherever it was read (`ignition_last_on_ms()`).  So no reading taken within a
minute of a key-on alerts, by any path.  The backup-power alert uses the
same guard.

### Movement alerts give the real deviation
Every movement alert from the car has said about 3000 mg (3010 on 2026-10-01,
3225-3484 in September), however gently the car moved.  The figure was read
straight after `accel_irq_disable()` moved the IMU from ±2 g low-power to
±8 g, before it had a valid sample at the new setting.  On the bench, a read
at that point was wrong on 5 tries out of 8 (11-14 g from a unit at rest),
while the same read taken at ±2 g just before the switch was 1-18 mg every
time.  The figure is now read before the switch, at the ±2 g the confirm ran
at.

## 0.4.65

### No more phantom impacts from FIFO samples read at the wrong full-scale
The IMU's FIFO ring was never emptied when the accelerometer changed range,
and the drain converts every sample at the range in force when it runs.  So
after a confirmed-movement wake in sleep, which goes to ±8 g and re-arms at
±2 g, the next drain read the ±8 g samples at a quarter of their size: a still
unit came out at ~250 mg, a deviation from 1 g of 744 mg, which the
parked-impact path takes for a hit.  The reverse happened on a wake from
sleep, where the awake ring started out holding ±2 g samples that it read at
±8 g, so a still unit read 4.1 g.  The IMU also keeps its FIFO, still
batching, through an MCU reset.  `accel_enable_wake_int()` and
`accel_fifo_enable()` now go through bypass, which empties the ring, before
starting it again.  On the bench, the two switches read 744 mg over 3154 ms
and 4.1 g over 3914 ms before the change, and 31 mg and 26 mg with nothing
over threshold after it.

## 0.4.64

### A power cut is reported, with or without the module's wake pulse
The first bench tests of the inline battery backup, on 2026-09-30, found two
things wrong with how a sleeping tracker learned of a cut.  The module fires
its ignition pulse as the car rail falls through about 10.06 V, but its
switchover comparator watches the car side of its ideal diode, and at that
moment the tracker's own rail is still held at the car's level by its input
capacitance: 11.31-11.35 V at the pulse, in the backup band 4.4-4.7 s later.
0.4.47 read the rail at the pulse and required the band, so it could never
recognise a cut, and without `APP_BACKUP_SUPPLY` a pulse that had ended by
the 200 ms re-read left nothing in the log at all.  Separately, IGN passes
straight through the module and the pulse goes in through 10K, so a bench
lead holding pin 5 at ground for "ignition off" shorted it; with pin 5 open
the tracker saw 482 ms and 706 ms pulses.  In a car the ignition wire's own
loads hold it down the same way, which is a module fix (an isolation diode),
so the firmware no longer depends on the pulse.
- **A pulse is told from a key by how long it lasts.**  With
`APP_BACKUP_SUPPLY`, the line still on 1.5 s after the interrupt
(`BACKUP_PULSE_MAX_MS`) is a key, timed from the interrupt itself.  So a
key-on from sleep is recognised after 1.5 s instead of 200 ms.  Without the
option the 200 ms debounce is unchanged.
- **Then the rail is watched, not read once.**  Up to 60 s for it to fall into
the band and stay there 2 s with the line off (`BACKUP_SETTLE_MS`,
`BACKUP_CONFIRM_MS`).  The watch gives up early if the line comes back on (a
key, or a crank ending) or the rail holds steady above the band, falling less
than 20 mV in a second, which is a car still connected.  A pulse that was
not a cut but lasted 200 ms or more is reported as a short key cycle, as
before; a shorter one is ignored.
- **Every ignition wake that is not a key is logged**, with the pulse length
and the rail: "wake: ignition pulse, on for 482 ms, rail 11.35V".
- **The rail is also polled while asleep.**  With `APP_BACKUP_SUPPLY`, every
sleep-loop pass takes one INA conversion (`battery_poll_voltage()`, about
5 ms) at the tilt poll's 30 s, so no extra wakes (`BACKUP_RAIL_POLL_S` sets
the cadence if tow detection is off).  In the band, confirmed 2 s later, it
reports the cut once.  Back above it, it reports the car supply's return
once, past the battery gates, so `car power restored` goes out straight
away.  On the bench a cut was reported 37 s after the supply went off
without the pulse, and 8.3 s after with it; a return 20 s after the supply
came back.
- **A report owed on a cut or a return survives the pass.**  It was a
per-pass flag, and a movement check that ended the pass early (a cut that
also jolts the car) dropped it.
- The `APP_BACKUP_SUPPLY` help said the module runs from an LTO pack; it is a
CR123A cell.

### Quiet sleep passes stay quiet with the modem in PSM
`sleep_modem_release()` counted a registered modem as one to release, and in
PSM the modem is registered for good.  So every 30 s tilt poll woke the
console and called `modem_sleep()`, which only logged "sleep: modem left
registered": about 9 ms of UART and HF clock per pass, as much as the pass's
own work.  A pass that raised nothing now leaves alone a modem that says it
is asleep in PSM (`modem_psm_asleep()`).  One that is registered but awake —
just used, back on the network by itself, or not getting into PSM — is still
released as before.  Not yet seen on a cell that grants PSM: the bench's
refused it for the whole test.

### Alerts are held until answered
On 2026-09-30 at 19:55:19 the bench's `backup power: 9.19V` went out two
seconds before the network rejected the registration (EMM cause 9, TAC
12296), and the server never saw it.  The record sent with it was held and
arrived three minutes later; the alert, forgotten the moment the modem took
it, did not.
- **Alerts are held like records.**  Each keeps its slot until the reply to
the datagram that carried it comes in (`databuf_ack()` credits alerts too).
Unanswered after `APP_ACK_TIMEOUT_S`, or when the socket is about to close
(`databuf_settle()`, which now waits for alert replies as well), an alert is
sent again with the next send.  The queue is 8 alerts, up from 5.
- **An alert lost with the network is owed like a report.**  The sleep loop
keeps the modem searching and sends it as the network returns, rather than
an hour later with the next timed report.  One unanswered with the network
still up waits for the next send, so a server that is not answering is not
asked again every 30 s.  With timed reports off, an owed alert goes on its
own.
- **An alert sent on its own waits for its reply**, up to 4 s
(`RESPONSE_TIMEOUT_MS`), since the radio goes down straight after.
- **`aid=<n>`**: each alert line now ends `,aid=<n>`, consecutive within a
boot from a random start like `rid=`, and the server drops a copy rather
than notify twice.  Traccar builds are unchanged: they hold nothing and send
no id.
- **Needs the server's alert dedup.**  A server without it shows the
`,aid=<n>` in the alert text and relays a resent copy twice.  Both the
l0destar server (`server/`, with tests) and the car's deployment have it.
It remembers ids in memory, so a copy that arrives across a server restart
is relayed again.
- Not yet proven on hardware: the bench test with `APP_DEBUG_DROP_DATAGRAMS`
was cut short when the module came off the bench.

## 0.4.63

### A datagram the modem accepted is not taken as delivered
Both drives on 2026-09-27 lost records the firmware had counted as sent.
Where tracking area 4096 meets 12296 (Morden/Mitcham) the network rejects
the tracking-area update with EMM cause 9, "UE identity cannot be derived",
and the modem drops its registration, attaches again and discards whatever
it had queued.  The device log has 16 such rejects at that boundary since
2026-09-11, on nearly every crossing and on Vodafone as well as O2, so it is
the network's and not ours.  At 10:57 it took a batch of three (rid
30531-30533).  At 11:50, pulling up at home, it took the last position and
the ignition-off record (30662-30663): the send that carried them had waited
for its reply, got none, and still counted.  Apart from that boundary, about
1% of driving records vanish with no network event at all (17 datagrams on
2026-09-26).  `transport_send()` returning 0 only ever meant the modem had
the datagram.
- **The reply is the receipt.**  The server answers every datagram, bound
to the request's nonce, so a reply identifies exactly the send it answers.
Each send now gets an id, the transport keeps the last eight nonces, and a
reply is credited to its send however late it turns up.  The socket is
kept between sends instead of closed after each one, so the replies have
somewhere to land.  That costs the radio nothing: the reply was always
transmitted, and was thrown away with the socket.  The release hint is now
`RAI_ONE_RESP` on every send, since a reply is always wanted.
- **Records are held until answered for.**  `databuf.c` keeps each sent
datagram's records under its id (`APP_DATABUF_UNACKED_SLOTS`, 6).  Once the
reply is in they are dropped.  If it has not come within
`APP_ACK_TIMEOUT_S` (30 s) they go back into the backlog and are sent again,
and the server drops the copy of anything that did arrive (same rid, same fix
time).  With every slot waiting, the oldest makes room the same way.  The
log says so at WRN, "datagram #N unanswered after S s — K records back in
the backlog", so the device log shows every datagram that went missing.
- **The backlog waits for proof the link works.**  It is flushed only once
something sent since the previous record — or since the network came back —
has been answered.  At 11:48 the modem sat on seven datagrams for 38 s on a
weak cell and delivered them in one burst, and the one after them never
arrived.  Resent records now wait for the link to come back instead of
joining the pile; the live record still goes each cycle, as before.
- **Before sleep, what is owed goes.**  Going to sleep, and at the end of
each timed wake, the device waits up to 4 s for the replies still owed
(`databuf_settle`), then sends the backlog while the radio is up, a couple
of datagrams at a time for as long as they are answered (`databuf_deliver`).
The ignition-off record's reply is collected rather than closed on, and a
backlog no longer waits an hour for the next wake.
- **Every reply is read, and so are its commands.**  Replies to sends that
were not waited for used to arrive at a closed socket.  The server deletes
a command once it has put it in a reply, so a command in one of those was
lost.  Each reply's settings and commands are now applied whichever send it
answers.  Several can arrive between two runs of the command parser, so they
merge: one-shot commands accumulate, and `fota=` and `track=` come from the
newest reply.
- Track mode's records are not held: they carry an IMU burst too big for the
backlog's slots, and a second copy of a one-second sample, a minute late, is
not worth the airtime.  The Traccar transport is unchanged: its HTTP 2xx
already is the receipt, and `APP_DATABUF_UNACKED_SLOTS` defaults to 0 there.
- **`APP_DEBUG_DROP_DATAGRAMS`** (bench only, default 0) reports every Nth
send as sent and discards it, which is how a lost datagram looks from here.
- **Needs the server's copy detection.**  A server without it stores a
resent record twice.  Both the l0destar server (`server/`, with tests) and
the car's deployment have it.

### A drive keeps recording without a network
`STATE_IDLE` parked the loop while there was no registration and recorded
nothing but ignition changes.  The backlog's "drive into the backlog" path
only ran if a send failed first, and a modem in the middle of a
tracking-area update accepts sends.  At 10:57 the receiver kept fixing
through the 16 s outage and a single record was built.
- **With the key on, recording carries on into the backlog.**  `send_data()`
puts the records straight into it rather than hand them to a modem with no
registration, which would take them and then lose them with its bearer.
The first send after the network returns leads, and the backlog follows
once that send is answered.
- **The radio's retry timer runs from `STATE_SEND` too**, since a drive
recording through an outage cycles between collecting and sending without
passing the idle poll.  It is reset whenever a send finds the modem
registered, so a later outage is not measured from an earlier one.
- The ignition-on fault-code read and the update check wait for a
registration.  The read goes out as its own datagram at once and would be
lost, and an update cannot download without one.

### A late record says when it was built
The server filed every record at the moment it arrived.  So a record sent
late, from the backlog after an outage or again after an unanswered send,
landed in the track and the journey after records built later than it.  A
late ignition-on record behind an ignition-off one was worse: the server
saw an ignition-on and would have raised the ignition alert for a drive that
had already ended.
- **`age=<s>`**: a record sent 10 s or more after it was built carries its
age, worked out afresh at each send from its own `up=`.
- **The server files it at arrival time less age**, and when something
built later is already stored treats it as history.  History fills in the
track but raises no ignition alert, opens or closes no journey, and changes
no settings or track mode.  The packaged server sums journey miles in time
order, so a resent record adds no detour.  A late record that is still the
newest, such as an ignition-off recorded during an outage, is the latest
state and acts like one, at the time it happened.

### The report owed after an outage goes when the network returns
A drive that ends without a registration leaves a report owed, sent once the
modem registers.  The sleep loop only looked every `RESEND_POLL_S` (30 s),
and an accelerometer wake that came to nothing skipped the look altogether.
On 2026-09-27 the modem was registered again at 11:50:54 and the report went
at 11:51:32.  The LTE handler now ends the sleep wait as registration lands
(`modem_on_registered()`), and a pass that finds the report owed on a
registration already there goes straight to it.

## 0.4.62

### PSM sleep engages
The car has run `CONFIG_APP_PSM_SLEEP=y` since 0.4.58.  When the network
grants PSM, sleep leaves the modem registered instead of taking it to CFUN=0:
the modem falls into PSM on its own once the active timer runs down, and the
next wake resumes instead of attaching again.  A modem that stays registered
without ever reaching PSM would draw milliamps, so the sleep loop checks for
that and falls back to CFUN=0.  On the car the fallback fired on every parked
wake from 0.4.58 to 0.4.61 — "PSM granted but the modem is still awake 70s
on", about 90 s after each send — so every wake still paid the full attach
PSM exists to avoid, on top of the tail PSM adds.
- **The modem's sleep notifications are compiled in.**  `APP_PSM_SLEEP` now
selects `LTE_LC_MODEM_SLEEP_NOTIFICATIONS`.  Without it the link controller
never subscribes to `%XMODEMSLEEP`, so `LTE_LC_EVT_MODEM_SLEEP_ENTER` never
arrived and the check could not see a modem that had gone to sleep.  The
modem was very probably in PSM for most of those 90 s — on the bench, on the
car's home cell, it gets there 12 s after the reply — and the fallback then
woke it only to power it off.  The notification threshold comes down from
the stock 20 minutes to 60 s
(`LTE_LC_MODEM_SLEEP_NOTIFICATIONS_THRESHOLD_MS`).
- **The check measures from the radio's last activity.**  It used to arm a
deadline on one pass and judge it about 90 s later.  When that deadline fell
due on the pass of a timed report, the send had just woken the modem out of
PSM, so it read as awake and was powered off — one wake in four.  On the
bench, with 150 s wakes, it fired on the fourth wake as predicted from the
log, and the wake after it attached again (3.9 s, 3.3 s of it attaching).
modem.c now records the radio's last sign of life — a registration, an RRC
change or a wake from PSM — and the loop falls back only when the modem is
not in PSM and the radio has been quiet for the active time plus
`APP_PSM_SLEEP_GRACE_S`.  Six bench wakes, no false fallback.  The warning
now reads "still awake 70s after its last radio activity".
- **A wake from PSM takes about a second.**  On the bench, on the car's home
cell (O2 234-10), a timed wake that resumes from PSM was awake 0.92 to 1.35 s
with no attach, against 3 to 4 s when it attaches (0.4.61 on the car: median
3.0 s, 2.6 s of it attaching).  The modem enters PSM about 12 s after the
reply: the network releases the connection about 2 s after the last data,
then the 10 s active timer runs.  `wake_ms` ends when the modem is left to
fall asleep, so a PSM wake's figure leaves that tail out.
- **No RAI on this network.**  `%RAI` reports neither AS nor CP RAI on O2's
cells here, so the `SO_RAI` hints transport.c gives are ignored and the
connection stays up until the network's inactivity timer releases it.

### A wake from PSM still reports the signal
- **The reading is taken after the send.**  A wake that finds the modem in
PSM builds its record before the radio is up, and `%XMONITOR` has no
measurement to give from a sleeping modem, so on the bench every PSM wake's
record went out without `rsrp`/`snr`/`band`.  When a record goes out without
a reading, the wake now reads `%XMONITOR` once the reply is in, while the
connection that carried it is still up.
- **It travels as `ws=<rid>:<rsrp>:<snr>:<band>`,** beside `wt=` on the next
record, and like `wt=` it describes the record it names.  The server files
it there only where that record has no reading of its own, and never holds
it the way it holds a wake figure: the reading follows a send the server
answered, so the record is already stored.  No schema change; a server
without the change ignores the field.  On a Traccar build it rides as
`wakeRssi`/`wakeSnr`/`wakeBand` attributes, joined on the same
`wakeRecordId`.

### A part that does not answer at boot raises an alert
- **An INA228 or accelerometer that does not answer on I2C is alerted, not
just logged.**  The warnings reached the captured log, but they are silent
losses of what the device is for: with no accelerometer there is no
movement, impact or tow detection, and with no INA228 there is no battery
voltage, which the low-battery alert, the sleep safety gate and the
engine-running fallback all read.  The unit carries on reporting position as
though it were whole.  Seen on a new bench build on 2026-09-24, where the
INA228 did not ACK while the accelerometer on the same bus was fine.  Queued
to ride out with the first record, once per boot, so a board with a dead
part reports again after every restart, FOTA reboots included.
- **`CONFIG_APP_HW_FAULT_PRIORITY` (default 1)** sits above the 0 used for
settings changes and below the movement alarm's 2: a dead sensor needs
noticing, not acknowledging at 3am.  -2 silences it, for a bench board
deliberately running without part of its hardware.

## 0.4.61

### Telling a weak location from a weak antenna
The car reports -108 to -112 dBm on band 20, from two separate eNBs within
4 dB of each other.  That is cell edge, and O2 carries LTE-M on band 20 only,
so there is no better band being missed.  What a single reading from a single
parked spot cannot say is whether that is the location or the hardware.
- **Signal is sampled on its own cadence (`APP_SIGNAL_SAMPLE_S`, 60 s)**
rather than riding the cell group.  One reading per wake describes the
parking space and nothing else; separating a weak location from a weak
antenna needs readings from many places, because coverage swings tens of dB
along a route while a detuned or badly-fed antenna is close to the same
offset everywhere.  A drive now surveys the route.  `AT%XMONITOR` is free on
the radio — it reads measurements the modem already keeps for cell
reselection — so this costs an AT round trip a minute and about 23 bytes on
the records that carry it.  Skipped in track mode, which avoids AT commands
by design.
- **`CONFIG_APP_CONN_EVAL` (default n) adds path loss, RSRQ, CE level and TX
repetitions.**  `dl_pathloss` is the one that travels: RSRP says how strong
the signal arrived, path loss says how much was lost getting here,
normalised against what the cell says it transmits — so unlike RSRP it is
comparable across cells and distances, and excess attenuation in the antenna
path shows up in it directly.  `ce_level` above 0 says LTE-M is using
repetitions.  Off by default because unlike `%XMONITOR` it runs an
evaluation and is refused while the radio is busy, which on a drive with
GNSS running is much of the time; a refusal is not an error and the
`%XMONITOR` figures still go out.
- **`CONFIG_APP_NCELLMEAS` (default n) reports what else the modem can
hear.**  Camped on -111 dBm while a neighbour reads -95 is a selection
problem wearing an antenna problem's clothes.  The count matters too: a weak
antenna pulls the serving cell and its neighbours down together, so levels
alone cannot separate it from a bad location, but marginal neighbours fall
below the detection floor and the list thins.  The scan is asynchronous, so
the wake waits for it rather than powering the modem off underneath it.
- New columns `pathloss`, `rsrq`, `ce_level`, `tx_rep`
(`sql/2026-09-23_log_conn_eval.sql`).  None are carried forward: they are
measurements of a moment, and NULL means no evaluation ran.

### A wake stops paying for a second it did not need
- **Registration is signalled, not polled.**  `wait_for_registration()` slept
a whole second before its first look and stepped in seconds after that, so
every wake paid at least 1 s however fast the modem actually was — a quarter
of a 4 s wake.  It now waits on a semaphore the LTE event handler gives the
moment registration arrives, and checks the status once before blocking in
case the URC beat it there.  The slices left in the loop only feed the
watchdog; a registration inside one ends the wait immediately.

### PLMN selection optimization is back on
- **`CONFIG_LTE_PLMN_SELECTION_OPTIMIZATION=y`** (NCS defaults it on; this
build had turned it off).  Its help calls it faster "especially for
stationary devices", which is what a parked car is, and the attach is where
a wake's time goes.  It had been disabled so the modem would re-evaluate
operators each connection rather than stick with the last one — a fix for EE
on B3 winning selection over O2/Vodafone on B20, which locking the band list
to 8 and 20 has since settled on its own.  `attach_ms` now measures the
difference directly, so this is falsifiable rather than a hunch.

### A diagnostic for whether PSM is worth adopting
- **`CONFIG_APP_PSM_PROBE` (default n) asks the network for PSM and reports
the answer.**  The modem is at CFUN=0 for the whole sleep with PSM disabled,
so every wake re-attaches from scratch; PSM would make a wake an RRC resume
while drawing what an off modem draws.  Whether that is available cannot be
read off a modem that has never asked — `AT%XMONITOR` reports the
network-provided Active-Time, but a UE that does not request PSM is never
assigned one, so the field is empty either way.  The probe requests PSM
before CFUN=1 and logs `LTE_LC_EVT_PSM_UPDATE` at WRN, so on an
`APP_DEBUG_LOG` build the answer reaches the server's device log without
needing a console: "granted" with the timers, or "refused" when the network
returns an active time of −1.  Repeats are demoted to INF.
- **It observes without adopting.**  Sleep still ends at CFUN=0, which
detaches long before the requested active time could expire, so PSM never
engages and neither sleep current nor wake behaviour moves; the request only
adds a few IEs to the attach.  The requested active time (60 s) is
deliberately clear of both the timed wake, which powers the modem off within
a second or two of its send, and the 30 s ignition-sleep cadence.  Safe to
enable on a deployed vehicle, which is the point — it is the one place the
question can currently be asked.

### A slow wake says why it was slow
Six wakes on 2026-09-23 ran 3.9 s to 44.6 s on one cell, same RAT, no GPS
involved.  The records themselves showed the send was not the problem — 0.51
to 0.72 s from the device building a record to the server storing it, on the
fast wakes and the slow ones alike — so all of the variance is before the
record exists.  That is the LTE attach, which every wake pays in full: the
modem is at CFUN=0 for the whole sleep and PSM and eDRX are disabled, so
there is no registration to resume.  Neither of these changes that; they make
it measurable.
- **`wt=` gained a third part: how much of the wake was the attach.**
`wt=<rid>:<total_ms>:<attach_ms>`, stored in the new `attach_ms` column
alongside `wake_ms`.  Timed from the CFUN=1 that started the search to the
registration event that ended it, not from `wait_for_registration()`'s return
— that polls at 1 Hz and sleeps a second before its first look, so every
attach read as at least 1 s and was rounded to the second.  The event handler
runs on the URC, so this is the modem's own timing.  0 means the modem was
already registered and the wake paid no attach.
- **Records carry the serving cell's signal quality.**  One `AT%XMONITOR`
after each attach yields `rsrp=` (dBm), `snr=` (dB) and `band=`, converted
from the 3GPP indices on the device since that mapping is fixed by the spec.
The command is free on the radio — it reads measurements the modem already
keeps for cell reselection, with no over-the-air transaction — and costs one
round trip per wake, not one per record.  The figures ride the existing cell
group, but only while the reading is that record's own: the group also goes
out on a cell change and on a record built from a stored position, and on
those there is no fresh reading, so an old one is dropped rather than sent as
if it were taken now (`SIGNAL_FRESH_MS`, and the cell-change event drops the
reading it invalidates).  Never carried forward by the server either, unlike
the cell identity: signal moves continuously, and a copied value would read
as a measurement that was never taken.  This is what
makes the obvious hypothesis testable — LTE-M raises its repetition count as
coverage worsens, so the same procedure on the same cell takes several times
longer at a lower RSRP.
- Schema: `sql/2026-09-23_log_attach_signal.sql`.  On a Traccar build the
attach rides as a `wakeAttachMs` attribute and the signal as `rssi`/`snr`/
`band`.

## 0.4.55

### A quiet tilt poll no longer wakes the console
- **The parked sleep loop leaves the console suspended when a pass has
nothing to do.**  With tow detection on (`APP_TOW_TILT_DEG`, default 6) the
sleep wait is capped at `APP_TOW_POLL_S`, so a parked unit wakes every 30 s
for the tilt check, and every one of those passes resumed the console UARTE
first.  An enabled UARTE holds the HF clock, so each poll showed on the input
current as a brief ~100 µA hump over the ~35 µA sleep floor, spent on a pass
that reads the accelerometer twice, reads the ignition pin and logs nothing.
A pass is now quiet when the wait timed out, no ignition or accelerometer
interrupt is latched, no resend is owed and no timed report is due; a quiet
pass runs with the console suspended, and skips the log drain and 2 ms settle
before the next wait.  Every branch a quiet pass can still turn into — tilt
over the threshold, the tow re-arm, the orientation tamper check, a polled
ignition change, the accelerometer's INT1 still high, the modem
power-off, the wake-cost line and key-on — resumes the console first,
so nothing it logs is lost.  The bit-banged accelerometer reads still run the
CPU for about 2 ms per poll, so the hump shrinks rather than disappears.  Not
yet measured on hardware.

## 0.4.54

### Every engine-off wake reports what it cost
- **`wt=<rid>:<ms>` carries a wake's duration and names the record it sent.**
Engine-off telemetry is where a parked unit spends its power — each wake pays
for a modem bring-up, a registration, a fix and a send, and one that cannot
register pays the whole `APP_NETWORK_SEARCH_TIMEOUT` before giving up — and
none of that was visible afterwards: the stored row said what was sent, never
what it took to send it.  The sleep loop now times from the wake to going
back to sleep with the modem off, spanning the `RESEND_POLL_S` passes of a
report that had to wait for registration, since that is exactly where a bad
wake's power goes.  The server files it against the record named, in the new
`wake_ms` column.
- **The figure lags at least one record by necessity.**  A wake cannot measure
itself: the device is still awake when it builds the record, and on a bad
wake most of the time comes after that record exists.  So it is finished once
the wake is over, held in `wake_pending`, and emitted on the next record of
any kind.
- **Every record now carries `rid=`, the device's own id for it.**
Consecutive within a boot, so a gap in it is a record that never arrived, and
seeded at random at each boot, so no two boots reuse the same ids.  It is
what lets the wake figure name its record instead of meaning "the one
before" — which is the wrong row in exactly the case the figure is most worth
having.  A wake whose own send failed leaves its record in the backlog, and
the backlog is flushed *behind* the live record that describes it; the server
holds the figure on the `device` row until that record lands.
- A wake that built no record at all has nothing to attribute the figure to,
so it is logged and dropped rather than filed against an unrelated record; a
wake that skipped its send on the battery gate is not measured at all.  On a
Traccar build the pair goes out as `wakeRecordId` and `wakeMs` attributes of
the record carrying them, since Traccar cannot amend a stored position.
- Schema: `sql/2026-09-22_log_wake_ms.sql` and `sql/2026-09-22_log_rec_id.sql`
(`migrations/` in the packaged server).  Not to be confused with the existing
`waketime` column, which despite its name holds seconds of uptime.

### A parked unit that cannot reach the network stops looking
- **`CONFIG_APP_NETWORK_SEARCH_TIMEOUT` (default 300 s, 0 = no limit) bounds
the search.**  With the ignition off, the radio is left looking for a network
for this long — counted from the moment it is brought up, so the 60 s
`APP_NETWORK_REGISTRATION_TIMEOUT` is part of it — and then powered off until
the next timed report.  Nothing else keeps a parked unit awake, so this is
what bounds the power it spends somewhere the network cannot be reached.
Before, a timed wake that missed registration left the modem searching until
the next wake, up to an hour (it was left up on purpose so the record could
go out when registration arrived, and now still is, for the length of the
window), and a boot with the key off idled in the main loop with GNSS and the
modem both searching until the network came, however long that took.  A key
that has been turned off again on a wake or a boot that never registered,
and a drive that lost the network and then parked, waited the same way; they
give up at the same point.  Anything recorded while waiting goes to the
backlog for the next send that gets through.  The search is not limited with
the ignition on: the car is running, and the drive should be reported as
soon as coverage returns.
- **No fix is attempted without a network to send it over.**  A timed wake
that could not register ran the receiver anyway when a fix was due — up to
the cold-start timeout, five minutes — for a record that could only go to the
backlog.  The search is left to the next wake that has a network: the no-fix
backoff (1st, 2nd, 4th, 8th, 16th wake, then every 16th) now counts only
wakes with a network, so the first of them searches, and a movement fix stays
wanted until then.  The same at boot: with no network and the key off the
receiver is not started, and the unit goes to sleep with the modem searching
for what is left of the window; the sleep loop sends the first record, and
runs the power-on update check, if the modem registers in time.
- **A wake with no stored position now polls for the registration it needs.**
The report was only marked as owed when a record had been built and its send
had failed, so a unit with no position since boot — which builds no record —
left the modem searching with nothing watching for the registration that
would have let it search for a fix.  The report is owed whenever the wake
ends unregistered.
- **The wake interval backs off while there is no network.**
`ENGINE_OFF_BOOT_INTERVAL` — the cadence from boot until the first server
reply sets one — is an hour rather than 900 s, and each consecutive timed
wake that cannot register doubles the wait until the next, up to
`CONFIG_APP_NO_SIGNAL_MAX_INTERVAL` (default 86400 s, 0 = no backoff): 1, 2,
4, 8 and 16 hours, then a day.  A unit somewhere with no coverage at all —
an underground car park, a container, a shipped spare — was waking on its
full cadence to spend a bring-up and a search window on a send that could
not happen.  The first registration puts it straight back to the configured
interval, and so does confirmed movement, since the likeliest reason
coverage returns is that the vehicle has been moved into it.  The server's
interval is never overwritten: the backoff is local, and by the time a
record reaches the server it has already been cleared, so the `int=` the
device reports stays the one it was given.
- **Key-off during an outage no longer floods the backlog.**  With the last
send failed and the ignition off, the send state went round again to report
the change — once a second, for the whole outage, each pass pushing another
copy of the ignition-off record into the backlog and thinning the drive's own
records out of it.  It now hands over to the idle state, which records the
change once, holds it for the send that follows registration, and gives up at
the timeout above.

## 0.4.51

### A stale reply no longer costs a warning
- **A tag that fails to verify is logged at debug, not warning.**  The
response AAD binds a reply to the nonce of the request it answers, so a reply
the server sent to an earlier datagram fails the tag check by construction,
and `transport.c` already treats that as a stale reply: it skips it and
listens on for the one it wants, warning only if nothing valid arrives by the
deadline.  But `crypto_decrypt()` warned on every failed check of its own, so
each skipped reply still sent `psa_aead_decrypt: -149` to the server — 60 on
the car, 42 of them within seconds of a registration outage ending, when the
replies to the sends made during the outage arrive together.  An
invalid-signature status is now debug-only; any other status, a key or buffer
problem, is still a warning, and the transport's own `response decrypt
failed` still fires when no reply authenticates.

## 0.4.50

### An hour of sleep is no longer logged as an outage
- **A deliberate modem power-off is not a lost registration.**  The car's
device log carried a pair of warnings for every timed wake on 2026-09-20:
`registered again after 3628 s (status 5)` as the modem came up, and a second
later `registration lost (status 0) — waiting for the modem` as it went back
down.  Both were the firmware's own doing.  A timed wake brings the modem up
with CFUN=1, sends the record and powers it off again with CFUN=0; the modem
answers that with a not-registered URC, and the LTE event handler — which
logs a loss at WRN so a real outage and its length reach the captured log —
could not tell it from the network going away.  The 3628 s was the 3600 s
loop interval plus the bring-up: every hour of sleep read as an hour without
a network, and a real hour-long outage would have looked the same.
`modem_power_off()` clears the handler's state before CFUN=0, so the URC is
logged as the plain status change it is and the next registration has
nothing to be measured against; sleep entry, the end of a timed wake, the
parked-impact branch and the pre-reboot power-off in the update path all go
through it.  A genuine outage still open when the modem is powered off gets
its closing line, `powering off still unregistered, N s after losing the
network`, so the log never shows a loss without an end.  Verified on the
bench: sleep entry now logs `nw reg status: 0` at INF and nothing at WRN.

## 0.4.49

### Threshold alerts on OBD telemetry
- **`CONFIG_APP_OBD_ALERTS` queues an alert when a PID crosses a line.**
A comma-separated rule list, `"coolant>=90,rpm>6500,stft<-12.5/2"`, judged
on the live snapshot after every poll (`src/obd_alert.c`), so a slow PID is
checked as soon as its rotation slot reads it rather than when the next
record is built.  One alert per crossing, at `APP_OBD_ALERT_PRIORITY`
(default 1), worded `coolant 92C (>= 90C)`; a clear at priority 0 once the
value is back across the threshold by the hysteresis, a twentieth of the
threshold unless the rule gives one.  Rules re-arm at key-off, not on a
mid-drive reopen.  A rule that does not parse disables the list with the
reason logged.  On a Traccar build a coolant or intake alert is a
`temperature` alarm.  See KWIRE.md, "Threshold alerts".

### A cell change on the motorway no longer costs a minute of tracking
Two dropouts on 2026-09-19's long drive, at 10:43 and 11:00, each held the
track for a minute or two on a motorway with a clear sky.  Both were the
firmware making a routine LTE cell change worse; the captured log shows how.
- **The registration retry timer measures the current outage.**  When the
modem loses registration the idle poll starts a timer and, after
`APP_NETWORK_RETRY_INTERVAL` (300 s), brings the radio up again.  The timer
was cleared only by the poll seeing registration return, but the LTE event
handler flips `network_ready` back on its own after every blip, and the poll
is then skipped — so the timer kept the start of some earlier outage.  Every
later blip fired the bring-up at once: at 10:43:10 the modem reported status
4 while reselecting on the motorway, and 5 s later the log reads `no
registration for 300s — bringing the radio up again`; the same again at 11:20
(3 s) and on 2026-09-18 at 20:12 (21 s).  It now resets whenever the unit is
registered.
- **`modem_radio_up()` leaves a modem that is already up alone.**  The
bring-up reapplies the link settings and CFUN=1, which is what a modem
reinitialised after a fault needs (it comes back at CFUN=0).  Issued to a
modem that is already in normal mode it is not idempotent: `+CGDCONT` is
refused while attached (`CGDCONT: 65536` in the log), and `+COPS=0` makes the
modem start PLMN selection over even in automatic mode (Nordic's own advice
for leaving an unwanted network) — the likeliest reason the 10:43 blip became
63 s without a network, and a 30 s hole followed the 11:20 one (the EMM cause
9 rejects around that one were the network's, and happened before the
bring-up too).  The comparable blips left alone (09:19, 10:14) cost under
15 s.  It now reads `+CFUN?` first and does nothing when the modem is in
normal mode; a modem that is up but not registering is left to its own
periodic search.
- **A starved warm search says so.**  At 10:59:41 the receiver had a fix;
then none for the whole warm wait while the modem worked through a cell
change.  Whether LTE's idle-mode work was starving the receiver of windows or
an RRC connection was blocking it outright is not in the log, because a warm
wait recorded neither.  Priority mode is not the answer there: it takes the
radio from the very measurements the modem needs to finish a cell change, and
Nordic's note on the call is to time it away from anticipated data transfer,
which a drive never is.  So a warm wait now logs its starved epochs, and the
timeout line carries what the receiver saw (satellites, blocked or not), so
the next one can be read.
- **The fix wait is a deadline.**  It counted down its one-second semaphore
waits and ignored the OBD poll that runs between them, so the 60 s warm wait
above took 88 s.  It now runs to a `k_uptime_get()`-based deadline.
- **A receiver that fixed within the last half hour searches before it asks
nRF Cloud.**  The restart after a warm timeout makes the next collect a cold
one, and a cold collect with an assistance request outstanding fetched it up
front, pausing the receiver for the whole exchange.  At 11:01:11 that fetch
ran 21 s into a TLS connect timeout on the very link that had just failed,
before the search even began; the retry from the wait succeeded in 5 s.  With
a fix younger than `AGNSS_FIX_FRESH_MS` (30 min) the ephemerides are still
good, so such a collect searches first and only fetches from the wait after
`AGNSS_RETRY_INTERVAL_MS` (15 s) without a fix.  A receiver that has really
lost its data — boot, a long stop — still fetches first.

### Recovery brings the radio up the same way everywhere
- **`modem_recover()`'s CFUN cycle and library restart go through
`modem_radio_up()`.**  Both set the APN and CFUN=1 and nothing else, so a
modem that came back through either registered without `%REL14FEAT` and
`%RAI`, quietly costing the GNSS duty cycle — the gap `modem_rescan_plmn()`
already closes after its own CFUN=4.  Neither path ran on 2026-09-19.

## 0.4.48

### Telemetry can go to a Traccar server
- **`CONFIG_APP_TRACCAR=y` reports to Traccar instead of the l0destar
server.**  `src/traccar.c` is built in place of `src/transport.c` and sends
the same lines — records, alerts, fault-code reports — as OsmAnd-protocol
HTTP requests to `APP_TRACCAR_HOST` on its OsmAnd port (`APP_TRACCAR_PORT`,
5055), the device identified by its IMEI or `APP_TRACCAR_ID`, over TLS when
`APP_TRACCAR_SEC_TAG` names a CA.  The record builder, batching, the backlog
and the alert queue are untouched.  Record fields arrive under Traccar's own
attribute names where it has one (`power`, `ignition`, `sat`, `versionFw`,
`rpm`, `coolantTemp`, the fuel trims unscaled ...) and their own otherwise;
alerts become positions with `alarm=` set from the wording (movement, tow,
tampering, accident, powerCut ...).  Traccar's *custom* command is returned
in the response and goes through the same command parser as the l0destar
server's, so `int=`, `movealarm=`, `locate`, `fota` and `reboot` work from
there.  See `TRACCAR.md` for the mapping and what Traccar cannot do:
settings from the server, track mode, update adverts, the device log.
- **One TCP connection is kept across sends.**  Release assistance drops the
RRC connection after each response, and closing the connection after that
would need a fresh one just for the FIN, so `transport_close()` keeps the
socket and only hints the release; `transport_teardown()` drops it, which
now happens before every `lte_lc_power_off()` (a no-op for the UDP
transport, whose socket is already closed by then).  A request on a
connection the server has meanwhile dropped is retried once on a new one.
- **A 4xx answer drops the report, a 5xx keeps it.**  400 is Traccar not
knowing the identifier; holding such a report in the backlog would retry it
forever and escalate the modem recovery for a link that is fine.
- **A build with no update server skips the update check** instead of
failing DNS on an empty hostname at every power-on: `APP_FOTA_HOST` empty
and `APP_SERVER_HOST` empty, as a Traccar build has them.

## 0.4.47

### A short key cycle no longer leaves the unit idling awake
- **IDLE sleeps when the ignition is off and nothing is owed.**  Leaving
sleep on the ignition line needs it on for only 200 ms, but IDLE then spends
seconds in `modem_connect()` before it polls the line again; a key that was
off again by then left no transition to act on, and since sleep was only
entered from the send state the unit sat polling with GNSS running until the
next timed record (or the next key).  Seen on the bench on 2026-09-16 with a
quick ignition pulse: awake and silent for minutes until the ignition was
poked again.  The same idle followed a routine collection that found no fix.
- **A key cycle that is over by then still reports once**, from the cached
position, before sleeping — so poking the ignition to get an update works
whatever the pulse length instead of needing the key held for a second or
two.

### A tracker on the inline battery backup keeps reporting
- **`CONFIG_APP_BACKUP_SUPPLY` (default `n`) declares the backup module.**
The module holds the rail at about 9 V while car power is absent, which is
under both timed-report gates (`APP_BATTERY_POWEROFF_MV` 11.8 V, after which
the next check is a day away, and `APP_SLEEP_SAFETY_MV` 12.0 V): a unit whose
power had just been cut would have gone silent for 24 hours, the opposite of
what the module is for.  With the option set, a reading between
`APP_BACKUP_MIN_MV` (8.5 V) and `APP_BACKUP_MAX_MV` (9.7 V) with the ignition
off is backup power: the timed wake reports as normal and the two gates are
bypassed.  Off by default because without the module a car battery in that
band is flat.
- **`backup power: 9.05V` goes out at priority 1, `car power restored: 12.60V`
when the rail returns.**  A cut supply on a parked car is a tamper signal, so
it carries the movement alarm's priority rather than the low battery's.  The
same 60 s ignition-off settle guard as the low battery alert keeps a crank
sag out of it, and the low battery alert itself stays quiet in the band.
- **A power cut wakes a sleeping tracker.**  Nothing about a cut reaches a
sleeping unit on its own - the wake sources are the timer, the accelerometer
and ignition-present - so revision F of the module lifts the ignition line to
about 12 V for ~0.4 s as its boost starts.  The sleep loop now latches the
ignition interrupt rather than trusting the pin level (which is also why a
quick key cycle used to need a second or two to take), acts on a pulse that
landed while it was busy before the next wait, and on an ignition wake reads
the rail first: in the backup band it is the module's pulse, so
the unit reports at once (the `backup power` alert goes with that record,
without the settle wait) and goes back to sleep; anything else is the key-on
it always was.  Without the pulse a cut is still found at the next timed
wake.

## 0.4.46

### A timed report that misses registration goes out when the modem registers
- **The sleep loop sends it instead of powering the modem off.**  A timed
wake gives up on registration after `APP_NETWORK_REGISTRATION_TIMEOUT` (60 s)
and leaves the radio searching, and the record goes to the backlog.  When the
modem registered a minute or two later, the next pass of the sleep loop
powered it straight off, so the record waited for the next timed wake an hour
on.  Observed on 2026-09-14: the 07:57 wake gave up, the modem registered at
08:00:31 and was powered off at 08:00:57, and the record arrived at 08:59; the
same happened at 02:49 and 04:51.  A report that failed for want of
registration is now owed: the loop polls every `RESEND_POLL_S` (30 s) and, once
the modem registers, reruns the timed report — a fresh record, the backlog and
the server's reply — before powering the modem off.

### A parked unit's records no longer carry a phantom speed
- **A speed goes out only with the fix it came from.**  Every timed check-in
while parked reuses the stored position, and it used to repeat that fix's
speed too, for as long as the unit slept: on 2026-09-14 the car's hourly
records carried 0.68 mph from the fix taken after its 06:56 reboot, and the
page showed 1 mph.  A fix older than `SPEED_FIX_MAX_AGE_MS` (10 s) now goes out
with speed 0.
- **Every ignition-off record reads a residual GNSS speed as stopped.**  The
`IGN_OFF_STOPPED_KMH` cut (3.22 km/h, 2 mph) applied only to the record where
the key turned, so a fresh fix taken while parked, such as the one after a
reboot, still reported the receiver's standstill noise.  Above the threshold a
roll-away or a tow reports what it measured.

### Connect Kit builds from a fresh clone see satellites
- **The GNSS antenna settings are committed.**  The Connect Kit's GNSS
amplifier is powered from the nRF9151's COEX0 pin, which the modem only drives
once the modem antenna library has sent `AT%XCOEX0`.  NCS turns that library
on only for Nordic's own boards, and the settings lived in the gitignored
`makerdiary.conf`, so a fresh clone built Connect Kit firmware that could not
see a satellite.  They now live in
`boards/nrf9151_connectkit_nrf9151_ns.conf`, which every Connect Kit build
applies.

### Bench harnesses no longer reset themselves
- **The watchdog is armed only in tracker builds.**  Since 0.4.36 a missed
feed really resets the SoC, and the board test, provisioning mode, K-wire
discovery, the CAN and K-line tests, the accel, voltage and L-sense streams,
the CAN bench agent and the LTE power test all wait in loops that never feed
it, so each reset about 35-45 s in.

### A unit with no position sleeps instead of searching awake
- **With the ignition off and no fix since boot, it goes to sleep.**  A record
needs a position and sleep was only entered after a send, so a unit that
started where GNSS could not see the sky, such as an underground car park
after a key-off update, stayed awake with GNSS searching until a fix turned
up.  Its timed wakes now search for a first fix on the 1st, 2nd, 4th, 8th and
16th wake and then every 16th.

### An update no longer interrupts a drive or retries in a loop
- **No update starts while the engine is running.**  The main loop ran any
pending check on every pass, so a download usually began mid-drive: GNSS and
telemetry stopped for minutes, then the unit rebooted while driving.  It now
waits for the engine to stop; key-off and timed wakes still check, and a
manual `fota` command waits too.
- **A failed download backs off.**  It only bumped the failure count, so the
`fota=` in the next reply re-armed the check, within half a minute while
driving, stopping GNSS each time.  It now backs off like any failed check, 10
minutes doubling to 80; the `fota` command still overrides it.
- **One download budget per check.**  `APP_FOTA_DOWNLOAD_TIMEOUT_S` was meant
to bound a whole check, but each attempt started its own deadline, so one
check could keep GNSS stopped for about 40 minutes.  Every attempt now shares
the check's deadline.
- **The failure alert counts the attempts made.**  `fota: x -> y failed after
N attempts` always gave the configured number, even when the budget stopped
the check sooner.
- **A reverted update is reported once.**  The record of a staged update
survives a warm reset, and nothing marked its revert as reported, so every
later warm reset raised `failed to boot` again and sent the server another
`F,fota,failed` line.  Later boots now only restore the refusal of a version
that has used up its attempts.
- **An inhibited build says so.**  With `APP_FOTA_INHIBIT` the `fota` command
replied `fota: check queued` and cleared the record of a version that had
failed to boot, though no check would run.  It now replies `fota: updates
inhibited` and leaves the record alone.

### A late registration no longer leaves the IMEI unset
- **The IMEI is read before connecting.**  It was read only inside a
successful start-up connect, so a modem that registered after the 60 s
start-up wait left it unset for the whole boot: every send was dropped with
`IMEI not set`, and the power-on update check asked for `imei=unknown`.
`AT+CGSN` needs no network, so it is now read as soon as the modem library
starts.

### Command replies match what the unit does
- **`locatenow` and `tomtomnow` take a fresh fix while awake.**  The send path
stops GNSS before it runs the commands in a reply, so their record waited out
the 60 s fix timeout on a stopped receiver and reported the last known
position.  With the ignition on they now resume GNSS first.
- **A build without track mode ignores `track=`.**  `APP_TRACK_MODE=n`
compiles the mode out, but a `track=1` in a reply still set the flag and sent
`track mode ON`.

### The K-wire L line is driven only for a vehicle that needs it
- **Discovery tries K alone first.**  It drove the L line whenever
`APP_L_SEND_ENABLED` allowed, the default on v3.3 and v3.4, and its summary
said `L wire driven` whenever that setting was on, so it could not show
whether a vehicle needed L.  Each 5-baud init now goes out on K alone, and
again with L only if that gets no answer; the summary says `L wire needed`
only then and suggests `CONFIG_APP_KLINE_USE_L=y`.  A sweep that finds nothing
on K alone is repeated with L, which takes about as long again.
- **The runtime session can drive L.**  `kline_session_open()` always opened
on K alone, so a vehicle that needed L would pass discovery and then never
open a session.  With `APP_KLINE_USE_L` it drives L as well, after the same
short-to-battery test.

### The bench tools no longer leave a board drawing milliamps asleep
- **`reset.sh` does a pin reset.**  Its soft reset left the nRF9151 in debug
interface mode, drawing milliamps asleep until a pin reset or power cycle.
USB now re-enumerates on every reset, as it does after `flash.sh`.
- **`ifmcu/build.sh` refuses a clone without the whole fix.**  It builds only
a Makerdiary clone that contains PR #20's merge and has no local changes.  An
older version of the script patched the clone itself, which made a clone from
before #20 look fixed.

## 0.4.43

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
