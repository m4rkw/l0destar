# Changelog

## Unreleased

### Sleep-state power
- Console UARTE is suspended for the duration of the blocking wait in
  `do_sleep()` and resumed on every wake (`CONFIG_PM_DEVICE`).  An enabled
  UARTE holds the nRF91 HF clock even with no traffic (~600-900 µA at 3.3 V),
  which accounted for the bulk of the ~300 µA parked input draw measured on
  v2.5K; the expected parked floor is now ~40-55 µA at the 12 V input.  All
  logging while awake is unaffected — the deferred log queue is drained
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
  (MCP2518FD abs max is VDD + 0.3 V — a high pin would backfeed the dead
  rail through the clamp diodes), CAN_INT/CAN_CS whose 10K pull-ups sit on
  the switched rail, K-line pins (TXS0104E A-port / TJA1027T with switched
  pull-ups), and v2.1 relay feedback (supplied from the AUX domain)
- Domains are reference-counted per board topology: v2.x single AUX domain,
  v2.5K/v2.6K AUX + K_EN (pins released only with both rails up), v3.0
  GPS_ENABLE and OBD_ENABLE switched separately — engine-off telemetry
  wakes now power only the GPS bias rail, and sleep wakes re-enable it
  (previously the aux rail stayed off after the first sleep, killing GPS)
- v3.0 TJA1027T put to sleep (SLP_N low) before its rail is cut

### CAN controller power handling (`src/hw_can.c`)
- MCP2518FD verified and configured at boot, then left in sleep mode
  (~10 uA); `IOCON.XSTBYEN` drives the transceiver standby pin from sleep
  state on v2.5C/v2.6C (TCAN334 STB otherwise floats — it has no other
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
  per-device JWT — cold TTFF drops from minutes to ~10 s
- Device onboarding to nRF Cloud without external sample firmware: `PROV=1`
  build (`prov.conf`, `APP_PROVISION_MODE`) turns the app into an AT-host
  bridge for `nrfcloud-utils`; credentials persist in modem NVM
- GNSS cold-start handling: extended timeout, periodic priority-mode windows,
  restart on cold-fix timeout

### Telemetry
- Gyroscope enabled (104 Hz ±250 dps) — `gx/gy/gz` (raw LSB) per record
- nRF9151 SiP die temperature — `mt` (°C) per record, via `AT%XTEMP`
- IMU die temperature — `it` (°C) per record
- Accelerometer fields `ax/ay/az` now in milli-g (FS-independent; previously
  raw LSB at ±2 g)
- Record batching support (`BATCH_SIZE`)
- Low-battery warning gated to ignition-off and demoted to normal priority —
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
  wakes are classified by true FIFO peak — `parked impact` alert above
  `APP_PARKED_IMPACT_MG` (default 0.8 g) instead of being silently ignored
- 100 ms settle after accel full-scale changes, fixing a spurious wake
  interrupt (and false impact) fired by the sensor's slope filter on every
  sleep entry

### Theft detection (while parked/asleep)
- Tow/jack: gravity vector polled against a sleep-entry reference every
  `APP_TOW_POLL_S` (30 s); sustained tilt past `APP_TOW_TILT_DEG` (6°) raises
  a `tow/jack` alert — catches slow, vibration-free flatbed lifts and jacking
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
  defaults — any signal remaps from `local.conf`; every pin has a defined
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
