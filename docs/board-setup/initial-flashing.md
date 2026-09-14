# Minimal firmware config and initial flashing

Build the tracker firmware for your board with the smallest configuration that reports to your server, flash it over USB, and read the device's IMEI from the console. The next page enrols the device on the server with that IMEI.

Complete [Board setup prerequisites](/board-setup/prerequisites.html) first. Commands on this page run in `firmware/`.

## Copy your server's CA certificate

The tracker firmware checks your server's certificate against a CA built into it when it downloads updates. Your server created that CA on its first start. Copy the certificate from it:

```sh
# in firmware/
scp tracker.example.com:/srv/l0destar/certs/ca.crt certs/ca.crt
```

The build turns it into `src/ca_cert.h`, and refuses to build the tracker firmware without it.

!!! warning "The first boot stores the CA for good"
    The first time the tracker firmware boots, it stores its CA in the modem, at security tags 1 and 42, and it never replaces a CA that is already there. A board that has once run tracker firmware built with another CA - from an earlier server, say - keeps trusting that one through every later flash, so its updates from your server fail while its telemetry works. The console then says `TLS CA already provisioned` on the very first boot. The recovery is in [firmware build options](/reference/firmware.html#the-ca-certificate). The board test and the nRF Cloud provisioning build never store the CA.

## Write local.conf

Generate the device's key:

```sh
openssl rand -hex 32
```

Then create `firmware/local.conf`, the configuration for the board on your bench, with the key in it:

```text
# firmware/local.conf

# Carrier board
CONFIG_APP_BOARD_L0DESTAR_V3_4=y
# The OBD interface you built: 0 none, 1 CAN, 2 K-wire (0 is safe on any board)
CONFIG_APP_OBD_MODE=0

# Your server, as named in its certificate
CONFIG_APP_SERVER_HOST="tracker.example.com"
# Your SIM provider's APN
CONFIG_APP_APN="your.apn"
# The key from openssl rand -hex 32
CONFIG_APP_PSK_HEX="<64 hex characters>"

# A bench unit: never replace this build over the air
CONFIG_APP_FOTA_INHIBIT=y
```

| Setting | What it does |
|---|---|
| `CONFIG_APP_BOARD_L0DESTAR_V3_4` | Selects the carrier board: its GPIO map, the parts it has and how its switched power rails are sequenced. Other revisions have their own symbol, listed in [firmware build options](/reference/firmware.html#board-selection). |
| `CONFIG_APP_OBD_MODE` | Must match the [interface selection pads](/reference/hardware.html#interface-selection-pads) you bridged: `0` none, `1` CAN, `2` K-wire. It decides which OBD rails are powered and which driver starts. `0` powers no OBD circuitry at all, so it is safe on any board while you check the rest. |
| `CONFIG_APP_SERVER_HOST` | Where telemetry (UDP 65480) and updates (TCP 65481) go. Use the name your server's certificate was issued for. It must resolve to an IPv4 address: the firmware does not use IPv6. |
| `CONFIG_APP_APN` | The default in `prj.conf` (`sensor.net`) is almost certainly not your SIM provider's APN. With the wrong APN the modem can register on the network and still have no data connection. |
| `CONFIG_APP_PSK_HEX` | The key that encrypts everything the device sends. The server gets the same key when you enrol the device. |
| `CONFIG_APP_FOTA_INHIBIT` | A bench build is version 0.4.0, older than anything published, and the firmware checks for an update every time it boots. If the server has a build published for this IMEI - a section for it in `remote.conf` - the build you are testing is replaced within seconds; a unit with no manifest is never offered one, but keep this set so a later publish cannot catch a bench unit. An image installed over the air still confirms itself with this set. Never use it in a production build; see [OTA updates](/board-setup/ota-updates.html). |

Everything else keeps its default. [Firmware build options](/reference/firmware.html) lists every setting, including `CONFIG_APP_SERVER_PORT` and `CONFIG_APP_FOTA_PORT` for a server that publishes its ports under other numbers.

!!! tip "Recording the console"
    `CONFIG_APP_DEMO_MODE=y` replaces coordinates with a placeholder in everything printed on the console, so a live unit can be shown on screen without revealing where it is. What goes to the server is unchanged.

## Build

```sh
# in firmware/
./build.sh
```

The first build also sets up what the firmware needs from your machine:

```text
Version: 0.4.0
Board profile: makerdiary  (target: nrf9151_connectkit/nrf9151/ns)
Fetching the Connect Kit board definition from makerdiary/nrf9151-connectkit
Creating the firmware signing key: /home/you/l0destar/firmware/mcuboot_priv.pem
Back it up somewhere safe: boards flashed from now on only install updates signed with it.
src/ca_cert.h updated from certs/ca.crt
...
Built: /home/you/l0destar/firmware/build/merged.hex  (profile: makerdiary, target: nrf9151_connectkit/nrf9151/ns)
```

!!! warning "Back up mcuboot_priv.pem"
    Firmware updates are signed with this key, and the bootloader on every board you flash installs only images signed with it. Lose it and none of those boards can be updated over the air again: each would need reflashing through its USB port. Anyone who has it can build firmware your boards will install. Keep a copy somewhere offline. It is gitignored.

- `build/merged.hex` is the bootloader and firmware together, for flashing over USB. `build/firmware/zephyr/zephyr.signed.bin` is the signed image an over-the-air update uses.
- A build on the bench is always version 0.4.0. Only `push_fw.sh` gives builds a patch number.
- `./build.sh pristine` forces a clean build.

| Build error | Cause |
|---|---|
| `LOCAL_CONF=local.conf not found` | There is no `local.conf` in `firmware/`. |
| `certs/ca.crt not found` | Copy your server's CA certificate first, as above. |
| `nRF Connect SDK v3.3.0 is not in /opt/nordic/ncs or ~/ncs` | The SDK is somewhere else: set `NCS_ROOT` to its directory. |
| `Aborting due to Kconfig warnings` | A line in `local.conf` names a symbol that does not exist, or a value out of range. The warning just above names the line. |

## Flash

If the bench supply's current limit is still at the 50mA used for the first power-up checks, raise it to around 300mA now. From here on the modem registers and transmits, averaging up to about 45mA from the 12V input at full transmit power with bursts above that, and a supply that hits its current limit makes the board brown out and reset.

With the Connect Kit connected over USB - the 12V supply can stay on:

```sh
# in firmware/
./flash.sh
```

`flash.sh` programs the board with pyocd and then pulses the reset line. That reset also restarts the Connect Kit's USB interface, so the serial ports disappear and come back: reopen your console afterwards.

A board that has been running other firmware may have its debug access locked. pyocd then erases the chip to unlock it, which prints `APPROTECT enabled: will try to unlock via mass erase`, and the first attempt can stop with `Memory transfer fault` just after the erase. `flash.sh` notices, says so and loads the firmware again, which programs the board. It only tries again in that case: any other failure stops it straight away. The erase only covers the application: the modem's firmware and everything stored in the modem survive it.

This first flash also puts the MCUboot bootloader on the board, which is why it has to be done through the probe: a board can only update itself over the air once it has a bootloader.

To restart the firmware without flashing it again:

```sh
./reset.sh
```

`reset.sh` pulses the reset line, like `flash.sh`, so the serial ports disappear and come back: reopen your console afterwards.

### Why the scripts reset the way they do

- Programming normally ends with a soft reset, but that leaves the nRF9151 in debug interface mode, where it draws milliamps while asleep until the next pin reset or power cycle. `flash.sh` and `reset.sh` use a pin reset instead.
- After a pin reset the chip's access port protection (APPROTECT) is armed again. The firmware clears it on every boot, but only while the chip's UICR register allows it. pyocd's default answer to a protected chip is a mass erase, which wipes the firmware and that register - and a board in that state erases itself again on its next reset. So both scripts tell pyocd not to erase when resetting. The programming step in `flash.sh` is the one place an erase is allowed, because it rewrites the register and the firmware straight afterwards.
- If `./reset.sh` fails and pyocd mentions APPROTECT, run `./flash.sh`. That is the recovery.

## Watch the console

The console is the first of the Connect Kit's two serial ports, at 115200 baud (8N1). On macOS, list the ports and open the first:

```sh
ls /dev/cu.usbmodem*
screen /dev/cu.usbmodemXXXX 115200
```

macOS resets a serial port to 9600 baud whenever it is opened, so use a terminal like `screen` that holds the port open. Install it from Homebrew or MacPorts if `screen` is not on your path.

On Linux the console has a fixed name. Install `screen` with `sudo apt install -y screen` if you do not have it, then:

```sh
screen /dev/serial/by-id/usb-Makerdiary_IFMCU_CMSIS-DAP_*-if00 115200
```

Leave `screen` with Ctrl-A then K. `screen -L` also records everything to `screenlog.0`. `./reset.sh` restarts the Connect Kit's USB as well, so reopen the console straight after it; the first lines of the boot can be missed.

A healthy boot looks like this (abridged - timings and readings will differ):

```text
*** Booting MCUboot v2.3.0-dev-fce4dac2e629 ***
*** Booting My Application v0.4.0 ***
<inf> main: === l0destar firmware boot (v0.4.0, board v3.4+kline) ===
<inf> main: reset cause: sw
<inf> settings: apn=your.apn user=
<inf> settings: imei=(unset)
<inf> hw_selftest: === power rail self-test ===
<inf> hw_selftest: self-test: all rails OK
<inf> main: ignition=OFF battery=12.01V
<inf> modem: init ok
<inf> modem: TLS CA provisioned (sec_tag 1)
<inf> modem: FOTA CA provisioned (sec_tag 42)
<inf> modem: connecting (this can take 30s+)...
<wrn> lte_lc: Registration rejected, EMM cause: 15, Cell ID: 366868, Tracking area: 12296, LTE mode: 7
<inf> modem: nw reg status: 5
<inf> modem: connected
<inf> main: imei=350000000000000
<wrn> fota: updates inhibited (APP_FOTA_INHIBIT) — running 0.4.0
<inf> modem: PLMN: mcc=234 mnc=30
<inf> transport: sent 552 bytes
```

What to look for:

- **`board v3.4+kline`** - the board and interface you configured (`+can` for CAN, nothing after the version for no interface).
- **`self-test: all rails OK`** - the switched rails came up. `RAIL:` or `SELFTEST:` failures send you back to the [board test](/assembly/board-test.html).
- **`battery=`** close to your supply voltage, and **`ignition=`** following the switch on pin 5.
- **`connected`** followed by **`imei=`**. Write the IMEI down: you need it on the next page.
- **`sent N bytes`** - a datagram went to your server. A record needs a position, so nothing is sent until the GNSS antenna has had its first fix since the tracker started: `no fix, skipping send` means it is still waiting.
- With pin 5 off, **`entering sleep`** a few seconds later. The `registration lost (status 0)` warning after `sleep: modem power off` is the modem being switched off, not a fault.

A roaming SIM often sees a few `Registration rejected` warnings from networks it may not use before `nw reg status: 5` (registered, roaming) - that took about 50 seconds on the author's bench. The two `CA provisioned` lines appear on the tracker firmware's first boot only; later boots, and boards that already hold a CA, print `TLS CA already provisioned (sec_tag 1)`.

### No imei= line

The firmware reads the IMEI once, after the modem registers during start-up. If `connected` never appears, the IMEI stays unset for that whole boot and every send is dropped with `IMEI not set, dropping packet` - even if the modem registers later. Check that the SIM is inserted and active, the APN, that the antenna is on the LTE connector and that there is LTE-M coverage, then `./reset.sh`.

### Expected at this stage

The server does not know the device yet, so it drops what the device sends: its `udp.log` shows `decrypt failed from <address>`, and the console never prints a `resp:` line. Enrolling the device on the next page fixes that, without building or flashing again.

## How it behaves on the bench

These are worth knowing when you check telemetry later:

| Bench state | What the firmware does |
|---|---|
| Pin 5 off (ignition off) | Sends a record, then sleeps. It wakes on ignition, movement, knocks and tilt, and every 15 minutes until the server has told it how often to report. Without a fix since it started it sends nothing and sleeps, then looks for a fix again on timed wakes: after 15 and 30 minutes, 1, 2 and 4 hours, then every 4 hours. |
| Pin 5 on, supply below 13.0V | Ignition on with the engine stopped: sends a record about every 30 seconds and reads the server's reply each time. |
| Pin 5 on, supply at 13.0V or above | The engine counts as running: it tracks continuously and sends three records per datagram. |

Once the engine counts as running, the supply has to stay below 13.0V for five minutes, with GNSS showing no movement, before the firmware treats it as stopped again.

## Troubleshooting

| Symptom | Check |
|---|---|
| `pyocd list` finds no probe | The cable carries data. On Linux, the udev rule from the [prerequisites](/board-setup/prerequisites.html#linux-access-to-the-connect-kit) is in place. In a virtual machine, the Connect Kit is connected to it. |
| `flash.sh` stops with `Memory transfer fault` | Unplug USB and power the board off and on, then run `./flash.sh` again. |
| Permission denied opening the serial port | On Linux, you are in the `dialout` group and have logged in again since adding it. |
| The board keeps restarting | The bench supply's current limit: around 300mA once firmware is running, because the modem's transmit bursts can take a 50mA supply into its limit and brown the board out. Then look for a pattern in the `reset cause:` line. |
| `RAIL:... fail` or `SELFTEST:` messages | A switched rail did not come up: go back to the [board test](/assembly/board-test.html). |
| `no GPS fix` | Active antenna on the GNSS connector, with a view of the sky. An unassisted first fix can take 2 to 5 minutes. |
| `no fix, skipping send`, and no `sent` line | Nothing is sent before the first GNSS fix since the tracker started. Check the antenna as above, then switch pin 5 on: with the ignition on it keeps searching until it has a fix. |
| `battery=0.00V` or a silly reading | 12V on pin 4, and the INA228 stage of the board test. |
