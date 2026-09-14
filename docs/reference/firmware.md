# Firmware build options

Everything in the tracker firmware that is fixed at build time: the configuration files and
the order they are applied in, the environment variables the build scripts read, every
application Kconfig symbol, the Zephyr and MCUboot settings worth knowing about, and the
constants compiled in from `src/config.h`.

Values describe firmware 0.4.x as of September 2026. Paths are
relative to the `firmware/` directory of your clone. Where an older document in the repository
says something different, the Kconfig files and the source are what the firmware actually does -
see [Where older documents disagree](#where-older-documents-disagree).

Settings the server sends to a running device, such as the engine-off interval and track mode,
are on [Device settings and commands](/reference/device-settings.html).

## Configuration files

| File | In git | Purpose |
|---|---|---|
| `prj.conf` | yes | Zephyr and nRF Connect SDK subsystems, plus `CONFIG_APP_APN="sensor.net"` and `CONFIG_APP_LOG_LEVEL=3` |
| `Kconfig`, `Kconfig.boards` | yes | Every `APP_*` symbol and its default; `Kconfig.boards` holds the per-board pin maps and hardware flags |
| `boards/makerdiary/nrf9151_connectkit/` | no | The Connect Kit board target, gitignored; `build.sh` copies it in from makerdiary/nrf9151-connectkit on the first build |
| `boards/nrf9151dk_nrf9151_ns.conf`, `.overlay` | yes | Applied only to nRF9151 DK builds; the overlay frees the DK's I2C2 and SPI3 pins for the application |
| `boards/nrf9151_connectkit_nrf9151_ns.conf` | yes | Applied only to Connect Kit builds: turns on the modem antenna library and sends `AT%XCOEX0`, which powers the Connect Kit's GNSS amplifier. Without it GNSS sees no satellites |
| `makerdiary.conf` | no | Optional, layered onto Connect Kit builds when it exists, for settings of your own such as a band lock |
| `local.conf` | no | Your bench build; `build.sh` stops if it is missing unless `LOCAL_CONF` is set empty |
| `local.overlay` | no | Optional devicetree overlay, applied last |
| `remote.conf` | no | The deployed fleet, one section per IMEI; `push_fw.sh` turns each section into `.remote/<imei>.conf` and builds with that instead of `local.conf` |
| `board_test.conf` | no | Written by `board_test.sh` and used instead of `local.conf` |
| `prov.conf` | yes | Layered with `PROV=1`: builds the nRF Cloud provisioning bridge |
| `lte_power_test.conf` | yes | Layered with `LTE_TEST=1`: builds the LTE transmit power test |
| `sysbuild.conf` | yes | Enables MCUboot; `build.sh` supplies the signing key |
| `mcuboot_priv.pem` | no | The image signing key, created by the first bench build when it is missing |
| `pm_static.yml` | yes | The pinned flash partition layout |
| `VERSION` | yes | `MAJOR.MINOR` only; the patch number is supplied at build time |
| `src/ca_cert.h` | no | The CA certificate written into the modem and used to verify the server during firmware downloads, generated from `certs/ca.crt` by `build.sh` |
| `certs/ca.crt` | no | Your server's CA certificate, copied from the server. `build.sh` builds `src/ca_cert.h` from it and will not build tracker firmware without it; `push_fw.sh` checks the update endpoint with it |
| `src/config.h` | yes | Compile-time constants that are not Kconfig symbols |

### Order of precedence

Kconfig values are merged in this order, a later file overriding an earlier one:

1. The defaults in `Kconfig`, `Kconfig.boards` and the SDK's own Kconfig files.
2. The board target's defconfig.
3. `prj.conf`, then the application's `boards/<board>.conf` where one exists (the DK and the Connect Kit each have one).
4. The overlays `build.sh` passes, in this order: `makerdiary.conf` (Connect Kit builds, when
   present), the local fragment (`local.conf`, or whatever `LOCAL_CONF` names), `prov.conf`
   (`PROV=1`), then `lte_power_test.conf` (`LTE_TEST=1`).

Two rules catch people out:

- Assigning a symbol that does not exist, or a value outside a symbol's range, aborts the build
  with `error: Aborting due to Kconfig warnings`.
- Assigning a symbol whose `depends on` is not met prints a warning and the value is dropped.
  `CONFIG_APP_KLINE_TELEMETRY=y`, for example, does nothing on a build without
  `APP_BOARD_HAS_KLINE`. Check the warnings when a setting seems to have no effect.

`build.sh` remembers the profile, board target and local fragment it last built with, and forces
a clean rebuild when any of them changes. Devicetree follows the same pattern: the board's own
description, then the application's `boards/<board>.overlay` (DK only), then `local.overlay`.

## Build script environment variables

### build.sh

| Variable | Default | Effect |
|---|---|---|
| `NCS_VERSION` | `v3.3.0` | nRF Connect SDK version to build with |
| `NCS_ROOT` | `/opt/nordic/ncs/$NCS_VERSION` or `~/ncs/$NCS_VERSION`, whichever exists | SDK workspace that `west` runs in |
| `PROFILE` | detected | `makerdiary` (Connect Kit) or `dk` (nRF9151 DK). Detection picks `makerdiary` when `pyocd list` shows the Makerdiary probe, and also when it does not, so a DK build needs `PROFILE=dk` |
| `BOARD` | per profile | Explicit west board target, overriding the profile's (`nrf9151_connectkit/nrf9151/ns` or `nrf9151dk/nrf9151/ns`) |
| `BUILD_DIR` | `build` | Absolute build directory |
| `BUILD_SUBDIR` | - | Build directory relative to `firmware/`, used when `BUILD_DIR` is not set (`push_fw.sh` uses it) |
| `LOCAL_CONF` | `local.conf` | Kconfig fragment layered after `prj.conf`, relative to `firmware/`; it must exist. `LOCAL_CONF=` layers none |
| `PRISTINE` | `auto` | Passed to `west build -p`; `always` forces a clean build |
| `FW_PATCH` | `0` | Patch number stamped into the version. `push_fw.sh` sets it; a bench build is `MAJOR.MINOR.0` |
| `PROV` | - | `1` layers `prov.conf` |
| `LTE_TEST` | - | `1` layers `lte_power_test.conf` |

`build.sh` runs `west` inside nRF Util's toolchain for `NCS_VERSION` when one is installed, and
otherwise uses the `west` in `$NCS_ROOT/.venv` or on `PATH`, as in the arm64 Linux installation
on [Board setup: Prerequisites](/board-setup/prerequisites.html). That logic is in `ncs_env.sh`,
which `ifmcu/build.sh` shares.

The first build fetches the Connect Kit board definition and creates `mcuboot_priv.pem`. Every
build generates `src/ca_cert.h` from `certs/ca.crt`, and stops if `certs/ca.crt` is missing,
unless it is a board test, provisioning or LTE test build: those never store a CA, and get a
header that holds none. A build with `FW_PATCH` set, as
`push_fw.sh` makes, stops rather than create a signing key, since a release has to be signed
with the key the fleet already trusts.

A first argument of `pristine` deletes `./build` before building. It deletes that directory
whatever `BUILD_DIR` says, so use `PRISTINE=always` to rebuild a different build directory from
scratch.

A build produces `build/merged.hex`, the complete image including MCUboot for flashing over SWD,
and `build/firmware/zephyr/zephyr.signed.bin`, the signed application image that is published for
over-the-air updates.

### push_fw.sh

| Variable | Default | Effect |
|---|---|---|
| `FW_SERVER` | `a` | ssh host the images and manifests are copied to. The default is the author's - set yours |
| `FW_DIR` | `/var/www/tracker/fw` | Directory on that host; it must be the server's `fw_dir` |
| `REMOTE_CONF` | `remote.conf` | The fleet description |
| `FW_HOST` | `CONFIG_APP_SERVER_HOST` from `[common]` | Hostname the published release is verified against |
| `VERIFY_PORT` | `65481` | Port of the server's firmware endpoint |
| `VERIFY_URL` | `https://$FW_HOST:$VERIFY_PORT` | Base URL for those checks |
| `CA_LOCAL` | `certs/ca.crt` | CA the checks verify the server's certificate with |

Options: `--device <imei>` publishes one device, `--no-build` publishes what is already in
`build_remote_<imei>/`, `--force` allows a version that is not newer than the one already offered,
`--list` prints the resolved configuration and the next version without publishing, and
`--patch <n>` pins the patch number instead of deriving it from the server.

In `remote.conf`, lowercase `key = value` lines are metadata - `name` (a label for the output),
`profile` (`dk` or `makerdiary`) and `board` (an explicit west target) - and `CONFIG_*` lines are
copied into that device's fragment, `[common]` first so the device's own lines win. Section names
must be 15-digit IMEIs.

Before publishing, `push_fw.sh` refuses a build whose `.config` has `CONFIG_APP_DEBUG_IGNITION`
other than `-1`, `CONFIG_APP_DEBUG_BATTERY_MV` other than `0`, or any of `CONFIG_APP_CAN_TEST`,
`CONFIG_APP_KLINE_TEST` and `CONFIG_APP_ACCEL_TEST` enabled. It checks nothing else - see
[Debug overrides and test harnesses](#debug-overrides-and-test-harnesses).

### board_test.sh

| Variable | Default | Effect |
|---|---|---|
| `SERIAL` | the first `/dev/cu.usbmodem*`, or on Linux the Connect Kit's first `/dev/serial/by-id/` port | Console port |
| `VERBOSE` | - | `1` keeps the module logs at info (`APP_LOG_LEVEL=3`); otherwise the test build uses 2 |
| `PROFILE`, `BOARD` | - | Passed through to `build.sh` |

The script does not layer `local.conf`. It writes `board_test.conf` - the board selection,
`APP_OBD_MODE`, `APP_BOARD_TEST=y`, `CONFIG_LOG_MODE_IMMEDIATE=y`, `CONFIG_APP_NETWORK_REGISTRATION_TIMEOUT=180`,
the APN and the impact threshold - and builds with that. From `local.conf` it reads only
`CONFIG_APP_APN` (falling back to `makerdiary.conf`, then `prj.conf`, then asking),
`CONFIG_APP_DEMO_MODE`, `CONFIG_APP_BOARD_TEST_HIDE_COORDS` and `CONFIG_APP_CRASH_THRESHOLD_MG`
(1200 in the test build when unset).

### ifmcu/build.sh

| Variable | Default | Effect |
|---|---|---|
| `NCS_VERSION` | `v3.4.0` | SDK version for the Connect Kit's interface MCU firmware |
| `NCS_ROOT` | `/opt/nordic/ncs/$NCS_VERSION` or `~/ncs/$NCS_VERSION`, whichever exists | SDK workspace |

It clones makerdiary/nrf9151-connectkit into `ifmcu/.makerdiary-repo` when that is missing, refuses
a checkout that predates the upstream power fix (PR #20), builds with that checkout as its board
root and writes `build_ifmcu/ifmcu_firmware/zephyr/zephyr.uf2`.

`flash.sh` and `reset.sh` read no variables. `flash.sh` always programs `build/merged.hex`. On a
locked chip the load can stop with a memory transfer fault just after the erase that unlocks it,
so when pyocd has reported unlocking the chip, `flash.sh` loads it a second time; any other failed
load stops the script.

## Kconfig symbols

Set any symbol with a prompt in `local.conf`, or in a device's section of `remote.conf`, as
`CONFIG_<symbol>=<value>`: `y` or `n` for booleans, strings in double quotes. Symbols marked
hidden have no prompt; they follow the board selection and cannot be set.

### Board selection

`APP_BOARD` is a choice: select exactly one board. The selection sets the pin map, the
fitted-hardware flags and the power-domain wiring described below. For the hardware side of the
same decision see [Hardware](/reference/hardware.html#interface-selection-pads).

| Symbol | Board | `APP_BOARD_ID` |
|---|---|---|
| `APP_BOARD_BENCH` (default) | nRF9151 DK or Connect Kit with the sensors on a breadboard | `bench` |
| `APP_BOARD_L0DESTAR_V2_1` | v2.1 (CAN, K-wire and AIO inputs) | `v2.1` |
| `APP_BOARD_L0DESTAR_V2_1_MINI` | v2.1 mini (no OBD, 5 LEDs) | `v2.1m` |
| `APP_BOARD_L0DESTAR_V2_5_CAN` | v2.5C | `v2.5c` |
| `APP_BOARD_L0DESTAR_V2_5_KLINE` | v2.5K | `v2.5k` |
| `APP_BOARD_L0DESTAR_V2_5_MICRO` | v2.5M (no OBD) | `v2.5m` |
| `APP_BOARD_L0DESTAR_V2_6_CAN` | v2.6C | `v2.6c` |
| `APP_BOARD_L0DESTAR_V2_6_KLINE` | v2.6K | `v2.6k` |
| `APP_BOARD_L0DESTAR_V2_6_MICRO` | v2.6M (no OBD) | `v2.6m` |
| `APP_BOARD_L0DESTAR_V3_0` | v3.0 (CAN and K-wire, jumper-selected) | `v3.0` |
| `APP_BOARD_L0DESTAR_V3_1` | v3.1 (adds rail sensing) | `v3.1` |
| `APP_BOARD_L0DESTAR_V3_2` | v3.2 (adds MCU over-voltage protection) | `v3.2` |
| `APP_BOARD_L0DESTAR_V3_3` | v3.3 (fixes the L-line pull-down and adds L_SENSE) | `v3.3` |
| `APP_BOARD_L0DESTAR_V3_4` | v3.4 (same pin map as v3.3) | `v3.4` |

| Symbol | Type | Default | Meaning |
|---|---|---|---|
| `APP_OBD_MODE` | int | `0` | The OBD interface populated on a v3.1, v3.2, v3.3 or v3.4 board: `0` none, `1` CAN, `2` K-wire. It sets the `APP_BOARD_HAS_CAN` and `APP_BOARD_HAS_KLINE` defaults, and which rails the boot self-test and the board test exercise. Ignored on other boards |
| `APP_BOARD_ID` | string, hidden | per board, as above | Board identity for update manifests. The full identity appends the fitted interfaces - `v3.4+kline`, `v3.4+can` or plain `v3.4` - and a device refuses an image built for a different one |
| `APP_BOARD_IS_L0DESTAR` | bool, hidden | `y` for every board but the bench | Switches the pin and flag defaults from bench values to PCB values |

### Fitted hardware and power domains

| Symbol | Type | Default | Meaning |
|---|---|---|---|
| `APP_BOARD_HAS_CAN` | bool | `y` on v2.1, v2.5C, v2.6C and v3.0; on v3.1-v3.3 when `APP_OBD_MODE=1`; otherwise `n` | MCP2518FD CAN controller fitted. On v3.0, which lays out both interfaces, set the one you did not populate to `n` |
| `APP_BOARD_HAS_KLINE` | bool | `y` on the bench, v2.1, v2.5K, v2.6K and v3.0; on v3.1-v3.3 when `APP_OBD_MODE=2`; otherwise `n` | K-wire transceiver fitted |
| `APP_L_SEND_ENABLED` | bool | `y` on v3.3 and the bench, `n` on every other board | Allows the firmware to drive the L line. Boards before v3.3 switch the L pull-down straight onto the wire, and an L wire shorted to battery destroys the transistor and can take the nRF9151 with it, so leave it off on those unless you know the wire is safe |
| `APP_BOARD_HAS_AIO` | bool | `y` on v2.1 | 0-30V AIO inputs fitted; adds `+aio` to the board identity |
| `APP_BOARD_HAS_L_SENSE` | bool | `y` on v3.3 | L-line sense input fitted, read through the ADC to detect an L wire shorted to battery |
| `APP_L_SENSE_LOW_MV` | int | `2800` | Depends on `APP_BOARD_HAS_L_SENSE`. Below this the L line counts as pulled low; a line that is high, open or shorted to battery reads at the 3.6V full scale |
| `APP_BOARD_HAS_RAIL_SENSE` | bool | `y` on v3.1-v3.3 | Switched-rail status inputs fitted, so the boot self-test and the `RAIL:` alerts can tell a rail that did not come up |
| `APP_BOARD_CAN_ON_AUX` | bool, hidden | `y` on v2.1, v2.5C and v2.6C | The CAN circuit is powered from the AUX domain |
| `APP_BOARD_KLINE_ON_AUX` | bool, hidden | `y` on v2.1 | The whole K-line circuit is powered from the AUX domain |
| `APP_BOARD_KLINE_SHIFT_ON_AUX` | bool, hidden | `y` on v2.5K and v2.6K | The level shifter is on AUX and the transceiver rails on `K_EN`; the K-line pins are released only while both are up |
| `APP_BOARD_OBD_DOMAIN` | bool, hidden | `y` on v3.0 | CAN and K-line share one switched OBD domain |
| `APP_BOARD_SPLIT_OBD_DOMAIN` | bool, hidden | `y` on v3.1-v3.3 | CAN and K-line rails are switched independently |
| `APP_BOARD_CAN_XSTBY` | bool, hidden | `y` on v2.5C, v2.6C and v3.0-v3.3 | The CAN transceiver's standby pin is driven by the MCP2518FD, so putting the controller to sleep puts the transceiver in standby |
| `APP_BOARD_IGN_EXT_PULLUP` | bool, hidden | `y` on every l0destar board | The ignition sense line has an external pull-up, so the nRF9151's internal one stays off |
| `APP_BOARD_RAIL_ST_12V_ACTIVE_LOW` | bool, hidden | `y` on v3.1-v3.3 | The 12V K rail sense reads low while the rail is up |
| `APP_LED_ACTIVE_LOW` | bool | `y` for a bench build on the DK, otherwise `n` | Status LED polarity |

The domain flags tell the firmware which pins end inside a switched rail. Those pins are parked
as inputs with pull-downs whenever their rail is off, so a powered nRF9151 never back-feeds a
dead rail through a chip's protection diodes.

### Pin assignments

The `APP_PIN_*` symbols are nRF9151 `P0.x` GPIO numbers, with `-1` meaning not fitted. Their
defaults follow the board selection; override one only for a reworked board. The columns cover
the current boards and the bench; the v2.x maps are in `Kconfig.boards`. No l0destar board uses
P0.11 or P0.12, which carry the Connect Kit's console.

| Symbol | Signal | v3.3, v3.4 | v3.1, v3.2 | v3.0 | Bench, Connect Kit | Bench, DK |
|---|---|---|---|---|---|---|
| `APP_PIN_AUX_SW` | GPS rail enable | 13 | 13 | 26 | 13 | 10 |
| `APP_PIN_CAN_EN` | CAN rail enable | 23 | 23 | -1 | -1 | -1 |
| `APP_PIN_K_EN` | K-line rails enable | 10 | 10 | -1 | -1 | -1 |
| `APP_PIN_OBD_EN` | Shared OBD domain enable | -1 | -1 | 24 | -1 | -1 |
| `APP_PIN_TWI_SDA` | I2C data (INA228 and IMU) | 20 | 20 | 19 | 16 | 5 |
| `APP_PIN_TWI_SCL` | I2C clock | 21 | 21 | 18 | 17 | 4 |
| `APP_PIN_INA_ALRT` | INA228 alert | -1 | -1 | 20 | 21 | 11 |
| `APP_PIN_ACC_INT1` | IMU wake and impact interrupt | 30 | 30 | 29 | 19 | 2 |
| `APP_PIN_ACC_INT2` | IMU second interrupt, not used by the firmware | 29 | 29 | 27 | 18 | 3 |
| `APP_PIN_IGN_SENSE` | Ignition sense, low when on | 24 | 24 | 10 | 10 | 31 |
| `APP_PIN_K1_TX` | K-line transmit | 1 | 1 | 2 | 22 | 22 |
| `APP_PIN_K1_RX` | K-line receive | 3 | 3 | 4 | 25 | 21 |
| `APP_PIN_K_SLEEP` | TJA1027T sleep control | 2 | 2 | 3 | -1 | -1 |
| `APP_PIN_L_SEND` | L-line pull-down | 4 | 4 | 5 | -1 | -1 |
| `APP_PIN_L_SENSE` | L-line sense, must be P0.13-P0.20 | 14 | -1 | -1 | -1 | -1 |
| `APP_PIN_CAN_SCK` | MCP2518FD SPI clock | 17 | 17 | 13 | -1 | -1 |
| `APP_PIN_CAN_SDI` | MCP2518FD SPI data in | 16 | 16 | 14 | -1 | -1 |
| `APP_PIN_CAN_SDO` | MCP2518FD SPI data out | 18 | 18 | 15 | -1 | -1 |
| `APP_PIN_CAN_CS` | MCP2518FD chip select | 15 | 15 | 16 | -1 | -1 |
| `APP_PIN_CAN_INT` | MCP2518FD interrupt | 19 | 19 | 17 | -1 | -1 |
| `APP_PIN_GPS_RAIL_ST` | GPS rail sense | 5 | 14 | -1 | -1 | -1 |
| `APP_PIN_CAN_RAIL_ST` | CAN rail sense | 22 | 22 | -1 | -1 | -1 |
| `APP_PIN_K3V3_RAIL_ST` | K 3.3V rail sense | 0 | 0 | -1 | -1 | -1 |
| `APP_PIN_K12V_RAIL_ST` | K 12V rail sense | 31 | 31 | -1 | -1 | -1 |
| `APP_PIN_LED1` | Status LED 1 | 28 | 26 | 28 | 9 | -1, the devicetree `led0` |
| `APP_PIN_LED2` | Status LED 2 | 27 | 27 | 30 | 8 | -1 |
| `APP_PIN_LED3` | Status LED 3 | 26 | 28 | 31 | 7 | -1 |

The remaining pin symbols only apply to older or bench hardware: `APP_PIN_K2_TX` and
`APP_PIN_K2_RX`, a second K-line transceiver on the bench (24 and 23, `-1` on every PCB);
`APP_PIN_L_RECV`, the L-line receive pin on v2.1 (6) and v2.5K/v2.6K (31); `APP_PIN_LED4` and
`APP_PIN_LED5` on the v2.1 mini (6 and 5); and the AIO inputs on v2.1, `APP_PIN_AIO1` (10),
`APP_PIN_AIO2` (24), `APP_PIN_AIO3` (23), `APP_PIN_AIO4` (22), `APP_PIN_AIO5` (21) and
`APP_PIN_AIO6` (20).

### Debug overrides and test harnesses

Every symbol in this table either fakes an input or replaces the tracker, and none belongs in an
image you publish. All but the first two take over the firmware before the modem is started: a
unit running one never reports, never checks for updates, and has to be reflashed over USB.

| Symbol | Default | What it does | Refused by `push_fw.sh` |
|---|---|---|---|
| `APP_DEBUG_IGNITION` | `-1` | Forces the ignition state: `0` on, `1` off, `-1` reads the input | yes, unless `-1` |
| `APP_DEBUG_BATTERY_MV` | `0` | Reports a fixed battery voltage in millivolts; `0` reads the INA228 | yes, unless `0` |
| `APP_ACCEL_TEST` | `n` | Streams accelerometer, gyro and temperature readings at 10Hz forever; carries on to the tracker if no IMU is found | yes |
| `APP_CAN_TEST` | `n` | Sends one CAN frame, waits for the reply from a host adapter (`CANBUS.md`), then halts | yes |
| `APP_KLINE_TEST` | `n` | Runs the K-line transceiver loopback, then halts | yes |
| `APP_VOLTAGE_TEST` | `n` | Streams the INA228 bus voltage at 2Hz forever | no |
| `APP_L_SENSE_TEST` | `n` | Depends on `APP_BOARD_HAS_L_SENSE`. Streams the L-line sense reading forever, after pulling the line low for 2 seconds where `APP_L_SEND_ENABLED` allows it | no |
| `APP_CAN_BENCH` | `n` | Runs the host-driven CAN bench agent (`can_bench/`) forever | no |
| `APP_KLINE_DISCOVER` | `n` | Runs vehicle discovery, then parks (see [K-wire](#k-wire)) | no |
| `APP_BOARD_TEST` | `n` | Runs the interactive board test (`board_test.sh`), then parks | no |
| `APP_LTE_POWER_TEST` | `n` | Runs the LTE transmit power and brown-out rig (`LTE_TEST=1`) | no |
| `APP_PROVISION_MODE` | `n` | Idles as the nRF Cloud AT bridge (`PROV=1`) | no |

`APP_FOTA_INHIBIT` is not a harness, but a published image with it set would never take another
update, and `push_fw.sh` does not check it either.

### Server, network and identity

| Symbol | Type | Default | Meaning |
|---|---|---|---|
| `APP_SERVER_HOST` | string | `""` | Hostname telemetry is sent to (looked up as an IPv4 address) and, unless `APP_FOTA_HOST` is set, where updates come from. Empty falls back to `HOSTNAME` in `src/config.h`, which is also empty, so set it |
| `APP_APN` | string | `""`, set to `"sensor.net"` by `prj.conf` | Cellular APN. Set your SIM provider's |
| `APP_SERVER_PORT` | int, 1-65535 | `65480` | UDP port telemetry is sent to. Change it only when the server publishes its UDP listener under another port number |
| `APP_PSK_HEX` | string | `""` | The device's 32-byte ChaCha20-Poly1305 key as 64 hex characters: generate one with `openssl rand -hex 32` and enrol the device with `tools/adddevice.py --psk`, or use the key `adddevice.py` prints. Empty means an all-zero key: the firmware still sends, and the server rejects everything. The key is stored in the image in plain text |

### Over-the-air updates

| Symbol | Type | Default | Meaning |
|---|---|---|---|
| `APP_FOTA` | bool | `y` | The update subsystem. Leave it on: turning it off also removes the call that confirms an image installed over the air, so MCUboot would revert such an image at the next boot |
| `APP_FOTA_INHIBIT` | bool | `n` | Keeps the subsystem but never checks, downloads or acts on update adverts or the `fota` command, while still confirming the running image. For bench builds, whose version is `MAJOR.MINOR.0` and would be replaced within seconds of booting by any build published for the unit's IMEI. Never in a published image |
| `APP_FOTA_HOST` | string | `""` | Update host; empty reuses `APP_SERVER_HOST` |
| `APP_FOTA_PORT` | int | `65481` | Update port: the server's TLS listener, which serves firmware downloads |
| `APP_FOTA_SEC_TAG` | int | `42` | Modem security tag holding the CA the update server is verified against. `-1` fetches over plain HTTP; the image is still signature-checked, but the manifest is not authenticated |
| `APP_FOTA_MANIFEST_PATH` | string | `"/fw/manifest.txt"` | Requested with `?imei=<imei>&v=<running version>` appended |
| `APP_FOTA_MIN_BATTERY_MV` | int | `12000` | Below this battery voltage a download is deferred to the next check |
| `APP_FOTA_REQUIRE_BATTERY_READING` | bool | `n` | Also defer when there is no plausible reading; below 5V means no INA228, as on a bench |
| `APP_FOTA_DOWNLOAD_ATTEMPTS` | int, 1-10 | `3` | Attempts per check. The downloader cannot resume, so one stall costs the whole transfer |
| `APP_FOTA_RETRY_DELAY_S` | int, 0-300 | `10` | Pause between attempts |
| `APP_FOTA_RESCAN_ON_RETRY` | bool | `y` | Force a network re-scan before the last attempt, so a unit camped on a marginal cell reselects |
| `APP_FOTA_NBIOT_DEFERRALS` | int, 0-20 | `3` | Checks to skip while registered on NB-IoT before downloading anyway; `0` never skips. NB-IoT is disabled in `prj.conf`, so this only matters if you enable it |
| `APP_FOTA_RESCAN_TIMEOUT_S` | int, 10-600 | `90` | Registration wait after a re-scan |
| `APP_FOTA_RETRY_HOLDOFF_S` | int | `600` | Wait after a failed attempt before trying again, doubling with each consecutive failure up to eight times this. The bare `fota` command overrides it |
| `APP_FOTA_MANIFEST_TIMEOUT_S` | int | `30` | Manifest request timeout |
| `APP_FOTA_DOWNLOAD_TIMEOUT_S` | int | `1200` | Limit on one update check's download, every attempt included |
| `APP_FOTA_FRAGMENT_SIZE` | int | `0` | Range request size handed to the download library; `0` asks for one continuous response. `FOTA.md` notes that over the modem's TLS the image arrives in 2KB ranges regardless |

### Intervals and timeouts

| Symbol | Type | Default | Meaning |
|---|---|---|---|
| `APP_ENGINE_OFF_LOOP_INTERVAL` | int | `0` | Engine-off timed wake interval, in seconds, that the device starts with. At `0` it wakes every 900 seconds (`ENGINE_OFF_BOOT_INTERVAL`) until the first server reply, which then sets the interval: in practice the server's `int` setting is what applies |
| `APP_IGNITION_ON_SLEEP_INTERVAL` | int | `30` | Send interval, in seconds, with the ignition on and the engine off |
| `APP_VOLTAGE_POLL_INTERVAL` | int | `5` | How often, in seconds, the battery is sampled with the ignition on and the engine off, to notice the engine starting |
| `APP_BATTERY_CHECK_INTERVAL` | int | `86400` | While asleep with the battery below `APP_BATTERY_POWEROFF_MV`, seconds until the next check |
| `APP_NETWORK_REGISTRATION_TIMEOUT` | int | `60` | Seconds a connect waits for registration before leaving the radio searching |
| `APP_NETWORK_RETRY_INTERVAL` | int | `300` | While awake and unregistered, how often the link settings are reapplied and the radio brought up again |
| `APP_GPS_FIX_TIMEOUT_MS` | int | `60000` | Longest wait for a fix when building a record |
| `APP_GPS_COLD_FIX_TIMEOUT_MS` | int | `300000` | Longest wait for the first fix after a cold boot, when the receiver has no almanac or ephemeris |
| `APP_RESP_POLL_S` | int, 0-600 | `30` | While driving, read a server reply at least this often, so changed settings reach a moving vehicle. `0` reads replies only when stopped or at an ignition change |

### Voltage thresholds

| Symbol | Type | Default | Meaning |
|---|---|---|---|
| `APP_BATTERY_WARNING_MV` | int | `11900` | A `low battery` alert once the ignition has been off for at least 60 seconds with the battery below this |
| `APP_BATTERY_POWEROFF_MV` | int | `11800` | While asleep, a timed wake below this sends nothing and the next check is `APP_BATTERY_CHECK_INTERVAL` later |
| `APP_SLEEP_SAFETY_MV` | int | `12000` | While asleep, a timed wake below this skips its send |
| `APP_ENGINE_RUNNING_MV` | int | `13000` | Without an engine speed from the ECU, a battery voltage above this means the engine is running. The reverse needs the voltage low and the vehicle stationary for 300 seconds (`ENGINE_STOPPED_HOLD_S`), because cars with charging control let the rail fall below 13V while driving |

### Movement, impact and tow detection

| Symbol | Type | Default | Meaning |
|---|---|---|---|
| `APP_ACC_MOVEMENT_THRESHOLD` | int | `150` | Change in acceleration, in milli-g, that a 100ms poll must exceed to count towards confirming movement after a wake |
| `APP_MOVEMENT_CONFIRM_MS` | int | `10000` | Length of that confirmation window |
| `APP_MOVEMENT_CONFIRM_HITS` | int | `6` | Polls above the threshold needed within the window, not necessarily consecutive. It must stay below `APP_MOVEMENT_CONFIRM_MS / 100` or the build fails |
| `APP_MOVEMENT_INACTIVITY_RESET` | int | `1800` | Seconds without movement after which the movement alert cooldown, which grows from 5 to 15, 30 and 60 minutes, starts again at 5 |
| `APP_MOVEMENT_ALARM` | bool | `n` | Starting value of the movement alarm setting (`ma`) until the first server reply replaces it. Firmware 0.4.x reports this setting but does not act on it: movement, impact and tilt alerts are raised either way |
| `APP_CRASH_THRESHOLD_MG` | int | `4000` | Impact alert threshold while awake, in milli-g, rounded down to 125mg steps. Around 1200 is useful on the bench, where a firm rap on the desk reaches 1.5-3g |
| `APP_IMPACT_IMMEDIATE_MG` | int | `700` | While parked, a hit peaking at least this far from 1g is reported straight away instead of after the movement confirmation; `0` always waits. The IMU runs at ±2g while asleep, which limits how high this can usefully go |
| `APP_IMPACT_IMMEDIATE_MAX_MS` | int | `300` | Longest disturbance still treated as a hit for the immediate report; longer events are left to the movement confirmation |
| `APP_PARKED_IMPACT_MG` | int | `800` | A wake that does not confirm as movement but peaked at least this high raises `parked impact` instead of being ignored |
| `APP_TOW_TILT_DEG` | int | `6` | Sustained tilt from the attitude at sleep entry that raises `possible tow/jack`; `0` disables the check. Keep it above about 4 - suspension settling alone is 1-2 degrees |
| `APP_TOW_POLL_S` | int | `30` | How often, in seconds, the tilt is checked while asleep |
| `APP_TOW_REARM_S` | int | `900` | After a tilt alert, how long a new attitude must hold still before it becomes the reference and a further tilt can alert again; `0` never re-arms |

### Alert priorities and LEDs

The alerts these settings shape are listed in the
[alert catalogue](/deployment/alerts.html#alert-catalogue).

| Symbol | Type | Default | Meaning |
|---|---|---|---|
| `APP_ACCEL_ALERT_PRIORITY` | int, -2 to 2 | `2` | Priority of the movement, tilt and impact alerts, on Pushover's scale: -2 log only, -1 quiet, 0 normal, 1 high, 2 emergency (repeated until acknowledged). Lower it on a bench unit or a vehicle that is bumped all day; the server already demotes 2 to 0 for a device marked as garaged |
| `APP_ACCEL_ALERT_BACKOFF_S` | int, 0-86400 | `300` | After one of those alerts, send any others within this many seconds at `APP_ACCEL_ALERT_BACKOFF_PRIORITY`; `0` disables. The window is fixed, not sliding |
| `APP_ACCEL_ALERT_BACKOFF_PRIORITY` | int, -2 to 2 | `0` | Priority inside that window |
| `APP_LED_ACCEL_WAKE` | bool | `n` | Blink the status LEDs when a parked wake qualifies as movement, tilt or an impact. Handy while installing; off by default because it costs battery and shows where the unit is |

### K-wire

Runtime settings:

| Symbol | Type | Default | Meaning |
|---|---|---|---|
| `APP_KLINE_TELEMETRY` | bool | `n` | Depends on `APP_BOARD_HAS_KLINE`. Add the OBD-II values the ECU supports to each record: engine speed, road speed, coolant and intake temperature, load, throttle, mass air flow, timing, fuel trims, fuel system status, lamp and stored code count. The diagnostic session is opened once and held for the drive |
| `APP_KLINE_DTC_REPORT` | bool | `n` | Depends on `APP_BOARD_HAS_KLINE`. Read the stored fault codes shortly after ignition on and again whenever the stored-code count changes during a drive, and send the complete set to the server; nothing is read at ignition off, when the ECU is unpowered. Read-only: clearing codes is not implemented |
| `APP_KLINE_DTC_ON_DELAY_MS` | int, 0-30000 | `5000` | Depends on `APP_KLINE_DTC_REPORT`. Wait after ignition on before reading codes, while the ECU boots |
| `APP_KLINE_ECU_ADDR` | hex, 0x01-0xfe | `0x33` | Address the session opens with the 5-baud init. `0x33` is the OBD functional address; some vehicles, including the author's Toyota, answer only on a physical address, so use what discovery reports |
| `APP_KLINE_BAUD` | int | `10400` | Data rate for the handshake and session. Some ECUs answer at 9600; the init retries at the other rate when the sync byte does not decode |
| `APP_KLINE_OBD` | bool, hidden | `y` when either of the first two is set | Builds the OBD-II code |

Discovery, run once per vehicle with the ignition on (see
[Deployment: Configuration](/deployment/configuration.html)):

| Symbol | Type | Default | Meaning |
|---|---|---|---|
| `APP_KLINE_DISCOVER` | bool | `n` | Depends on `APP_BOARD_HAS_KLINE`. At boot, try the 5-baud and fast inits on the functional address, then sweep the physical addresses, print a summary ending in the settings to use, and park. Takes up to 15 minutes and never starts the tracker, so it is a test harness |
| `APP_KLINE_INIT_FAST` | bool | `y` | Depends on `APP_KLINE_DISCOVER`. Also try the ISO 14230-4 fast init; turning it off for an ECU known to answer the 5-baud init saves about 2.5 minutes |
| `APP_KLINE_INIT_SWEEP` | bool | `y` | Depends on `APP_KLINE_DISCOVER`. Sweep every address from 0x01 to 0xFE when 0x33 does not answer. Never on a moving vehicle |
| `APP_KLINE_INIT_ADDRS` | string | `""` | Addresses already known to answer the 5-baud init, as comma-separated hex such as `"13,29,58,B4"`; each is tried in turn and everything it sends is captured |
| `APP_KLINE_INIT_ACK` | bool | `y` | Depends on `APP_KLINE_DISCOVER`. During a capture, reply with the inverted second byte as a tester would |
| `APP_KLINE_INIT_DIAG` | bool | `n` | Depends on `APP_KLINE_DISCOVER`. Before the init, hold the L pull-down on for 5 seconds and listen on K for 20 seconds, so the wiring can be checked with a meter |
| `APP_KLINE_IDENT` | bool | `n` | Depends on `APP_KLINE_DISCOVER`. Ask each address that completes the handshake to identify itself: ECU identification, supported PIDs, engine speed, coolant temperature and VIN |
| `APP_KLINE_DTC` | bool | `n` | Depends on `APP_KLINE_IDENT`. Also read stored, pending and permanent fault codes |

`APP_L_SEND_ENABLED` is under [Fitted hardware and power domains](#fitted-hardware-and-power-domains).

### Track mode

| Symbol | Type | Default | Meaning |
|---|---|---|---|
| `APP_TRACK_MODE` | bool | `y` | Build track mode: while the server has it switched on and the ignition is on, stop GNSS and send the ECU's fast-moving values with a burst of IMU samples on a fixed period, keeping the radio connected |
| `APP_TRACK_PERIOD_MS` | int, 200-5000 | `1000` | Shortest time between records. The K-wire poll itself takes 400-500ms, so 500 gives an uneven two records a second |
| `APP_TRACK_IMU_SAMPLES` | int, 0-32 | `24` | IMU samples carried per record, from the 26Hz FIFO; `0` sends only the instantaneous reading |
| `APP_TRACK_RESP_INTERVAL_S` | int, 1-120 | `10` | How often a reply is read in track mode, which is how the device learns the mode has been switched off |

### Telemetry batching and backlog

| Symbol | Type | Default | Meaning |
|---|---|---|---|
| `APP_BATCH_SIZE` | int, 1-16 | `3` | Records per datagram while driving. Each send costs a radio connection and a GNSS re-acquisition, so batching raises the record rate at the cost of the map updating every few records. A byte limit flushes a batch before the datagram fills, so values above 3 change little. Ignition changes, setting changes and failed sends flush at once |
| `APP_DATABUF_SLOTS` | int, 0-512 | `32` | Records kept in RAM while the link is down; `0` disables the backlog. A full backlog is thinned by half rather than truncated |
| `APP_DATABUF_REC_MAX` | int, 128-1024 | `512` | Longest record the backlog holds, in bytes; a longer one is dropped with an error. Records are around 300 bytes, about 470 with every OBD-II field |
| `APP_DATABUF_FLUSH_PER_CYCLE` | int, 1-16 | `2` | Backlog datagrams sent alongside each live send, so a backlog never delays the current position |
| `APP_COAST_STOP_SPEED_KMH_X10` | int | `50` | When the ignition goes off with the vehicle still moving faster than this (km/h × 10, so 5km/h by default), keep sending until it slows below it |
| `APP_COAST_MAX_ITERATIONS` | int | `60` | Most send cycles that coasting can add |

### Captured warnings and errors

| Symbol | Type | Default | Meaning |
|---|---|---|---|
| `APP_DEBUG_LOG` | bool | `y` | Depends on `LOG`. Keep warnings and errors in RAM and send them with the next record that gets through, as `L,` lines the server writes to `device.log`, and report the reset cause (`rst=`) after each boot. Off in the provisioning build, which disables logging |
| `APP_DEBUG_LOG_HEAD` | int, 256-8192 | `1024` | Bytes kept from the start of an incident |
| `APP_DEBUG_LOG_TAIL` | int, 256-16384 | `3072` | Bytes kept from the most recent lines, as a ring in which a repeated line collapses into a count |
| `APP_DEBUG_LOG_LINE_MAX` | int, 64-255 | `160` | Longest line kept, including about 20 bytes of prefix |
| `APP_DEBUG_LOG_PER_RECORD` | int, 0-1000 | `600` | Most log bytes appended to one record; the rest waits for the next |

### Modem recovery

| Symbol | Type | Default | Meaning |
|---|---|---|---|
| `APP_MODEM_STUCK_CFUN_S` | int | `600` | Seconds registered but unable to send before the modem's functional mode is cycled. Time spent unregistered is not counted; do not shorten it to seconds, because re-attaching can take minutes |
| `APP_MODEM_STUCK_RESET_S` | int | `1800` | Seconds registered but unable to send before the modem library is restarted - the last resort, which forces a full band scan |

### Logging, demo mode and provisioning

| Symbol | Type | Default | Meaning |
|---|---|---|---|
| `APP_LOG_LEVEL` | int | `3` | Log level of the application modules: 0 off, 1 errors, 2 warnings, 3 info, 4 debug |
| `APP_DEMO_MODE` | bool | `n` | Mask latitude and longitude wherever the firmware prints them on the console, for public demonstrations. What reaches the server is unchanged |
| `APP_PROVISION_MODE` | bool | `n` | Set by `prov.conf`: bring up the modem library and idle as an AT command bridge for nRF Cloud onboarding instead of running the tracker |
| `APP_BOARD_TEST_HIDE_COORDS` | bool | `n` | Board test only: leave the fix coordinates out of its output altogether; wins over `APP_DEMO_MODE` |
| `APP_BOARD_TEST_SKIP_MODEM` | bool | `n` | Board test only: skip the modem and DNS test. `board_test.sh` sets it when no APN is configured |

## Settings in prj.conf

| Setting | Value | Why it matters |
|---|---|---|
| `CONFIG_LTE_NETWORK_MODE_LTE_M_GPS` | `y` | LTE-M plus GNSS. NB-IoT is off, so the SIM and network must support LTE-M. Do not add a `CONFIG_LTE_MODE_PREFERENCE_*` line: those depend on a dual-mode choice and are dropped with a warning |
| `CONFIG_LTE_LC_RAI_MODULE`, `CONFIG_LTE_RAI_REQ` | `y` | Release Assistance Indication lets the radio drop straight after a send, handing the antenna back to GNSS |
| `CONFIG_NRF_MODEM_LIB_ON_FAULT_RESET_MODEM` | `y` | A modem crash reinitialises the modem library within a second instead of leaving the unit awake and mute |
| `CONFIG_TASK_WDT_MIN_TIMEOUT` | `10000` | The hardware watchdog fallback is fed every 10 seconds, which is also how often the CPU wakes during an engine-off sleep; `CONFIG_TASK_WDT_HW_FALLBACK_DELAY=2000` |
| `CONFIG_LOG_BUFFER_SIZE`, `CONFIG_LOG_PROCESS_TRIGGER_THRESHOLD` | `4096`, `3` | Deferred logging with room for a whole send's log burst; `board_test.sh` switches to immediate logging |
| `CONFIG_PM_DEVICE` | `y` | Lets the console UART be suspended during sleep; left running it holds the high-frequency clock, costing 600-900µA |
| `CONFIG_PSA_WANT_ALG_CHACHA20_POLY1305` | `y` | The telemetry cipher |
| `CONFIG_MODEM_KEY_MGMT` | `y` | Writing the CA certificate into the modem |
| `CONFIG_BOOTLOADER_MCUBOOT` | `y` | The application side of MCUboot, including confirming an updated image |
| `CONFIG_DATE_TIME_NTP` | `y` | Time from NTP when the network does not supply it |
| `CONFIG_NRF_CLOUD_REST`, `CONFIG_NRF_CLOUD_AGNSS`, `CONFIG_MODEM_JWT` | `y` | A-GNSS from nRF Cloud, authenticated with a token signed by the key at `CONFIG_NRF_CLOUD_SEC_TAG` (16842753) |
| `CONFIG_APP_APN` | `"sensor.net"` | Override it with your SIM's APN |

`prov.conf` sets `CONFIG_APP_PROVISION_MODE=y`, `CONFIG_AT_HOST_LIBRARY=y`,
`CONFIG_UART_INTERRUPT_DRIVEN=y` and `CONFIG_LOG=n`, so nothing else writes to the console during
the AT exchange. `lte_power_test.conf` sets only `CONFIG_APP_LTE_POWER_TEST=y`.

## MCUboot, flash layout and version

`sysbuild.conf` enables MCUboot, and `build.sh` supplies its key:

- `SB_CONFIG_BOOTLOADER_MCUBOOT=y` builds MCUboot with the application. It swaps images using
  swap-using-move with ECDSA P-256 signatures, and the swap is revertible: an update that never
  confirms itself is rolled back at the next boot.
- `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` is set by `build.sh` to `mcuboot_priv.pem` in `firmware/`,
  which the first bench build creates. The path has to be free of spaces.

!!! warning "Back up the signing key"
    The public half of the key is built into every bootloader you flash. Lose the private half and
    no unit already in the field can be updated again without opening it up and reflashing it over
    SWD.

`pm_static.yml` pins the partition map, so every later build stays installable by the bootloader
already on a unit:

| Partition | Address | Size |
|---|---|---|
| `mcuboot` | 0x00000 | 48KB |
| `mcuboot_primary`, holding TF-M and the application | 0x10000 | 416KB |
| `mcuboot_secondary`, the update download target | 0x78000 | 416KB |
| `tfm_storage` | 0xe8000 | 80KB |

The TF-M partition is 99.4% full, about 580 bytes spare, so anything that grows the secure image
now fails the build instead of quietly moving the slots. Changing the layout means reflashing
every unit over SWD; read the header of `pm_static.yml` first.

`VERSION` holds `MAJOR.MINOR`, currently `0.4`. `build.sh` temporarily rewrites it in the form
Zephyr requires, with `PATCHLEVEL` set to `FW_PATCH`, and restores it afterwards. That one number
feeds `app_version.h`, the MCUboot image header and the update comparison. Each field is 0-255;
`push_fw.sh` derives the patch number from what the server has already published, so bump the
minor version to restart the count.

### The CA certificate

`src/ca_cert.h` is generated by `build.sh` from `certs/ca.crt`, the CA certificate your server
created on its first start (see [Server: Installation](/server/installation.html#give-the-build-machine-the-ca)). When the tracker firmware boots it writes the
certificate into modem security tag 1 and into `APP_FOTA_SEC_TAG` (42), but only into a tag that
does not already hold a CA certificate. The board test, provisioning and LTE power test builds do
not write it.

!!! warning "Changing the CA does not reach a modem that already has one"
    A unit that has booted tracker firmware carrying a different CA - from an earlier server, for
    example - keeps that one, and its update downloads then fail certificate verification. Copy
    your server's `ca.crt` before the first tracker build. To replace a certificate that is already
    stored, build and flash the provisioning image (`PROV=1`), take the modem offline from a serial
    terminal with `AT+CFUN=4`, delete the stored certificates with `AT%CMNG=3,42,0` and
    `AT%CMNG=3,1,0`, then flash normal firmware built with your `certs/ca.crt`. Leave
    tag 16842753, the nRF Cloud key, alone. This follows from the provisioning code in
    `src/modem.c` and has not been tried on an l0destar unit.

## Constants in src/config.h

These are compiled in and have no Kconfig symbol; change them in the source and rebuild.

| Constant | Value | Meaning |
|---|---|---|
| `UDP_PACKET_SIZE` | `1200` | Largest datagram the transport builds |
| `RESPONSE_TIMEOUT_MS` | `4000` | How long a send that wants a reply listens for it |
| `HOSTNAME` | `""` | Fallback for an empty `APP_SERVER_HOST` |
| `DEFAULT_APN` | `"sensor.net"` | Fallback for an empty `APP_APN` |
| `DEFAULT_USER`, `DEFAULT_PASS` | `""` | PDN username and password; stored in the settings but not applied to the modem |
| `PSK_HEX_DEFAULT` | 64 zeros | Fallback for an empty `APP_PSK_HEX` |
| `TLS_SEC_TAG` | `1` | The other modem tag the CA certificate is written to |
| `ENGINE_OFF_BOOT_INTERVAL` | `900` | Timed wake interval, in seconds, from boot until the first server reply when `APP_ENGINE_OFF_LOOP_INTERVAL` is 0 |
| `ENGINE_STOPPED_HOLD_S` | `300` | Seconds of low voltage and standstill before the voltage fallback calls the engine stopped |
| `ENGINE_MOVING_KMH` | `3.0` | GNSS speed above which the vehicle counts as moving, restarting that hold |
| `ENGINE_FIX_MAX_AGE_S` | `180` | Oldest fix still counted as evidence of movement |
| `BATTERY_WARN_SETTLE_S` | `60` | Seconds the ignition must have been off before a low reading can raise `low battery`, since cranking sags the rail |
| `IMPLAUSIBLE_VOLTAGE` | `5.0` | Readings below this mean no INA228 rather than a flat battery |
| `BATTERY_SAMPLES`, `BATTERY_SAMPLE_GAP_MS` | `8`, `3` | INA228 conversions averaged per battery reading, and the milliseconds between them |
| `BATTERY_SPREAD_WARN_V` | `0.5` | Log the range when one reading's samples spread wider than this |
| `MOVEMENT_TEMPORARY_ENGINE_OFF_INTERVAL` | `14400` | Timed wake interval applied after confirmed movement when the configured one is 0 or longer |
| `TOW_STABLE_TENTHS` | `10` | Attitude change, in tenths of a degree, still counted as holding still for `APP_TOW_REARM_S` |
| `WATCHDOG_TIMEOUT_S` | `32` | Longest an awake loop may go without feeding the watchdog |
| `WATCHDOG_SLEEP_SLICE_S` | `20` | Sleeps are waited out in slices this long so the watchdog keeps being fed |
| `DATA_LIMIT` | `2500` | Size of the record buffer, in bytes |
| `BATCH_HEADROOM` | `400` | Room kept for one more record and the log lines that ride along |
| `BATCH_FLUSH_BYTES` | `736` | `UDP_PACKET_SIZE - 64 - BATCH_HEADROOM`: a batch is sent once it reaches this size |
| `SPEED_MIN_SATS` | `4` | Satellites needed before the GNSS speed counts as evidence of movement or rest |
| `IGN_OFF_STOPPED_KMH` | `3.22` | On the ignition-off record, GNSS speeds below this (2mph) are sent as 0 |
| `GYRO_REST_KMH`, `GYRO_AUTOZERO_SAMPLES`, `GYRO_AUTOZERO_GAP_MS`, `GYRO_AUTOZERO_REJECT_LSB`, `GYRO_AUTOZERO_EMA_SHIFT` | `1.0`, `16`, `5`, `250`, `2` | Learning the gyro's temperature-dependent zero-rate offset while stationary with a good fix |

`TLS_PORT` (65481), `DTLS_PORT` (5684), `LOW_POWER_STANDBY`, `NO_MOVEMENT_GPS_SKIP` and
`ACCEL_POLL_INTERVAL` are defined but not used by firmware 0.4.x. The other macros in the file
only give Kconfig symbols shorter names, for example `CRASH_THRESHOLD_MG` for
`CONFIG_APP_CRASH_THRESHOLD_MG` and `UDP_PORT` for `CONFIG_APP_SERVER_PORT`.

## Where older documents disagree

- `firmware/README.md` describes the transport as DTLS on port 65482 and calls `APP_PSK_HEX`
  legacy. Telemetry is plain UDP with ChaCha20-Poly1305 on 65480, and the key is required.
- Its Kconfig table gives `APP_MOVEMENT_CONFIRM_MS` and `APP_MOVEMENT_CONFIRM_HITS` as 3000 and 2
  (now 10000 and 6) and lists `APP_GSM_ESCALATION_POWERCYCLE`, `APP_GSM_ESCALATION_SLEEP` and
  `APP_GSM_RECOVERY_SLEEP_INTERVAL`, which no longer exist; `APP_MODEM_STUCK_CFUN_S` and
  `APP_MODEM_STUCK_RESET_S` replaced them. Its impact section gives `APP_PARKED_IMPACT_MG` as 1.5g
  (the default is 800mg) and still names the LSM6DSO as the IMU.
- `local.conf.example` suggests 3000 and 2 for the movement confirmation.
- `QUICKSTART.md` gives the default APN as `iot.1nce.net` (`prj.conf` sets `sensor.net`) and says
  an all-zero key disables sending, which it does not. It installs the SDKs with
  `nrfutil sdk-manager install --ncs-version v3.3.0`, where current nRF Util takes the version as
  a plain argument (`nrfutil sdk-manager install v3.3.0`), and opens the console with
  `screen -L /dev/cu.usbmodem* 115200`, which matches both of the Connect Kit's serial ports: name
  the first one instead.
- The comment in `sysbuild.conf` describes two ~448KB slots; `pm_static.yml` pins 416KB.
- The `APP_KLINE_DISCOVER` help says the result is sent as an alert once the modem is up, but the
  firmware parks before starting the modem.
- The help for `APP_IMPACT_IMMEDIATE_MG` says it must sit above `APP_PARKED_IMPACT_MG`, yet the
  defaults are 700 and 800.
