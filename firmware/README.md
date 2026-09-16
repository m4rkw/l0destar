# l0destar firmware

**Before building, installing or relying on any of this, read the [project disclaimer](../DISCLAIMER.md).**

Automotive telemetry tracker firmware for the **Nordic nRF9151** SiP (LTE-M +
GNSS), built on Zephyr RTOS via the nRF Connect SDK (NCS). It reports position,
speed, battery and ignition state to a back-end over an encrypted link, sleeps
deeply when the vehicle is parked, and wakes on movement, ignition, or a timer.

> Ported from the original Arduino/Polaris `*.ino` firmware. Where a module
> replaces a legacy one, its header comment notes the original source file.

---

## Hardware

| Function          | Part / detail |
|-------------------|---------------|
| MCU / modem / GNSS | nRF9151 SiP (LTE-M, integrated GNSS receiver) |
| IMU               | ASM330LHHX (automotive 6-axis, WHO_AM_I 0x6B) over bit-banged I²C - accelerometer for movement detection, the wake interrupt and impacts; gyro in telemetry and impact alerts; FIFO as a ring of recent samples; MLC/FSM unused |
| Voltage monitor   | INA228 over the same I²C bus - battery / ignition-derived voltage |
| Diagnostics       | K-wire (ISO 14230-1 K-line, KWP2000) via TJA1027T on v3.x (L9637D on v2.x); 5-baud address bit-banged, data bytes on UARTE1 — see [KWIRE.md](KWIRE.md) |
| Ignition sense    | GPIO input (MOSFET-gated 3.3 V rail) |
| Status LED        | board `led0` alias (LED1 on the nRF9151 DK) |

Builds target the **Makerdiary nRF9151 Connect Kit** (`nrf9151_connectkit/nrf9151/ns`),
which every l0destar carrier board is built around; `PROFILE=dk` builds for the
nRF9151 DK instead. Pin assignments come from `Kconfig.boards` and are given
names in `src/pins.h`; DK-specific peripheral conflicts are resolved in
`boards/nrf9151dk_nrf9151_ns.overlay` (+ `.conf`).

### Carrier boards

Every l0destar PCB carries the same Connect Kit but lands each signal on a
different header pin and wires the switched power rails differently.
`Kconfig.boards` defines one selectable definition per PCB - GPIO map,
fitted-hardware flags, and power-domain topology, all extracted from the KiCad
designs in `../hardware/`. Select the board in `local.conf` and everything
else follows:

```
CONFIG_APP_BOARD_L0DESTAR_V3_0=y
```

Available: `APP_BOARD_BENCH`, `..._L0DESTAR_V2_1`, `..._V2_1_MINI`,
`..._V2_5_CAN/_KLINE/_MICRO`, `..._V2_6_CAN/_KLINE/_MICRO`, `..._V3_0`,
`..._V3_1`, `..._V3_2`, `..._V3_3`, `..._V3_4` (default).
Individual pins/flags can still be overridden after the board defaults apply
(on v3.0, set `CONFIG_APP_BOARD_HAS_CAN=n` or `..._HAS_KLINE=n` to match the
jumper configuration of the physical board).

Selecting a board also arms the **power-domain sequencing** in
`src/hw_domain.c`. Signals that terminate inside a switched rail (CAN SPI
pins, K-line pins) are parked as input+pulldown whenever that
rail is off and only released while it is powered - a pin driven high into an
unpowered MCP2518FD/TXS0104E would backfeed the dead rail through its ESD
clamp diodes (abs max VDD + 0.3 V), and the CAN_INT/CAN_CS/K-line pull-ups
live on the switched rails, so they float when the domain is down. On boards
with transceiver standby control (v2.5C/v2.6C, and v3.0 onwards),
`src/hw_can.c` configures MCP2518FD `IOCON.XSTBYEN` at boot so sleep mode
automatically drops the transceiver into standby - required on v2.5/v2.6
where the CAN rail shares the GPS AUX domain and stays powered during
engine-off telemetry wakes.

---

## Architecture

`main()` initialises the peripherals, modem, GNSS and watchdog, then runs a
state machine:

```
            ┌───────────────────────────── engine on ──────────────┐
            ▼                                                        │
  IDLE ──► GPS_COLLECT ──► SEND ──┬─► (ignition off) ─────► SLEEP    │
   ▲                              ├─► (engine off)  ─► IGNITION_SLEEP┘
   └──────────────────────────────┘
```

- **IDLE** – wait for network registration, poll ignition + battery, decide when to send.
- **GPS_COLLECT** – acquire a GNSS fix, build and buffer a telemetry record.
- **SEND** – transmit batched telemetry over encrypted UDP, process any server command.
- **IGNITION_SLEEP** – ignition on but engine off: periodic sends, watch for engine start / ignition off.
- **SLEEP** – ignition off: peripherals powered down; wakes on the IMU's movement interrupt, ignition change, or telemetry timer.

Extras handled in the loop: **movement alarm** with escalating cooldowns,
**coast-to-stop** (keep reporting briefly after ignition-off while still
rolling), and **modem recovery**: registered but unable to send for
`APP_MODEM_STUCK_CFUN_S` (10 min) brings a CFUN cycle, and for
`APP_MODEM_STUCK_RESET_S` (30 min) a modem library restart. Losing coverage is
left to the modem.

### Impact detection

While awake the IMU runs at ±8 g with a high-g interrupt
(`APP_CRASH_THRESHOLD_MG`, default 4 g - set ~1200 in `local.conf` and rap the
desk to bench-test) and batches accel+gyro at 26 Hz into the chip's 3 KB FIFO
ring (~9 s of history). On impact, the ring is drained and an alert is sent
with peak g (per-axis), peak rotation rate, disturbance duration, and speed;
the ±0.5 s waveform around the peak is dumped to the serial log. While asleep,
a short, sharp hit above `APP_IMPACT_IMMEDIATE_MG` (default 700 mg) is reported
at once, and an unconfirmed movement wake whose peak exceeds
`APP_PARKED_IMPACT_MG` (default 800 mg) raises a `parked impact` alert instead
of being ignored. Telemetry `ax/ay/az` are **milli-g** (FS-independent);
`gx/gy/gz` are bias-corrected LSB at ±250 dps.

### Source layout (`src/`)

| File | Description |
|---|---|
| `main.c`      | Entry point, state machine, sleep/wake, movement + coast logic |
| `modem.c`     | LTE-M bring-up, registration, APN, RAI, cell-info tracking, error recovery |
| `gnss.c`      | GNSS fixes via the nRF9151's built-in receiver (`nrf_modem_gnss`) |
| `agnss.c`     | A-GNSS assistance from **nRF Cloud REST** (device-JWT auth) |
| `transport.c` | UDP telemetry, each datagram sealed with **ChaCha20-Poly1305** under the device key |
| `data.c`      | Telemetry CSV record builder (position, speed, battery, ignition, accel) |
| `commands.c`  | Server command dispatch (`key=value[,…]`) |
| `alert.c`     | Movement/event alert queue (piggybacks on sends, or standalone) |
| `settings.c`  | Runtime settings (in-memory; Kconfig-backed defaults) |
| `crypto.c`    | ChaCha20-Poly1305 through PSA Crypto, CSPRNG, PSK hex parsing |
| `hw_common.c` | GPIO init + bit-banged I²C bus |
| `hw_domain.c` | Switched power-domain sequencing (park/release of pins in AUX/OBD/K domains) |
| `hw_power.c`  | INA228 voltage, ignition read, INA shutdown/wake, AUX domain wrappers |
| `hw_can.c`    | MCP2518FD power/domain handling, sleep mode + transceiver standby (XSTBY) |
| `hw_accel.c`  | ASM330LHHX IMU (accel path): polling + hardware wake interrupt |
| `hw_kline.c`  | K wire: 5-baud init bit-banged, data bytes on UARTE1; vehicle discovery and the runtime session ([KWIRE.md](KWIRE.md), [KWIRE_QUICKSTART.md](KWIRE_QUICKSTART.md)) |
| `kline_obd.c` | OBD-II over the K wire: PID polling into telemetry, fault codes, engine/speed for the tracker's own logic; the fast rotating poll for track mode ([TRACK_MODE.md](TRACK_MODE.md)) |
| `fota.c`      | Over-the-air updates: manifest check, battery gate, MCUboot image download ([FOTA.md](FOTA.md)) |
| `databuf.c`   | Backlog of records that could not be sent, kept until the link comes back |
| `dbglog.c`    | Keeps warnings and errors in RAM and sends them with the next record that gets through |
| `fatal.c`     | Fatal error handler: reboots instead of halting, leaving a note for the next record's `rst=` field |
| `hw_selftest.c` | Boot-time switched-rail self-test (v3.1+) |
| `board_test.c` · `lte_power_test.c` · `can_bench.c` | Bench builds only: the interactive board test run by `board_test.sh` · LTE transmit power and brown-out rig · CAN bench-test agent |
| `led.c` · `watchdog.c` · `reboot.c` | Status LED · 32 s task watchdog (HW fallback) · reboot helper |
| `config.h` · `pins.h` · `app.h` · `ca_cert.h` | Compile-time defaults · names for the `APP_PIN_*` pins · shared API/state · server CA cert, generated from `certs/ca.crt` by `build.sh` and not in git |

---

## Connectivity & telemetry

Telemetry is **plain UDP**, each datagram sealed with **ChaCha20-Poly1305**
under the device's 32-byte key, `CONFIG_APP_PSK_HEX` (`transport.c`,
`crypto.c`). The server holds the same key for the device's IMEI, so a datagram
that decrypts is authenticated as well as private.

- Endpoint: `CONFIG_APP_SERVER_HOST` : `CONFIG_APP_SERVER_PORT` (**65480**).
- Request: `[1] IMEI length` `[n] IMEI` `[12] nonce` `ciphertext` `[16] tag`, with
  the IMEI as associated data. Response: `[12] nonce` `ciphertext` `[16] tag`,
  bound to the IMEI and the request's nonce. See
  [PROTOCOL.md](../server/docs/PROTOCOL.md).
- `modem.c` enables release assistance (`AT%RAI`), so the radio is released
  soon after each exchange and GNSS gets the shared antenna back.

The CA in `src/ca_cert.h` is for updates only: `modem.c` writes it into the
modem on first boot, and the update server's certificate is checked against it.

Each telemetry record is one CSV line built in `data.c` (timestamp, lat, lon,
speed, altitude, heading, HDOP, satellites, battery, ignition, uptime and
power-on flag, then extras such as the accelerometer;
[PROTOCOL.md](../server/docs/PROTOCOL.md) lists them all). Records batch by `BATCH_SIZE` (`CONFIG_APP_BATCH_SIZE`,
default 3 while driving: each send costs an RRC connection and a GNSS
re-acquisition, so three records a datagram roughly doubles the record rate
for a page update every few seconds; ignition changes and settings syncs
still flush at once).

---

## GNSS & A-GNSS (nRF Cloud)

GNSS fixes come from the nRF9151's built-in receiver. A cold fix with no cached
ephemeris can take 2–5 minutes (it time-shares the antenna with LTE), so the
firmware optionally fetches **A-GNSS** assistance from nRF Cloud to speed up
time-to-first-fix.

`agnss.c` authenticates to nRF Cloud's REST API with a **per-device JWT** signed
by a key in the modem at `CONFIG_NRF_CLOUD_SEC_TAG` (default `16842753`). nRF
Cloud only accepts that JWT once the device has been **onboarded** to your
account - otherwise the request fails with `401 / 40100 "Auth token is
malformed"`. See **[nRF Cloud device provisioning](#nrf-cloud-device-provisioning)**.

> A-GNSS is an optimisation, not a dependency: GNSS still cold-fixes without it.

---

## Building

Requires [nRF Connect SDK **v3.3.0**](https://docs.nordicsemi.com/) where nRF
Util's sdk-manager installs it: `/opt/nordic/ncs/v3.3.0` on macOS, `~/ncs/v3.3.0`
on Linux (override with `NCS_ROOT`). Builds target the Connect Kit; `PROFILE=dk`
builds for the nRF9151 DK, and `BOARD` overrides the target.

```sh
./build.sh            # incremental build  -> build/merged.hex
./build.sh pristine   # clean rebuild
```

`build.sh` runs `west build` inside the NCS toolchain. It layers `local.conf`,
and stops if there is none unless `LOCAL_CONF` is set empty; `PROV=1` adds
`prov.conf` on top.

### Configuration layers

| File | Committed | Purpose |
|---|---|---|
| `prj.conf` | yes | Zephyr/NCS subsystem config (modem, sockets, crypto, logging, PM) |
| `Kconfig`  | yes | Application symbols + defaults (see table below) |
| `boards/nrf9151dk_nrf9151_ns.{conf,overlay}` | yes | Board-specific Kconfig + devicetree (disables conflicting DK peripherals) |
| `boards/nrf9151_connectkit_nrf9151_ns.conf` | yes | Connect Kit builds: turns on the modem antenna library and sends `AT%XCOEX0`, which powers the GNSS amplifier |
| `local.conf` | **no** (git-ignored) | Per-deployment secrets / overrides |
| `prov.conf`  | yes | Provisioning-build overlay (AT-host bridge) - enabled via `PROV=1` |

The full list, with `remote.conf`, `sysbuild.conf` and `pm_static.yml`, is in
[firmware build options](../docs/reference/firmware.md).

### local.conf

`local.conf` is git-ignored; create it with at least the board, the server, the
APN and the device key:

```
CONFIG_APP_BOARD_L0DESTAR_V3_4=y
CONFIG_APP_SERVER_HOST="tracker.example.com"
CONFIG_APP_APN="iot.example.net"
CONFIG_APP_PSK_HEX="<64 hex characters>"
```

Any `APP_*` symbol from `Kconfig` can be overridden here (see the reference
table). `CONFIG_APP_PSK_HEX` is required: the server decrypts this device's
telemetry with the same key, which it is given when the device is enrolled
(`openssl rand -hex 32` makes one).

### Flashing

```sh
./flash.sh                          # program over the Connect Kit's CMSIS-DAP probe
./reset.sh                          # reboot without reflashing
```

Both use pyocd and end with a pin reset: pyocd's default soft reset leaves the
nRF9151 in debug interface mode, drawing milliamps while asleep.
[QUICKSTART.md](QUICKSTART.md) explains why they also stop pyocd erasing a
protected part.

---

## Over-the-air updates

The build carries **MCUboot** (`sysbuild.conf`), which splits the 1 MB flash
into two 416 KB slots pinned by `pm_static.yml`. `fota.c` checks for an update
at power-on, when sent the `fota` command, and at the next safe moment after a
telemetry reply advertises a newer version (`fota=<version>`). It fetches its
own manifest from the update server (`CONFIG_APP_FOTA_HOST`, by default the
telemetry host) on TCP 65481 over TLS, and installs a newer image built for its
board - provided the battery is above `CONFIG_APP_FOTA_MIN_BATTERY_MV`
(12.0 V). A swapped image that never finishes booting is rolled back
automatically.

The firmware version lives in one place, the `VERSION` file, which holds
`MAJOR.MINOR`; `build.sh` adds the patch number, and the result feeds
`<zephyr/app_version.h>`, the MCUboot image header and the version comparison.
`push_fw.sh` derives the patch number from what the server has already
published, so bump the minor version to restart the count.

> The first build with MCUboot has to be flashed over SWD - a unit running a
> pre-MCUboot image has no bootloader to swap slots.

See **[FOTA.md](FOTA.md)** for the manifest format, the server requirements
(range requests are mandatory over TLS), the release procedure and the signing
key.

---

## nRF Cloud device provisioning

One-time per device, to enable A-GNSS. The device's key/cert are written to
**modem NVM**, so they survive reflashing the application. No separate
`at_client` sample is needed - a provisioning build of *this* firmware acts as
the AT bridge.

**Prerequisites**

- An nRF Cloud account and its **REST API key** (nrfcloud.com → User Account).
- [`nrfcloud-utils`](https://pypi.org/project/nrfcloud-utils/): `uv tool install --python 3.12 nrfcloud-utils`

**1. Create a device CA (once)**

```sh
mkdir -p onboarding
create_ca_cert -c GB -o l0destar -p onboarding -f l0destar
```

Writes `onboarding/*_ca.pem` / `*_prv.pem` (git-ignored - the key is secret).

**2. Build & flash the provisioning firmware**

```sh
PROV=1 BUILD_SUBDIR=build_prov ./build.sh
pyocd load -t nrf91 --no-reset build_prov/merged.hex
pyocd reset -t nrf91 -m hw -O auto_unlock=false
```

It boots into "PROVISIONING MODE", with the AT host on the console port. The
build goes into its own directory, leaving your normal `build/` alone, and the
two pyocd commands are what `flash.sh` does, pointed at this build.

`prov.conf` enables `CONFIG_AT_HOST_LIBRARY`, disables logging (clean UART), and
`main()` idles after `modem_init()` so the AT exchange isn't corrupted.

**3. Free the serial port, then install credentials**

> ⚠️ Any serial monitor holding the port (e.g. a `cat`/logger daemon) will
> race the installer and corrupt the CSR. Stop it first.

```sh
device_credentials_installer \
  --port /dev/cu.usbmodemXXXX --cmd-type at \
  --ca     onboarding/*_ca.pem \
  --ca-key onboarding/*_prv.pem \
  --id-imei --id-str nrf- \
  -S 16842753 -d \
  --csv onboarding/onboard.csv --verify
```

`--id-imei --id-str nrf-` produces device ID `nrf-<IMEI>` and `-S 16842753`
matches the firmware's JWT sec-tag - both **must** match or nRF Cloud rejects
the JWT.

**4. Register the device to your account**

```sh
nrf_cloud_onboard --api-key "$NRF_CLOUD_API_KEY" --csv onboarding/onboard.csv
# verify:
curl -s "https://api.nrfcloud.com/v1/devices" -H "Authorization: Bearer $NRF_CLOUD_API_KEY"
```

**5. Reflash the normal firmware**

```sh
./flash.sh                          # build/ still holds the tracker build
```

On the next cold boot the A-GNSS fetch authenticates with the device JWT and
returns assistance data (`agnss: received … bytes` → `A-GNSS data injected`).

---

## Configuration reference (`Kconfig`)

| Symbol | Default | Meaning |
|---|---|---|
| `APP_LOG_LEVEL` | 3 | App module log level (0=off … 4=dbg) |
| `APP_PROVISION_MODE` | n | Build as the AT-host provisioning bridge (set via `prov.conf`) |
| `APP_PIN_*` | PCB map | Every signal's P0.x GPIO (K-line, I²C, IMU INTs, ignition) - remap per-board in `local.conf` |
| `APP_DEBUG_IGNITION` | -1 | Force ignition state (0=ON, 1=OFF, -1=live GPIO) |
| `APP_DEBUG_BATTERY_MV` | 0 | Force battery voltage in mV (0=live INA228) |
| `APP_CRASH_THRESHOLD_MG` | 4000 | Impact alert threshold while awake (mg) |
| `APP_PARKED_IMPACT_MG` | 800 | Parked-impact threshold from FIFO peak (mg) |
| `APP_IMPACT_IMMEDIATE_MG` | 700 | Parked hit reported without waiting for the movement confirm (mg, 0 = never) |
| `APP_DEMO_MODE` | n | Mask lat/lon in the serial log (public demos); telemetry unaffected |
| `APP_SERVER_HOST` | "" | Telemetry hostname (else `HOSTNAME` in `config.h`) |
| `APP_SERVER_PORT` | 65480 | Telemetry UDP port |
| `APP_APN` | "" | Cellular APN (else `DEFAULT_APN`) |
| `APP_PSK_HEX` | "" | Device key, 64 hex characters: required, and must match the server's |
| `APP_ENGINE_OFF_LOOP_INTERVAL` | 0 | Engine-off wake interval until the server sets one (s; 0 = 900) |
| `APP_IGNITION_ON_SLEEP_INTERVAL` | 30 | Send cadence: ignition on, engine off (s) |
| `APP_VOLTAGE_POLL_INTERVAL` | 5 | Battery sample cadence in IDLE (s) |
| `APP_BATTERY_CHECK_INTERVAL` | 86400 | Battery check during deep sleep (s) |
| `APP_NETWORK_REGISTRATION_TIMEOUT` | 60 | Registration timeout (s) |
| `APP_NETWORK_RETRY_INTERVAL` | 300 | Network retry interval (s) |
| `APP_GPS_FIX_TIMEOUT_MS` | 60000 | GNSS fix timeout (ms) |
| `APP_GPS_COLD_FIX_TIMEOUT_MS` | 300000 | Cold-start fix timeout (ms) |
| `APP_BATTERY_WARNING_MV` / `_POWEROFF_MV` | 11900 / 11800 | Battery warning / power-off (mV) |
| `APP_SLEEP_SAFETY_MV` | 12000 | Skip-send threshold while sleeping (mV) |
| `APP_ENGINE_RUNNING_MV` | 13000 | Engine-running voltage threshold (mV) |
| `APP_BACKUP_SUPPLY` | n | Inline battery backup module fitted: 8.5-9.7 V (`APP_BACKUP_MIN_MV`/`_MAX_MV`) with the ignition off is backup power, not a flat battery |
| `APP_ACC_MOVEMENT_THRESHOLD` | 150 | Movement delta threshold (milli-g) |
| `APP_MOVEMENT_CONFIRM_MS` / `_HITS` | 10000 / 6 | Movement confirmation window / samples |
| `APP_MOVEMENT_INACTIVITY_RESET` | 1800 | Inactivity reset timer (s) |
| `APP_MOVEMENT_ALARM` | n | Enable movement alarm |
| `APP_COAST_STOP_SPEED_KMH_X10` | 50 | Coast-to-stop speed threshold (km/h ×10) |
| `APP_COAST_MAX_ITERATIONS` | 60 | Coast-to-stop max iterations |
| `APP_BATCH_SIZE` | 3 | Records per datagram while driving |
| `APP_MODEM_STUCK_CFUN_S` | 600 | Registered but unable to send this long: CFUN cycle (s) |
| `APP_MODEM_STUCK_RESET_S` | 1800 | Registered but unable to send this long: modem restart (s) |

---

## Logging / serial

The console and logs are on the first of the Connect Kit's two USB serial ports
(the J-Link VCOM on the DK) at **115200 baud, 8-N-1**. macOS resets a serial
port to 9600 baud whenever it is opened, so use a terminal that holds the port
open, such as `screen /dev/cu.usbmodemXXXX 115200`.
