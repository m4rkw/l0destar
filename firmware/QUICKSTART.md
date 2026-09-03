# l0destar tracker - quickstart

**Before building, installing or relying on any of this, read the [project disclaimer](../DISCLAIMER.md).**

## 1. Update the IF MCU firmware

The Makerdiary Connect Kit's nRF52820 IF MCU (USB bridge + CMSIS-DAP) draws
~2 mA after USB disconnect because the stock firmware leaves HFCLK running.
The fix enters SYSTEM OFF on cable removal, dropping quiescent draw to
~0.3 uA. On a battery-backed tracker that 2 mA is the difference between
weeks and days of standby, so this step is not optional.

The first part of the fix is upstream, merged as
<https://github.com/makerdiary/nrf9151-connectkit/pull/19>. It is not
complete on its own: #19 discovered that `sys_poweroff()` needs SEVONPEND
cleared first or the nRF52820 spins at ~2 mA instead of powering down, but
applied that only to the USB-unplug path. Two other power-off paths (the
once-a-second charger poll and the shell `shutdown` command) still call the
bare poweroff, and the charger poll can win the race after an unplug and hang
the chip in exactly the state #19 was meant to cure. The follow-up that
routes all three through one helper lives in
`ifmcu/patches/0001-ifmcu-system-off-sevonpend.patch` until it is merged
upstream; `ifmcu/build.sh` applies it automatically.

**None of this reaches your board on its own.** The IF MCU firmware is not
updated automatically, and it is not touched by `./flash.sh` (which only
programs the nRF9151). Every Connect Kit currently shipping - and every
prebuilt image in the latest Makerdiary release, v2.0.0 - was built before
#19 landed, so a new board always needs this done once, by hand.

Build the patched IF MCU firmware (requires NCS v3.4.0):

```bash
ifmcu/build.sh
```

`ifmcu/build.sh` clones `makerdiary/nrf9151-connectkit` into
`ifmcu/.makerdiary-repo` on first run, applies `ifmcu/patches/*.patch` (each
patch once; it refuses to build if a patch no longer applies), and builds.
If you already have that clone from before #19 was merged, update it first:

```bash
git -C ifmcu/.makerdiary-repo pull
```

To apply the patch to your own checkout of the Makerdiary repo instead:

```bash
git -C /path/to/nrf9151-connectkit apply /path/to/firmware/ifmcu/patches/0001-ifmcu-system-off-sevonpend.patch
```

Once the follow-up is merged upstream, delete the patch file; `build.sh`
will then build the clone unmodified.

Flash via UF2 bootloader:

1. Double-press the Connect Kit reset button - a `UF2BOOT` mass-storage
   volume appears.
2. Copy the built image:
   ```bash
   cp build_ifmcu/ifmcu_firmware/zephyr/zephyr.uf2 /Volumes/UF2BOOT/
   ```
   The board resets automatically after the copy completes.

To confirm it took: unplug USB with no other supply attached. On patched
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

Monitor serial output (the Connect Kit exposes a USB CDC-ACM console):

```bash
screen -L /dev/cu.usbmodem* 115200
```
