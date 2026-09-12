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

1. Double-press the Connect Kit reset button - a `UF2BOOT` mass-storage
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

Install `nrfutil` and its required subcommands:

```bash
brew install --cask nordicsemiconductor/nrfutil/nrfutil   # or: pip install nrfutil
nrfutil install device
nrfutil install toolchain-manager
```

Install the NCS SDK (v3.3.0 for the tracker firmware, v3.4.0 for IF MCU):

```bash
nrfutil sdk-manager install --ncs-version v3.3.0
nrfutil sdk-manager install --ncs-version v3.4.0
```

Install pyocd for SWD flashing:

```bash
pip install pyocd
```

## 3. Configure

Create `local.conf` (gitignored) with three required settings and a board
selection:

```kconfig
# Board - pick the carrier PCB variant:
#   APP_BOARD_L0DESTAR_V2_5_CAN, APP_BOARD_L0DESTAR_V2_5_KLINE,
#   APP_BOARD_L0DESTAR_V2_5_MICRO, APP_BOARD_L0DESTAR_V2_6_CAN,
#   APP_BOARD_L0DESTAR_V2_6_KLINE, APP_BOARD_L0DESTAR_V2_6_MICRO,
#   APP_BOARD_L0DESTAR_V3_0
# See Kconfig.boards for the full list.
CONFIG_APP_BOARD_L0DESTAR_V2_6_KLINE=y

# Server hostname (DTLS endpoint - server setup documented separately)
CONFIG_APP_SERVER_HOST="tracker.example.com"

# Device PSK - 32-byte key as 64 hex characters.
# All-zeros disables sending; generate a real key per device.
CONFIG_APP_PSK_HEX="0000000000000000000000000000000000000000000000000000000000000000"
```

The APN defaults to `iot.1nce.net` (from `prj.conf`). Override it in
`local.conf` if needed:

```kconfig
CONFIG_APP_APN="your.apn.here"
```

### A-GNSS provisioning (optional, one-time)

For faster first fix, onboard the device to nRF Cloud for A-GNSS:

```bash
PROV=1 BUILD_DIR="$PWD/build_prov" ./build.sh pristine
pyocd load -t nrf91 build_prov/merged.hex
```

Then run `nrf_cloud_onboard` / `device_credentials_installer` over the AT
console to write credentials to modem NVM. Re-flash the normal firmware
afterwards - credentials persist across reflashes.

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

It uses `sysresetreq`, which resets the nRF9151 core only and leaves USB and an
open `screen` session alone. If it fails reporting APPROTECT, the board booted
locked; `./flash.sh` is the recovery path.

Monitor serial output (the Connect Kit exposes a USB CDC-ACM console):

```bash
screen -L /dev/cu.usbmodem* 115200
```
