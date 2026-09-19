# Onboarding devices into the server

Your server only accepts telemetry from devices it knows. Enrolling a device records its IMEI and the key its firmware was built with, and from then on the server accepts what the device sends and answers it.

The server's tools run inside its container: `sudo docker exec l0destar python tools/<tool>.py`.

## Enrol the device

On the server, give `adddevice.py` the IMEI from the console on [minimal config and initial flashing](initial-flashing.md), a name, optionally the vehicle's registration, and the key from `CONFIG_APP_PSK_HEX` in your `local.conf`:

```sh
sudo docker exec l0destar python tools/adddevice.py 350000000000000 "Car" AB12CDE --psk <64 hex characters>
```

```text
enrolled Car (350000000000000)
```

- The **name** appears in every alert (`Car: movement: ...`) and in the device list. The **registration** is optional; when set, the map page shows it instead of the name.
- The IMEI must be 14 to 16 digits, and a device cannot be enrolled twice.
- Keep the key in your password manager as well as in `local.conf`. No tool shows it again.

Nothing needs building or flashing again: the server accepts the device's next datagram. To have it send one now rather than at its next wake, restart it:

```sh
# on the build machine, in firmware/
./reset.sh
```

Reopen the console, which the reset disconnects. Once the modem has registered, the device sends, and this time the server answers. The console shows the reply, and with pin 5 off the device then goes back to sleep:

```text
<inf> main: imei=350000000000000
<inf> transport: sent 332 bytes
<inf> data: resp: 1,0,1,track=0
<inf> main: entering sleep
```

On the server, `udp.log` records the device, and the device list shows when it was last heard from:

```sh
# on the server
tail -n 3 /srv/l0destar/logs/udp.log
sudo docker exec l0destar python tools/device.py list
```

```text
2026-09-13 17:51:56 2 records from 350000000000000 (203.0.113.7)

imei             name        registration  last seen            firmware  battery  ignition
350000000000000  Car         AB12CDE       2026-09-13 17:51:56  0.4.0     12.01V   off
```

The address in `udp.log` is your mobile operator's, not the SIM's own.

[Verifying telemetry](verifying-telemetry.md) goes through the rest of the checks.

!!! note "Letting the server choose the key"
    Without `--psk`, `adddevice.py` generates a key and prints it once. That suits a device whose firmware does not have a key yet: put the printed key in `CONFIG_APP_PSK_HEX`, then build and flash again.

## What a new device starts with

| Setting | Default | Meaning |
|---|---|---|
| `int` | `0` | Engine-off timed wake interval, in seconds. `0` means no timed wakes: the device still wakes on ignition, movement, knocks and tilt. |
| `ma` | `1` | The movement alarm flag. Firmware 0.4.x reports it but does not act on it: movement, impact and tilt alerts are raised either way. |
| `track_mode` | `0` | Track mode, switched on per session from the web page. |
| `alarm` | `0` | Server alert whenever the ignition comes on. |
| `garage` | `0` | Marks a vehicle expected to be moved, downgrading urgent alerts. |
| `overnight_alarm` | `0` | Server alert when the ignition comes on overnight (23:00 to 06:00 unless changed). |

`int`, `ma` and `track_mode` travel to the device in every reply; the other three only change what the server does. To change them use `command.py` or the API - every setting and command is in [device settings and commands](../reference/device-settings.md), and what `int` costs in battery is worked out in [deployment configuration](../deployment/configuration.md). For example, to have a parked vehicle report every hour:

```sh
sudo docker exec l0destar python tools/command.py 350000000000000 int=3600
```

`command.py` uses the API token created during [server installation](../server/installation.md). A setting like `int=` waits in the queue until the device next reads a reply; server-only settings such as `alarm=1` take effect immediately.

## More than one device

Enrol each device the same way. Every device has its own IMEI, its own key and its own section in `remote.conf` with its own board and interface settings; nothing is shared between them.

- Give each a name you will recognise in an alert.
- The web interface lists them all on its device page and opens the map for whichever you choose; see [web interface and API](web-interface.md).
- API calls that only read can fall back to a default device, but calls that change anything - settings, commands, track mode - must name the device.

## Managing devices

`tools/device.py` looks after enrolled devices:

| Command | What it does |
|---|---|
| `list` | Lists every enrolled device. |
| `show <imei>` | One device's settings, update state, most recent record and queued commands. |
| `rename <imei> <name> [registration]` | Changes the name, and the registration if given. |
| `rekey <imei> [--psk <64 hex>]` | Replaces the device's key, printing the new key once. |
| `remove <imei> --yes` | Deletes the device and all of its history. |

```sh
sudo docker exec l0destar python tools/device.py list
sudo docker exec l0destar python tools/device.py show 350000000000000
sudo docker exec l0destar python tools/device.py rename 350000000000000 "Van" XY34FGH
```

!!! danger "remove deletes the history"
    `remove` deletes the device's telemetry, journeys, fault codes and queued commands along with the device. There is no undo, so back up the database first. When you retire a device, also delete its section from `remote.conf` and its `manifest-<imei>.txt` and images from the server's firmware directory.

## Changing a device's key

### On the bench

1. Generate a new key with `openssl rand -hex 32`, put it in `local.conf`, then build and flash.
2. Give the server the same key:

```sh
sudo docker exec l0destar python tools/device.py rekey 350000000000000 --psk <64 hex characters>
```

Until both have the new key, the server rejects what the device sends.

### A device in the field

Order matters here: the device can only learn about new firmware from replies it can still decrypt.

1. On your build machine, generate the new key:

```sh
openssl rand -hex 32
```

2. Put it in the device's section of `remote.conf` and publish:

```sh
# in firmware/
./push_fw.sh --device 350000000000000
```

The device still uses its old key, so it sees the update in its replies and installs it as normal.

3. When the new image starts, it uses the new key and the server rejects its records. Its update check at start-up still reaches the server over TLS, which confirms the update and sends `Car: fota: updated to <version>`. That is your signal. If it has not arrived within about 15 minutes of the download finishing, look at the server's logs instead: `decrypt failed` lines in `udp.log` from the address the device reports from mean it is already sending with the new key, and a `/fw/manifest.txt` request in `tls.log` after the download's last range means it has restarted.
4. Straight away, give the server the same key:

```sh
sudo docker exec l0destar python tools/device.py rekey 350000000000000 --psk <64 hex characters>
```

Records sent between steps 3 and 4 are lost. This is also how to retire a key you think has leaked, for example with a firmware image; see [server security](../server/security.md).
