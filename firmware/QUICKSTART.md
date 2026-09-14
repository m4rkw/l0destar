# l0destar tracker - quickstart

**Before building, installing or relying on any of this, read the [project disclaimer](../DISCLAIMER.md).**

## 1. Update the IF MCU firmware

The Makerdiary Connect Kit's nRF52820 IF MCU (USB bridge + CMSIS-DAP) draws
~2 mA after USB disconnect because the stock firmware leaves HFCLK running.
The fix enters SYSTEM OFF on cable removal, dropping quiescent draw to
~0.3 uA. On a battery-backed tracker that 2 mA is the difference between
weeks and days of standby, so this step is not optional.

The fix is upstream in `makerdiary/nrf9151-connectkit`, in two parts:
<https://github.com/makerdiary/nrf9151-connectkit/pull/19> (SYSTEM OFF on
USB unplug) and <https://github.com/makerdiary/nrf9151-connectkit/pull/20>
(the same SEVONPEND handling on the charger-poll and shell `shutdown` paths,
without which the chip can still hang at ~2 mA after an unplug). Both are
merged, so the current `main` branch on GitHub is all you need; this repo no
longer carries any patches of its own.

**None of this reaches your board on its own.** The IF MCU firmware is not
updated automatically, and it is not touched by `./flash.sh` (which only
programs the nRF9151). Every Connect Kit currently shipping - and every
prebuilt image in the latest Makerdiary release, v2.0.0 - was built before
#19 and #20 landed, so a new board always needs this built from current
`main` and flashed once, by hand.

Build the IF MCU firmware (requires NCS v3.4.0):

```bash
ifmcu/build.sh
```

`ifmcu/build.sh` clones `makerdiary/nrf9151-connectkit` into
`ifmcu/.makerdiary-repo` on first run and builds it unmodified. It refuses
to build a checkout that predates #20. If you already have that clone from
before #20 was merged, update it first; if it also has the old l0destar
patch applied as local edits, discard those before pulling:

```bash
git -C ifmcu/.makerdiary-repo checkout -- . && git -C ifmcu/.makerdiary-repo pull
```

To use your own checkout of the Makerdiary repo instead, just make sure it
is on a `main` that includes #20 and build
`applications/ifmcu_firmware` for `nrf9151_connectkit/nrf52820`.

Flash via UF2 bootloader:

1. Hold the Connect Kit's DFU/RST button while plugging in USB (with the 12V
   supply off if it is on the carrier board) - a `UF2BOOT` mass-storage
   volume appears.
2. Copy the built image:
   ```bash
   cp build_ifmcu/ifmcu_firmware/zephyr/zephyr.uf2 /Volumes/UF2BOOT/
   ```
   The board resets automatically after the copy completes.

To confirm it took: unplug USB with no other supply attached. On current
firmware the RGB LED goes fully dark and board draw falls to ~uA; on stock
firmware the nRF52820 stays awake at ~2 mA.

## 2. Install nRF tooling

Install [nRF Util](https://www.nordicsemi.com/Products/Development-tools/nRF-Util),
then let it install the SDKs (v3.3.0 for the tracker firmware, v3.4.0 for the IF
MCU) with their toolchains:

```bash
nrfutil install sdk-manager
nrfutil sdk-manager install v3.3.0
nrfutil sdk-manager install v3.4.0
```

Nordic publishes no toolchain for arm64 Linux. There, install the SDKs with west
as the [prerequisites](../docs/board-setup/prerequisites.md) and
[Makerdiary firmware](../docs/board-setup/makerdiary-firmware.md) pages describe.

Install pyocd for SWD flashing, with [uv](https://docs.astral.sh/uv/):

```bash
uv tool install --python 3.12 pyocd
```

## 3. Configure

Create `local.conf` (gitignored) with a board selection, the server, your APN
and the device key:

```kconfig
# Board - the carrier PCB revision (Kconfig.boards lists the others)
CONFIG_APP_BOARD_L0DESTAR_V3_4=y
# The OBD interface fitted: 0 none, 1 CAN, 2 K-wire (0 is safe on any board)
CONFIG_APP_OBD_MODE=0

# Your telemetry server, as named in its certificate
CONFIG_APP_SERVER_HOST="tracker.example.com"

# Your SIM provider's APN; the default in prj.conf is sensor.net
CONFIG_APP_APN="your.apn.here"

# Device key - 32 bytes as 64 hex characters, from `openssl rand -hex 32`.
# The server needs the same key when you enrol the device. With any other key,
# including the all-zero default, the device still sends but the server cannot
# decrypt it.
CONFIG_APP_PSK_HEX="<64 hex characters>"
```

### A-GNSS provisioning (optional, one-time)

For faster first fix, onboard the device to nRF Cloud for A-GNSS:

```bash
PROV=1 BUILD_SUBDIR=build_prov ./build.sh
pyocd load -t nrf91 --no-reset build_prov/merged.hex
pyocd reset -t nrf91 -m hw -O auto_unlock=false
```

Then run `device_credentials_installer` on the console port to write the
credentials to modem NVM, and `nrf_cloud_onboard` to add the device to your
account (both in [README.md](README.md#nrf-cloud-device-provisioning)). Flash
the normal firmware afterwards with `./flash.sh` - credentials persist across
reflashes.

## 4. Build and flash

Build:

```bash
./build.sh
```

Flash over SWD (Connect Kit must be plugged in via USB):

```bash
./flash.sh
```

`flash.sh` finishes with a hardware (pin) reset rather than pyocd's default
soft reset. A soft reset leaves the nRF9151 in debug interface mode, where it
draws milliamps in sleep until a pin reset or power cycle. The reset line is
shared with the interface MCU, so USB re-enumerates and an open `screen`
session must be reopened after each flash.

Both `flash.sh` and `reset.sh` pass `-O auto_unlock=false` to the pyocd calls
that must not destroy the image on the chip. pyocd defaults that option to
true, and every connect to an nRF91 reads CTRL-AP `APPROTECTSTATUS` first; a
part reporting APPROTECT engaged gets a CTRL-AP `ERASEALL` so the debugger can
attach again. The erase takes about a second and wipes application flash *and*
UICR, which looks exactly like the board bricking itself on an innocent reset.

That matters because the pin reset returns the SoC to normal mode, where
APPROTECT is re-armed in hardware at every reset. The firmware clears it on
boot (`CONFIG_NRF_APPROTECT_USE_UICR=y`) but only while `UICR.APPROTECT` reads
Unprotected (`0x50FA50FA`), and a mass erase leaves UICR blank -- so one erase
puts the board in a state where the next reset erases it again. `flash.sh`
keeps auto-unlock on for the load step alone, where pyocd restores UICR and
reprograms the firmware straight after. Modem firmware is outside the erased
region and survives.

Reboot the board without reflashing:

```bash
./reset.sh
```

It pulses the reset line, as `flash.sh` does, so the nRF9151 leaves debug interface
mode and sleeps at its proper current. USB re-enumerates, so reopen any `screen`
session afterwards.

Monitor serial output on the first of the Connect Kit's two USB serial ports:

```bash
ls /dev/cu.usbmodem*
screen /dev/cu.usbmodemXXXX 115200
```
