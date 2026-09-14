# Board setup prerequisites

This section brings one board up on the bench - flashed with the tracker firmware, enrolled on your server and reporting - before it goes anywhere near a vehicle.

The shortest way to a working tracker is the first four pages: this one, [minimal config and initial flashing](/board-setup/initial-flashing.html), [onboarding the device into the server](/board-setup/server-onboarding.html) and [verifying telemetry](/board-setup/verifying-telemetry.html). The pages after them cover the web interface, over-the-air updates, the Makerdiary firmware update every Connect Kit needs before it goes into a vehicle, and the optional nRF Cloud onboarding. Once a board is reporting, [firmware build options](/reference/firmware.html) lists everything you can change.

It assumes a board that has been through [Assembly](/assembly/board-test.html) and a server installed as described in [Server installation](/server/installation.html). The interactive stage of the board test needs the software on this page, so if you have not run it yet, come back to it once this page is done.

## Hardware

- **The assembled carrier board** with the Makerdiary nRF9151 Connect Kit fitted, the u.FL leads connected, and the unpowered and first power-up checks from the [board test](/assembly/board-test.html) passed.
- **A USB-C data cable.** A charge-only cable powers the Connect Kit but never shows up as a debug probe.
- **A current-limited 12V bench supply.** The 50mA limit is only for the first power-up checks. Before running firmware, raise it to around 300mA: a modem transmitting at full power averages up to about 45mA from the 12V input, with bursts above that, and a supply that hits its current limit makes the board brown out and reset.
- **A bench lead** for the board's Micro-Fit 3.0 connector (pinout in the [hardware reference](/reference/hardware.html#vehicle-connector-pinout)): ground on pin 2, 12V on pin 4 (permanent live), and 12V through a switch on pin 5 so you can turn the ignition on and off. Leave pins 1, 3 and 6 unconnected on the bench.
- **An antenna** that meets the [antenna requirements](/reference/hardware.html#antenna). The GNSS port needs an active antenna. Put it by a window or outside: GNSS will not get a fix in the middle of a room.
- **A SIM card that supports LTE-M**, activated, and the APN its provider tells you to use. The firmware only uses LTE-M - NB-IoT is disabled in `prj.conf` - so a SIM or network without LTE-M never registers.
- **Your l0destar server**, installed and reachable from the internet on UDP 65480 and TCP 65481 ([Telemetry port](/server/telemetry-port.html)).

## Software

The build and flash scripts run on macOS and Linux.

### nRF Connect SDK

The tracker firmware builds with nRF Connect SDK v3.3.0.

**On macOS, or Linux on an x86_64 machine,** install [nRF Util](https://www.nordicsemi.com/Products/Development-tools/nRF-Util), then let it install the SDK and its toolchain:

```sh
nrfutil install sdk-manager
nrfutil sdk-manager install v3.3.0
```

It puts the SDK in `/opt/nordic/ncs/v3.3.0` on macOS and `~/ncs/v3.3.0` on Linux, which is where the build script looks for it.

**On Linux on an arm64 machine** - a Raspberry Pi, or a virtual machine on an Apple silicon Mac - Nordic publishes no toolchain for nRF Util to install, so install the SDK with west and the Zephyr SDK instead. These commands were tested on Ubuntu 26.04, and use about 5GB:

```sh
sudo apt install -y git cmake ninja-build gperf device-tree-compiler xz-utils file make gcc g++ libmagic1 wget
curl -LsSf https://astral.sh/uv/install.sh | sh
```

Open a new shell so that `uv` is on your path, then:

```sh
uvx --python 3.12 west init -m https://github.com/nrfconnect/sdk-nrf --mr v3.3.0 ~/ncs/v3.3.0
cd ~/ncs/v3.3.0
uvx --python 3.12 west update --narrow -o=--depth=1
uv venv --python 3.12 .venv
uv pip install --python .venv/bin/python west -r zephyr/scripts/requirements.txt -r nrf/scripts/requirements.txt -r bootloader/mcuboot/scripts/requirements.txt

cd ~
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.17.4/zephyr-sdk-0.17.4_linux-aarch64_minimal.tar.xz
tar xf zephyr-sdk-0.17.4_linux-aarch64_minimal.tar.xz && rm zephyr-sdk-0.17.4_linux-aarch64_minimal.tar.xz
zephyr-sdk-0.17.4/setup.sh -t arm-zephyr-eabi -c
```

The build script finds this installation by itself: it uses the `west` in `~/ncs/v3.3.0/.venv` whenever nRF Util has no toolchain for the version. `--depth=1` fetches only the SDK's current source, not its history.

### pyocd

pyocd flashes the board through the Connect Kit's debug probe. Install it with [uv](https://docs.astral.sh/uv/), which also provides the Python 3.12 it needs:

```sh
uv tool install --python 3.12 pyocd
```

### Linux: access to the Connect Kit

On Linux, only root can use the Connect Kit's debug probe and serial ports until you allow otherwise. Add a udev rule for the probe, and add yourself to the group that owns serial ports:

```sh
printf '%s\n' \
  'SUBSYSTEM=="usb", ATTR{idVendor}=="2fe3", ATTR{idProduct}=="0204", MODE="0660", GROUP="plugdev", TAG+="uaccess"' \
  'KERNEL=="hidraw*", ATTRS{idVendor}=="2fe3", ATTRS{idProduct}=="0204", MODE="0660", GROUP="plugdev", TAG+="uaccess"' \
  | sudo tee /etc/udev/rules.d/50-connectkit.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
sudo usermod -aG dialout,plugdev "$USER"
```

Log out and back in for the group change to take effect.

## Get the source

```sh
git clone --depth 1 https://github.com/m4rkw/l0destar.git
cd l0destar/firmware
```

The remaining commands in this section run in `firmware/`. There is nothing more to set up by hand: the first build fetches the Connect Kit's board definition from Makerdiary and creates your firmware signing key.

!!! warning "No spaces in the path"
    Zephyr's build system breaks on source paths containing spaces. Clone into a path without them.

## Check the computer sees the board

Connect the Connect Kit with the USB-C cable, then:

```sh
pyocd list
```

It should list the probe as `Makerdiary IFMCU CMSIS-DAP`, with the target `nrf91`. The Connect Kit also has two serial ports, the first of which is the firmware console: `/dev/cu.usbmodem*` on macOS, and on Linux `/dev/serial/by-id/usb-Makerdiary_IFMCU_CMSIS-DAP_<serial>-if00`.

!!! tip "Working in a virtual machine"
    Flashing resets the Connect Kit's USB interface, so it disconnects and reconnects. Set the virtual machine to connect the device automatically, or it can end up back on the host after the first flash.
