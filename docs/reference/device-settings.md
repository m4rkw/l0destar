# Device settings and commands

The tracker takes its settings from the server on every reply it reads, runs the commands queued for it, and reports some of its state back. This page is the reference for that runtime interface. Build-time options are in [Firmware build options](/reference/firmware.html), and the battery cost of the engine-off interval is covered in [Deployment configuration](/deployment/configuration.html).

## How a reply reaches the device

The server answers every datagram it accepts with a single line:

```text
1,<int>,<ma>[,<commands>][,fota=<version>][,track=<0|1>]
```

| Part | Meaning |
|---|---|
| `1` | Acknowledgement |
| `<int>` | The device's engine-off interval, from the server |
| `<ma>` | The device's movement alarm flag, from the server |
| `<commands>` | Every command queued for the device, comma-separated; they leave the queue as they are sent |
| `fota=<version>` | The newest build published for this device, when there is one that has not been withheld after a failed update |
| `track=0` or `track=1` | The track mode switch, on every reply |

When the tracker reads a reply it:

1. applies `<int>` and `<ma>` - unless it has changed them itself since its last record, in which case it sends its own values (`int=<n>;ma=<n>`) with its next record and the server adopts them;
2. acts on everything after `<ma>`: the queued commands, the update advert (`fota=`) and the track mode switch (`track=`).

It reads at most 127 characters after `<ma>`, so do not queue more than a few commands at once.

Settings live in the tracker's RAM. After a restart, until the first reply arrives, it uses:

- an engine-off interval of 900 seconds, so that one lost reply cannot leave a parked unit silent;
- the movement alarm flag from `CONFIG_APP_MOVEMENT_ALARM` (off by default);
- track mode off.

## Settings

| Setting | Reply field | Server column | New device | Meaning |
|---|---|---|---|---|
| Engine-off interval | `<int>` | `device.int` | `0` | Seconds between timed reports while parked; `0` means none |
| Movement alarm | `<ma>` | `device.movement_alarm` | `1` | Stored, synced and reported, but not acted on by firmware 0.4.x |
| Track mode | `track=` | `device.track_mode` | `0` | GNSS off, engine and IMU data streamed quickly |
| Update advert | `fota=` | per-device manifest | - | See [OTA updates](/board-setup/ota-updates.html) |

- A timed report while parked sends the last known position. GNSS is only started for it when movement has been detected since the previous one. Ignition, movement, impact and tilt wake the tracker whatever the interval is.
- Movement, impact and tilt alerts are raised whatever the movement alarm flag says. Use the priority settings in [Configure alerts](/deployment/alerts.html) to quieten them.
- Track mode is switched from the web page (see [Web interface and API](/board-setup/web-interface.html)). The server clears it when the ignition goes off, or when a device is heard from after an hour of silence, and the tracker ignores `track=1` while the ignition is off.

## When the device reads a reply

A queued command waits until the tracker next reads a reply.

| Tracker state | Reads a reply | A command waits up to |
|---|---|---|
| Driving | At ignition changes, while the vehicle is reported stationary, and at least every `APP_RESP_POLL_S` (30 seconds) | about 30 seconds |
| Ignition on, engine off | Every `APP_IGNITION_ON_SLEEP_INTERVAL` (30 seconds) | about 30 seconds |
| Track mode | Every `APP_TRACK_RESP_INTERVAL_S` (10 seconds) | about 10 seconds |
| Parked, interval above 0 | On every timed report | the interval |
| Parked, interval 0 | Not until the ignition comes on, or a timed report scheduled after movement | the next drive |

After a movement alert on a unit whose interval is 0 or longer than four hours, the tracker schedules a single timed report four hours later, with GNSS; the reply to that report puts the server's interval back.

## Commands

Queue commands from the server with `tools/command.py`, or with `POST /api/1.0/command` and a bearer token (see [Web interface and API](/board-setup/web-interface.html)). Several commands in one call are joined with commas and delivered in the same reply.

```sh
# on the server
sudo docker exec l0destar python tools/command.py 350000000000000 locate
```

```sh
curl -X POST https://tracker.example.com/api/1.0/command \
  -H 'Authorization: Bearer <token>' \
  -H 'Content-Type: application/json' \
  -d '{"imei": "350000000000000", "command": "locate"}'
```

A request to queue a command must name the device, with `imei`, `device_id` or an `X-Imei` header.

### Commands the tracker runs

| Command | What the tracker does | Alert it sends back |
|---|---|---|
| `int=<seconds>` | Sets the engine-off interval. `0` stops timed reports; 1 to 9 become 10. Reported back with the next record. | `engine-off interval changed; <old> -> <new>` |
| `movealarm=0`, `movealarm=1` | Sets the movement alarm flag, which firmware 0.4.x does not act on. Reported back with the next record. | `movement alarm OFF`, `movement alarm ON` |
| `movereset` | Resets the movement alert back-off, and restores the engine-off interval if a movement alert had changed it. | `movement alarm reset` |
| `locate` | Reports the last known position; no new fix is taken. | `google: <lat>,<lon>` |
| `locatenow` | Builds and sends a record first, then reports the position. While driving that is a fresh fix; on a parked unit's timed report it is the last known position. | `google: <lat>,<lon>` |
| `tomtom`, `tomtomnow` | As `locate` and `locatenow`, with a TomTom link. | `tomtom: <lat>,<lon>` |
| `config` | Reports its firmware version, settings, battery, ignition and uptime. | `fw=<version> int=<n> ma=<n> tm=<n> bat=<volts>V ign=<on or off> up=<seconds>s` |
| `fota` | Checks for an update at once, or once the engine stops if it is running, ignoring the retry holdoff and clearing any version it had given up on. | `fota: check queued` |
| `reboot` | Restarts the next time its main loop runs: straight away while driving, at engine start or key-off when the ignition is on with the engine off, and at the next ignition-on for a parked unit. | `rebooting` |
| `track=0`, `track=1` | Sent by the server on every reply. Acted on only when it changes the mode; `track=1` is ignored while the ignition is off. | `track mode OFF`, `track mode ON` |
| `fota=<version>` | Sent by the server when a newer build is published. The tracker compares it with its own version and downloads only a newer one. | None; see the `fota:` alerts |

- The tracker finds each command by its name anywhere in the reply, so the order does not matter and a command runs at most once per reply.
- Positions in `google:` and `tomtom:` replies arrive as tappable links in Pushover.
- There is no `poweroff` command in firmware 0.4.x.

### Delivery

- Delivery is at most once. Commands leave the queue when they are put into a reply, so if the tracker misses that reply the command is lost. Look for the alert it sends back, and queue the command again if none arrives.
- Queuing a command removes queued commands with the same name; `locate` and `locatenow` replace each other, as do `tomtom` and `tomtomnow`. Commands queued in one call are stored together, and removed together when a later command replaces any one of them.

### Settings the server keeps

These are accepted by `command.py` and `POST /api/1.0/command` in the same way as commands, but they are written to the device's row straight away and never sent to the tracker.

| Setting | Column | New device | Effect |
|---|---|---|---|
| `alarm=0`, `alarm=1` | `alarm` | 0 | Notify when the ignition comes on |
| `garage=0`, `garage=1` | `garage` | 0 | Demote priority 2 alerts to 0, and leave the device out of home check notifications |
| `overnightalarm=0`, `overnightalarm=1` | `overnight_alarm` | 0 | Notify when the ignition comes on inside the overnight window |
| `overnight_alarm_hour_from=<hour>` | `overnight_alarm_hour_from` | 23 | First hour of the window (0-23), in the server's local time |
| `overnight_alarm_hour_to=<hour>` | `overnight_alarm_hour_to` | 6 | Hour the window ends (0-23); the window runs up to the start of this hour |

Values must be whole numbers. The alerts themselves are described in [Configure alerts](/deployment/alerts.html).

### fota-retry

`command.py <imei> fota-retry` is handled by the tool rather than the tracker. It clears the version the server is withholding from that device after a failed update, then queues `fota` so the tracker checks again.

## The config endpoint

`GET /api/1.0/config` and `POST /api/1.0/config` read and write the same settings directly, with a bearer token. The fields use short names:

| Field | Column |
|---|---|
| `int` | `int` |
| `ma` | `movement_alarm` |
| `al` | `alarm` |
| `ga` | `garage` |
| `oa` | `overnight_alarm` |
| `oaf` | `overnight_alarm_hour_from` |
| `oat` | `overnight_alarm_hour_to` |

A `POST` must name the device, and every value must be an integer:

```sh
curl -X POST https://tracker.example.com/api/1.0/config \
  -H 'Authorization: Bearer <token>' \
  -H 'Content-Type: application/json' \
  -d '{"imei": "350000000000000", "int": 3600}'
```

Unlike the `int=` command, writing `int` here changes the server's value directly. The tracker adopts it from its next reply without sending an alert, and values from 1 to 9 are not raised to 10.

## Examples

Set an hourly check-in and confirm the tracker has it. Both alerts arrive once it reads the reply:

```sh
sudo docker exec l0destar python tools/command.py 350000000000000 int=3600 config
```

Ask where the vehicle is:

```sh
sudo docker exec l0destar python tools/command.py 350000000000000 locate
```

Turn on ignition alarms between midnight and 5am:

```sh
sudo docker exec l0destar python tools/command.py 350000000000000 overnightalarm=1 overnight_alarm_hour_from=0 overnight_alarm_hour_to=5
```

Retry an update the server has stopped offering:

```sh
sudo docker exec l0destar python tools/command.py 350000000000000 fota-retry
```
