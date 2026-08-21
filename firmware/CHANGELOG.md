# Changelog

## Unreleased

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
