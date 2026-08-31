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
| IMU               | ASM330LHHX (automotive 6-axis, WHO_AM_I 0x6B) over bit-banged I²C - accel-only today: movement detection + hardware wake interrupt; gyro/MLC/FSM/FIFO unused |
| Voltage monitor   | INA228 over the same I²C bus - battery / ignition-derived voltage |
| Diagnostics       | K-line (ISO-9141) via L9637D, bit-banged UART |
| Ignition sense    | GPIO input (MOSFET-gated 3.3 V rail) |
| Status LED        | board `led0` alias (LED1 on the nRF9151 DK) |

Reference board is the **nRF9151 DK** (`nrf9151dk/nrf9151/ns`); production runs
on a custom carrier around the MakerDiary nRF9151 Connect Kit module. Pin
assignments live in `src/pins.h`; DK-specific peripheral conflicts are resolved
in `boards/nrf9151dk_nrf9151_ns.overlay` (+ `.conf`).

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

Available: `APP_BOARD_BENCH` (default), `..._L0DESTAR_V2_1`, `..._V2_1_MINI`,
`..._V2_5_CAN/_KLINE/_MICRO`, `..._V2_6_CAN/_KLINE/_MICRO`, `..._V3_0`,
`..._V3_1`, `..._V3_2`, `..._V3_3`.
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
with transceiver standby control (v2.5C/v2.6C TCAN334, v3.0 MAX33041),
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
- **SEND** – transmit batched telemetry over DTLS, process any server command.
- **IGNITION_SLEEP** – ignition on but engine off: periodic sends, watch for engine start / ignition off.
- **SLEEP** – ignition off: peripherals powered down; wakes on LSM6DSO movement interrupt, ignition change, or telemetry timer.

Extras handled in the loop: **movement alarm** with escalating cooldowns,
**coast-to-stop** (keep reporting briefly after ignition-off while still
rolling), and **progressive modem recovery** (power-cycle → sleep) on repeated
send failures.

### Impact detection

While awake the IMU runs at ±8 g with a high-g interrupt
(`APP_CRASH_THRESHOLD_MG`, default 4 g - set ~1200 in `local.conf` and rap the
desk to bench-test) and batches accel+gyro at 26 Hz into the chip's 3 KB FIFO
ring (~9 s of history). On impact, the ring is drained and an alert is sent
with peak g (per-axis), peak rotation rate, disturbance duration, and speed;
the ±0.5 s waveform around the peak is dumped to the serial log. While asleep,
an unconfirmed movement wake whose peak exceeds `APP_PARKED_IMPACT_MG`
(default 1.5 g) raises a `parked impact` alert instead of being ignored.
Telemetry `ax/ay/az` are **milli-g** (FS-independent); `gx/gy/gz` are raw LSB
at ±250 dps.

### Source layout (`src/`)

| File | Description |
|---|---|
| `main.c`      | Entry point, state machine, sleep/wake, movement + coast logic |
| `modem.c`     | LTE-M bring-up, registration, APN, RAI, cell-info tracking, error recovery |
| `gnss.c`      | GNSS fixes via the nRF9151's built-in receiver (`nrf_modem_gnss`) |
| `agnss.c`     | A-GNSS assistance from **nRF Cloud REST** (device-JWT auth) |
| `transport.c` | **DTLS-over-UDP** telemetry, offloaded to the modem |
| `data.c`      | Telemetry CSV record builder (position, speed, battery, ignition, accel) |
| `commands.c`  | Server command dispatch (`key=value[,…]`) |
| `alert.c`     | Movement/event alert queue (piggybacks on sends, or standalone) |
| `settings.c`  | Runtime settings (in-memory; Kconfig-backed defaults) |
| `crypto.c`    | CSPRNG + PSK hex parsing (PSK used only by the legacy transport) |
| `hw_common.c` | GPIO init + bit-banged I²C bus |
| `hw_domain.c` | Switched power-domain sequencing (park/release of pins in AUX/OBD/K domains) |
| `hw_power.c`  | INA228 voltage, ignition read, INA shutdown/wake, AUX domain wrappers |
| `hw_can.c`    | MCP2518FD power/domain handling, sleep mode + transceiver standby (XSTBY) |
| `hw_accel.c`  | ASM330LHHX IMU (accel path): polling + hardware wake interrupt |
| `hw_kline.c`  | K-line bit-bang UART (L9637D) |
| `fota.c`      | Over-the-air updates: manifest check, battery gate, MCUboot image download ([FOTA.md](FOTA.md)) |
| `led.c` · `watchdog.c` · `reboot.c` | Status LED · 32 s task watchdog (HW fallback) · reboot helper |
| `config.h` · `pins.h` · `app.h` · `ca_cert.h` | Compile-time defaults · pins · shared API/state · server CA cert |

> `transport 2.c` (legacy ChaCha20-Poly1305 UDP envelope) and `stubs.c` are
> **not** in `CMakeLists.txt` and are not built.

---

## Connectivity & telemetry

Telemetry uses **DTLS 1.2 over UDP**, with the handshake, encryption and
certificate verification offloaded to the modem (`transport.c`). The
application sends/receives plaintext; the server is authenticated by
certificate (`TLS_PEER_VERIFY_REQUIRED`) against the CA in `src/ca_cert.h`,
which `modem.c` provisions into the modem at **`TLS_SEC_TAG = 1`** on first boot.

- Endpoint: `CONFIG_APP_SERVER_HOST` : `DTLS_PORT` (**65482**, fixed in `config.h`).
- Datagram: `[2-byte big-endian length] [IMEI "\n"] [CSV record(s)]`.
- DTLS connection ID + session caching abbreviate later handshakes; `SO_RAI`
  hints release the radio after each exchange so GNSS can use the antenna.

Each telemetry record is one CSV line built in `data.c` (timestamp, lat, lon,
speed, altitude, heading, HDOP, satellites, battery, ignition, uptime,
accelerometer). Records batch by `BATCH_SIZE` (default 1).

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

Requires [nRF Connect SDK **v3.3.0**](https://docs.nordicsemi.com/) installed at
`/opt/nordic/ncs/v3.3.0` (override with `NCS_ROOT`). Default board is
`nrf9151dk/nrf9151/ns` (override with `BOARD`).

```sh
./build.sh            # incremental build  -> build/merged.hex
./build.sh pristine   # clean rebuild
```

`build.sh` runs `west build` inside the NCS toolchain. It auto-overlays
`local.conf` if present, and layers `prov.conf` when `PROV=1` is set.

### Configuration layers

| File | Committed | Purpose |
|---|---|---|
| `prj.conf` | yes | Zephyr/NCS subsystem config (modem, sockets, crypto, logging, PM) |
| `Kconfig`  | yes | Application symbols + defaults (see table below) |
| `boards/nrf9151dk_nrf9151_ns.{conf,overlay}` | yes | Board-specific Kconfig + devicetree (disables conflicting DK peripherals) |
| `local.conf` | **no** (git-ignored) | Per-deployment secrets / overrides |
| `prov.conf`  | yes | Provisioning-build overlay (AT-host bridge) - enabled via `PROV=1` |

### local.conf

`local.conf` is git-ignored; create it with at least the endpoint and APN:

```
CONFIG_APP_SERVER_HOST="tracker.example.com"
CONFIG_APP_APN="iot.example.net"
```

Any `APP_*` symbol from `Kconfig` can be overridden here (see the reference
table). `CONFIG_APP_PSK_HEX` is **legacy** (only the unused ChaCha20 transport
consumed it) and is not required.

### Flashing

```sh
west flash -d build                 # or: nrfutil device program / the nRF Connect programmer
```

(Run `west flash` inside the NCS toolchain, the same way `build.sh` invokes
`west build`.) On Apple Silicon, flashing needs a **native arm64** SEGGER
J-Link install - an Intel-only J-Link library cannot be loaded by the arm64
toolchain Python.

---

## Over-the-air updates

The build carries **MCUboot** (`sysbuild.conf`), which splits the 1 MB flash
into two 416 KB slots pinned by `pm_static.yml`. `fota.c` checks a manifest on
the telemetry host at power-on and on each engine-off telemetry wake, and
installs anything newer - provided the battery is above
`CONFIG_APP_FOTA_MIN_BATTERY_MV` (12.0 V). A swapped image that never finishes
booting is rolled back automatically.

The firmware version lives in one place, the `VERSION` file: it feeds
`<zephyr/app_version.h>`, the MCUboot image header, and the version comparison.
Bump it before publishing.

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
PROV=1 BUILD_DIR="$PWD/build_prov" ./build.sh pristine
west flash -d build_prov            # boots into "PROVISIONING MODE" (AT host on VCOM0)
```

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
./build.sh && west flash -d build
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
| `APP_DEMO_MODE` | n | Mask lat/lon in the serial log (public demos); telemetry unaffected |
| `APP_SERVER_HOST` | "" | Telemetry hostname (else `HOSTNAME` in `config.h`) |
| `APP_APN` | "" | Cellular APN (else `DEFAULT_APN`) |
| `APP_PSK_HEX` | "" | **Legacy** PSK for the old ChaCha20 transport |
| `APP_ENGINE_OFF_LOOP_INTERVAL` | 0 | Engine-off telemetry interval (0 = off) |
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
| `APP_ACC_MOVEMENT_THRESHOLD` | 150 | Movement delta threshold (milli-g) |
| `APP_MOVEMENT_CONFIRM_MS` / `_HITS` | 3000 / 2 | Movement confirmation window / samples |
| `APP_MOVEMENT_INACTIVITY_RESET` | 1800 | Inactivity reset timer (s) |
| `APP_MOVEMENT_ALARM` | n | Enable movement alarm |
| `APP_COAST_STOP_SPEED_KMH_X10` | 50 | Coast-to-stop speed threshold (km/h ×10) |
| `APP_COAST_MAX_ITERATIONS` | 60 | Coast-to-stop max iterations |
| `APP_GSM_ESCALATION_POWERCYCLE` / `_SLEEP` | 3 / 5 | Send failures before power-cycle / sleep |
| `APP_GSM_RECOVERY_SLEEP_INTERVAL` | 300 | Recovery sleep interval (s) |

---

## Logging / serial

The console + logs are on the nRF9151 DK's first VCOM at **115200 baud, 8-N-1**.
Note that macOS resets a USB-serial line to 9600 on each `open()`, so a monitor
must hold the descriptor open while running `stty … 115200` (as the project's
`monitor.sh` logger does).
