# l0destar firmware

Automotive telemetry tracker firmware for the nRF9151 SiP, built on Zephyr RTOS / Nordic NCS.

## Hardware

- **MCU**: nRF9151 (LTE-M + GNSS)
- **Accelerometer**: LSM6DSO (I2C, bit-banged)
- **Voltage monitor**: INA228 (I2C, shared bus with accelerometer)
- **K-line transceivers**: 2x L9637D via bidirectional level shifter
- **Relay**: Latching (SET/RST coils with feedback pins)
- **Ignition sense**: GPIO input (active-low)

### Pin assignments

See `src/pins.h`. DK-specific peripheral conflicts (QSPI, UART1, SPI3, I2C2) are disabled in `boards/nrf9151dk_nrf9151_ns.overlay`.

## Building

Requires [nRF Connect SDK v3.3.0](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/index.html) installed at `/opt/nordic/ncs/v3.3.0` (or set `NCS_ROOT`).

```
./build.sh            # incremental build
./build.sh pristine   # clean rebuild
```

Flash with nrfjprog or the nRF Connect for Desktop programmer.

## Configuration

Build configuration is split across three layers:

| File | Committed | Purpose |
|---|---|---|
| `prj.conf` | Yes | Zephyr subsystem config (modem, networking, GPIO, crypto, logging) |
| `Kconfig` | Yes | Application symbol definitions with defaults |
| `local.conf` | No (gitignored) | Per-deployment overrides: secrets, network, preferences |

`build.sh` automatically picks up `local.conf` as a Kconfig overlay if present.

### Setting up local.conf

Copy the template and fill in your values:

```
cp local.conf.example local.conf
```

Required settings (no defaults in the committed code):

```
CONFIG_APP_PSK_HEX="<64-char hex key>"
CONFIG_APP_SERVER_HOST="<hostname>"
CONFIG_APP_SERVER_PORT=<port>
CONFIG_APP_APN="<apn>"
```

Optional overrides (defaults shown):

```
CONFIG_APP_ALWAYS_ON=n
CONFIG_APP_MOVEMENT_ALARM=n
CONFIG_APP_ENGINE_OFF_LOOP_INTERVAL=0
CONFIG_APP_IGNITION_ON_SLEEP_INTERVAL=30
CONFIG_APP_VOLTAGE_POLL_INTERVAL=5
CONFIG_APP_BATTERY_CHECK_INTERVAL=86400
CONFIG_APP_BATTERY_WARNING_MV=11900
CONFIG_APP_BATTERY_POWEROFF_MV=11800
CONFIG_APP_SLEEP_SAFETY_MV=12000
CONFIG_APP_ENGINE_RUNNING_MV=13000
CONFIG_APP_ACC_MOVEMENT_THRESHOLD=150
CONFIG_APP_MOVEMENT_CONFIRM_MS=3000
CONFIG_APP_MOVEMENT_CONFIRM_HITS=2
CONFIG_APP_COAST_STOP_SPEED_KMH_X10=50
CONFIG_APP_COAST_MAX_ITERATIONS=60
```

## State machine

```
IDLE  -->  GPS_COLLECT  -->  SEND  -->  IDLE
                                   -->  IGNITION_SLEEP (ign on, engine off)
                                   -->  SLEEP (ign off)
```

- **IDLE**: polls ignition and voltage, decides when to collect GPS
- **GPS_COLLECT**: acquires a fix, buffers a record
- **SEND**: transmits batched telemetry over encrypted UDP, handles server commands
- **IGNITION_SLEEP**: ignition on but engine not running; periodic sends, watches for engine start or ignition off
- **SLEEP**: ignition off; peripherals powered down, wakes on accelerometer interrupt (LSM6DSO hardware wake-up), ignition change, or telemetry timer

## Telemetry protocol

UDP packets are encrypted with ChaCha20-Poly1305:

```
imei_len(1) | IMEI | nonce(12) | ciphertext | tag(16)
```

IMEI is bound as AAD. The PSK is provisioned per-device via `local.conf`.

## Source layout

| File | Description |
|---|---|
| `main.c` | Entry point, state machine, sleep/wake logic |
| `modem.c` | LTE-M modem control (AT commands, registration, APN) |
| `gnss.c` | GNSS fix acquisition |
| `transport.c` | Encrypted UDP send/receive |
| `data.c` | Telemetry record formatting and batching |
| `hw_power.c` | INA228 voltage monitor, ignition read |
| `hw_accel.c` | LSM6DSO accelerometer (polling + hardware wake interrupt) |
| `hw_relay.c` | Latching relay with feedback verification |
| `hw_kline.c` | K-line bit-bang UART via L9637D |
| `hw_common.c` | GPIO init, bit-banged I2C bus |
| `crypto.c` | ChaCha20-Poly1305 envelope seal/open |
| `settings.c` | Runtime settings (in-memory, Kconfig-backed defaults) |
| `commands.c` | Server command handler |
| `alert.c` | Movement/event alert queue |
| `watchdog.c` | Task watchdog with hardware fallback |
| `pins.h` | PCB pin assignments |
| `config.h` | Compile-time defaults (mapped from Kconfig) |
