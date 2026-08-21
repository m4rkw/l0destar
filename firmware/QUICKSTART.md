# l0destar tracker — quickstart

**Before building, installing or relying on any of this, read the [project disclaimer](../DISCLAIMER.md).**

## 1. Patch the IF MCU power firmware

The Makerdiary Connect Kit's nRF52820 IF MCU (USB bridge + CMSIS-DAP) draws
~2 mA after USB disconnect because the stock firmware leaves HFCLK running.
The fix enters SYSTEM OFF on cable removal, dropping quiescent draw to
~0.3 µA.

Upstream PR: <https://github.com/makerdiary/nrf9151-connectkit/pull/19>

If the PR hasn't been merged yet, apply the patch from this repo:

```bash
cd ifmcu/.makerdiary-repo
git apply ../power-fix.patch
```

Build the patched IF MCU firmware (requires NCS v3.4.0):

```bash
ifmcu/build.sh
```

Flash via UF2 bootloader:

1. Double-press the Connect Kit reset button — a `UF2BOOT` mass-storage
   volume appears.
2. Copy the built image:
   ```bash
   cp build_ifmcu/ifmcu_firmware/zephyr/zephyr.uf2 /Volumes/UF2BOOT/
   ```
   The board resets automatically after the copy completes.

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
# Board — pick the carrier PCB variant:
#   APP_BOARD_L0DESTAR_V2_5_CAN, APP_BOARD_L0DESTAR_V2_5_KLINE,
#   APP_BOARD_L0DESTAR_V2_5_MICRO, APP_BOARD_L0DESTAR_V2_6_CAN,
#   APP_BOARD_L0DESTAR_V2_6_KLINE, APP_BOARD_L0DESTAR_V2_6_MICRO,
#   APP_BOARD_L0DESTAR_V3_0
# See Kconfig.boards for the full list.
CONFIG_APP_BOARD_L0DESTAR_V2_6_KLINE=y

# Server hostname (DTLS endpoint — server setup documented separately)
CONFIG_APP_SERVER_HOST="tracker.example.com"

# Device PSK — 32-byte key as 64 hex characters.
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
afterwards — credentials persist across reflashes.

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
