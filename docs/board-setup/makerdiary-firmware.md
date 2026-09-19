# Updating the Makerdiary firmware

The Connect Kit has a second microcontroller, an nRF52820, that runs its USB connection and acts as the debug probe. Its stock firmware leaves a high-frequency clock running after the USB cable is unplugged, which costs about 2mA continuously - more than fifty times the tracker's whole sleep budget of roughly 35.5µA. On a vehicle battery that is the difference between weeks of standby and days.

The fix is in the Makerdiary repository in two parts: [#19](https://github.com/makerdiary/nrf9151-connectkit/pull/19) puts the nRF52820 into SYSTEM OFF when USB is unplugged, and [#20](https://github.com/makerdiary/nrf9151-connectkit/pull/20) does the same on the two other power-off paths, without which the chip can still hang at about 2mA. Both are merged into `main`, but at the time of writing no Makerdiary release contains them: v2.0.0, every prebuilt image and the Connect Kits currently shipping all predate the fix. Build the firmware from source and flash it once on every Connect Kit before it goes into a vehicle. `flash.sh` only programs the nRF9151 and never touches this chip.

There is more background in the [original write-up](https://l0destar.com/blog/makerdiary-nrf52820-firmware-bug.html).

## Install nRF Connect SDK v3.4.0

The interface firmware builds with nRF Connect SDK v3.4.0, alongside the v3.3.0 the tracker firmware uses.

**With nRF Util** (macOS, or Linux on x86_64):

```sh
nrfutil sdk-manager install v3.4.0
```

**On Linux on arm64**, install it the way the [prerequisites](/board-setup/prerequisites.md#nrf-connect-sdk) install v3.3.0, with the Zephyr SDK that v3.4.0 needs, 1.0.1. It takes about another 4.5GB:

```sh
uvx --python 3.12 west init -m https://github.com/nrfconnect/sdk-nrf --mr v3.4.0 ~/ncs/v3.4.0
cd ~/ncs/v3.4.0
uvx --python 3.12 west update --narrow -o=--depth=1
uv venv --python 3.12 .venv
uv pip install --python .venv/bin/python west -r zephyr/scripts/requirements.txt -r nrf/scripts/requirements.txt -r bootloader/mcuboot/scripts/requirements.txt

cd ~
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.1/zephyr-sdk-1.0.1_linux-aarch64_minimal.tar.xz
tar xf zephyr-sdk-1.0.1_linux-aarch64_minimal.tar.xz && rm zephyr-sdk-1.0.1_linux-aarch64_minimal.tar.xz
zephyr-sdk-1.0.1/setup.sh -t arm-zephyr-eabi -c
```

## Build

```sh
# in firmware/
ifmcu/build.sh
```

The script builds Makerdiary's repository, unmodified, from the copy your first tracker build fetched into `ifmcu/.makerdiary-repo` (it clones the repository itself if that is missing), taking the board definition from the same copy. Set `NCS_VERSION` or `NCS_ROOT` if your SDK is elsewhere.

It refuses to build a clone that doesn't include #20, or that has local changes - an older `ifmcu/build.sh` patched the clone itself, and that patch made a clone from before #20 look fixed. Discard any changes, update the clone and run the script again:

```sh
git -C ifmcu/.makerdiary-repo checkout -- .
git -C ifmcu/.makerdiary-repo pull
```

The image is written to `build_ifmcu/ifmcu_firmware/zephyr/zephyr.uf2`.

## Flash

1. If the Connect Kit is on the carrier board, switch off the 12V supply: the board powers the Connect Kit through its battery connector, so plugging in USB would not restart it.
2. Unplug USB, hold the DFU/RST button on the Connect Kit, plug USB back in and then release the button. A drive called `UF2BOOT` appears. (Pressing the button while the Connect Kit is running only resets the nRF9151.)
3. Copy the image onto it.

On macOS:

```sh
# in firmware/
cp build_ifmcu/ifmcu_firmware/zephyr/zephyr.uf2 /Volumes/UF2BOOT/
```

On a Linux desktop, the drive is mounted where your desktop puts removable drives, usually `/media/$USER/UF2BOOT`:

```sh
# in firmware/
cp build_ifmcu/ifmcu_firmware/zephyr/zephyr.uf2 /media/$USER/UF2BOOT/
```

The Connect Kit resets by itself when the copy completes.

!!! tip "Working in a virtual machine"
    In bootloader mode the Connect Kit is a different USB device, a drive rather than a probe, so the virtual machine has to be given that device too.

## Check it took

1. Remove every other supply: if the Connect Kit is on the carrier board, switch off the 12V bench supply.
2. Unplug USB.

With the fixed firmware the RGB LED goes completely dark and the Connect Kit's draw falls to microamps. With the stock firmware the nRF52820 stays awake at about 2mA, sometimes with the green LED still lit.

Once the tracker firmware is running, the whole board asleep at 12V should draw tens of microamps. A board sitting a couple of milliamps above that after USB has been unplugged has not taken this update.
