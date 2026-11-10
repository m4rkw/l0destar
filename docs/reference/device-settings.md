# Device settings and commands

The tracker takes its settings from the server on every reply it reads, runs the commands queued for it, and reports some of its state back. This page is the reference for that runtime interface. Build-time options are in [Firmware build options](firmware.md), and the battery cost of the engine-off interval is covered in [Deployment configuration](../deployment/configuration.md).

## How a reply reaches the device

The server answers every datagram it accepts with a single line:

```text
1,<int>,<ma>[,<commands>][,backup=<seconds>][,fota=<version>][,track=<0|1>]
```

| Part | Meaning |
|---|---|
| `1` | Acknowledgement |
| `<int>` | The device's engine-off interval, from the server |
| `<ma>` | The device's movement alarm flag, from the server |
| `<commands>` | Every command queued for the device, comma-separated; they leave the queue as they are sent |
| `backup=<seconds>` | The device's backup interval, on every reply: the timed-report interval while the inline battery backup module is carrying the tracker, `0` for "the same as `<int>`" |
| `fota=<version>` | The newest build published for this device, when there is one that has not been withheld after a failed update |
| `track=0` or `track=1` | The track mode switch, on every reply |

When the tracker reads a reply it:

1. applies `<int>` and `<ma>` - unless it has changed them itself since its last record, in which case it sends its own values (`int=<n>;ma=<n>`) with its next record and the server adopts them;
2. applies `backup=` as it reads it, and takes it out of what follows: it is a setting, not a command, and unlike `<int>` the tracker never reports it back, because nothing on the tracker changes it;
3. acts on everything else after `<ma>`: the queued commands, the update advert (`fota=`) and the track mode switch (`track=`).

It reads at most 127 characters after `<ma>`, so do not queue more than a few commands at once.

Settings live in the tracker's RAM. After a restart, until the first reply arrives, it uses:

- an engine-off interval of 900 seconds, so that one lost reply cannot leave a parked unit silent;
- no backup interval (`CONFIG_APP_BACKUP_LOOP_INTERVAL`, 0 by default), so the engine-off interval applies on backup power too;
- the movement alarm flag from `CONFIG_APP_MOVEMENT_ALARM` (off by default);
- track mode off.

## Settings

| Setting | Reply field | Server column | New device | Meaning |
|---|---|---|---|---|
| Engine-off interval | `<int>` | `device.int` | `0` | Seconds between timed reports while parked; `0` means none |
| Backup interval | `backup=` | `device.backup_int` | `0` | Seconds between timed reports while the inline backup module is carrying the tracker; `0` means the engine-off interval applies then too. Set with `backup_interval=<seconds>` or `backup` in `POST /api/1.0/config`; see [Reporting interval on backup power](../deployment/configuration.md#reporting-interval-on-backup-power) |
| Movement alarm | `<ma>` | `device.movement_alarm` | `1` | Stored, synced and reported, but not acted on by firmware 0.4.x |
| Track mode | `track=` | `device.track_mode` | `0` | GNSS off, engine and IMU data streamed quickly |
| Device LEDs | `le=` | `device.leds` | `1` | `1` lets the status LEDs light, `0` keeps them all dark. Sent on every reply straight after `gw=`; set with `leds=<0\|1>` or `le` in `POST /api/1.0/config` |
| Runtime tunables | `tu=` | 16 nullable `device` columns, [below](#runtime-tunables) | NULL | Optional overrides of values the firmware is built with. NULL means no override: the unit keeps the value it was built with. Sent on every reply straight after `le=` as `tu=<code>:<value>[;...]` (`tu=-` when none is set) |
| Update advert | `fota=` | per-device manifest | - | See [OTA updates](../board-setup/ota-updates.md) |

- A timed report while parked sends the last known position. GNSS is only started for it when movement has been detected since the previous one, or when the tracker has had no fix since it started. Ignition, movement, impact and tilt wake the tracker whatever the interval is.
- Movement, impact and tilt alerts are raised whatever the movement alarm flag says. Use the priority settings in [Configure alerts](../deployment/alerts.md) to quieten them.
- Track mode is switched from the web page (see [Web interface and API](../board-setup/web-interface.md)). The server clears it when the ignition goes off, or when a device is heard from after an hour of silence, and the tracker ignores `track=1` while the ignition is off.

## When the device reads a reply

A queued command waits until the tracker next reads a reply.

| Tracker state | Reads a reply | A command waits up to |
|---|---|---|
| Driving | At ignition changes, while the vehicle is reported stationary, and at least every `APP_RESP_POLL_S` (30 seconds) | about 30 seconds |
| Ignition on, engine off | Every `APP_IGNITION_ON_SLEEP_INTERVAL` (30 seconds) | about 30 seconds |
| Track mode | Every `APP_TRACK_RESP_INTERVAL_S` (10 seconds) | about 10 seconds |
| Parked, interval above 0 | On every timed report | the interval |
| Parked, interval 0 | Not until the ignition comes on, or a timed report scheduled after movement | the next drive |

After a movement alert on a unit whose interval is 0 or longer than four hours, the tracker schedules a single timed report four hours later, with GNSS; the reply to that report puts the server's interval back. On backup power the interval that counts is the backup interval, when there is one: if it is longer than four hours the countdown is cut to four hours, once, and the setting is left alone.

While the backup module is carrying the tracker, "the interval" in the table above is the backup interval when one is set, and a parked unit with an engine-off interval of 0 still reads a reply at each of those timed reports.

## Commands

Queue commands from the server with `tools/command.py`, or with `POST /api/1.0/command` and a bearer token (see [Web interface and API](../board-setup/web-interface.md)). Several commands in one call are joined with commas and delivered in the same reply.

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
| `movereset` | Resets the movement alert back-off, and restores the engine-off interval the tracker had when it last went to sleep; the next reply from the server replaces it with the server's value. | `movement alarm reset` |
| `locate` | Reports the last known position; no new fix is taken. | `google: <lat>,<lon>` |
| `locatenow` | Builds and sends a record first, then reports the position. While driving that is a fresh fix; on a parked unit's timed report it is the last known position. | `google: <lat>,<lon>` |
| `tomtom`, `tomtomnow` | As `locate` and `locatenow`, with a TomTom link. | `tomtom: <lat>,<lon>` |
| `config` | Reports its firmware version and board, settings, battery, ignition and uptime. | `fw=<version> board=<id> int=<n> ma=<n> tm=<n> backup=<n> bat=<volts>V ign=<on or off> up=<seconds>s` |
| `fota` | Checks for an update at once, or once the engine stops if it is running, ignoring the retry holdoff and clearing any version it had given up on. | `fota: check queued`, or `fota: updates inhibited` on a unit built with `CONFIG_APP_FOTA_INHIBIT` |
| `reboot` | Restarts the next time its main loop runs: straight away while driving, at engine start or key-off when the ignition is on with the engine off, and at the next ignition-on for a parked unit. | `rebooting` |
| `track=0`, `track=1` | Sent by the server on every reply. Acted on only when it changes the mode; `track=1` is ignored while the ignition is off. | `track mode OFF`, `track mode ON` |
| `fota=<version>` | Sent by the server on every reply while a build is published for the device and not withheld. The tracker compares it with its own version and downloads only a newer one. | None; see the `fota:` alerts |

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

Values must be whole numbers. The alerts themselves are described in [Configure alerts](../deployment/alerts.md).

### fota-retry

`command.py <imei> fota-retry` is handled by the tool rather than the tracker. It clears the version the server is withholding from that device after a failed update, then queues `fota` so the tracker checks again.

## The config endpoint

`GET /api/1.0/config` and `POST /api/1.0/config` read and write the same settings directly, with a bearer token. The fields use short names:

| Field | Column |
|---|---|
| `int` | `int` |
| `backup` | `backup_int` |
| `ma` | `movement_alarm` |
| `al` | `alarm` |
| `ga` | `garage` |
| `oa` | `overnight_alarm` |
| `oaf` | `overnight_alarm_hour_from` |
| `oat` | `overnight_alarm_hour_to` |
| `gw` | `gnss_off_window` |
| `gwf` | `gnss_off_hour_from` |
| `gwt` | `gnss_off_hour_to` |
| `le` | `leds` |
| `xtt` | `tow_tilt_deg` |
| `xpi` | `parked_impact_mg` |
| `xii` | `impact_immediate_mg` |
| `xap` | `accel_alert_priority` |
| `xab` | `accel_alert_backoff_s` |
| `xoa` | `obd_alerts` |
| `xis` | `ignition_on_sleep_s` |
| `xbw` | `battery_warning_mv` |
| `xbp` | `battery_poweroff_mv` |
| `xds` | `drive_stream` |
| `xdi` | `drive_stream_interval_s` |
| `xtp` | `track_period_ms` |
| `xdd` | `databuf_drain_s` |
| `xns` | `network_search_s` |
| `xrp` | `resp_poll_s` |
| `xlw` | `led_accel_wake` |

A `POST` must name the device, and every value must be an integer (the runtime tunables below can also be `null`, and `xoa` is text):

```sh
curl -X POST https://tracker.example.com/api/1.0/config \
  -H 'Authorization: Bearer <token>' \
  -H 'Content-Type: application/json' \
  -d '{"imei": "350000000000000", "int": 3600}'
```

Unlike the `int=` command, writing `int` here changes the server's value directly. The tracker adopts it from its next reply without sending an alert, and values from 1 to 9 are not raised to 10.

## Runtime tunables

These are values the firmware is normally built with (Kconfig options). Each can be overridden per device from the server. The unit holds the overrides in RAM only, and the server sends the full set on every reply, so a rebooted unit gets them back with its first reply.

**NULL means no override: the unit keeps the value it was built with.** Every column starts NULL, so a device you have not touched behaves exactly as before, and the values in `remote.conf` stay meaningful. Setting a column to NULL again takes the override off and the unit goes back to its built value.

| Column | `/config` code | Range | Overrides | Notes |
|---|---|---|---|---|
| `tow_tilt_deg` | `xtt` | 0-30 deg | `APP_TOW_TILT_DEG` | - |
| `parked_impact_mg` | `xpi` | 300-2500 mg | `APP_PARKED_IMPACT_MG` | - |
| `impact_immediate_mg` | `xii` | 0-1200 mg | `APP_IMPACT_IMMEDIATE_MG` | 0 = never |
| `accel_alert_priority` | `xap` | -2 to 2 | `APP_ACCEL_ALERT_PRIORITY` | - |
| `accel_alert_backoff_s` | `xab` | 0-86400 s | `APP_ACCEL_ALERT_BACKOFF_S` | - |
| `obd_alerts` | `xoa` | rule list, up to 120 characters | `APP_OBD_ALERTS` | see below |
| `ignition_on_sleep_s` | `xis` | 10-600 s | `APP_IGNITION_ON_SLEEP_INTERVAL` | - |
| `battery_warning_mv` | `xbw` | 11000-12500 mV | `APP_BATTERY_WARNING_MV` | - |
| `battery_poweroff_mv` | `xbp` | 10500-12200 mV | `APP_BATTERY_POWEROFF_MV` | must be below `battery_warning_mv` when both are set |
| `drive_stream` | `xds` | 0 or 1 | `APP_DRIVE_STREAM` | - |
| `drive_stream_interval_s` | `xdi` | 0-900 s | `APP_DRIVE_STREAM_INTERVAL_S` | 0 = as fast as possible, otherwise at most one record every N s |
| `track_period_ms` | `xtp` | 200-5000 ms | `APP_TRACK_PERIOD_MS` | - |
| `databuf_drain_s` | `xdd` | 1-600 s | `APP_DATABUF_DRAIN_S` | - |
| `network_search_s` | `xns` | 60-86400 s | `APP_NETWORK_SEARCH_TIMEOUT` | - |
| `resp_poll_s` | `xrp` | 5-600 s | `APP_RESP_POLL_S` | - |
| `led_accel_wake` | `xlw` | 0 or 1 | `APP_LED_ACCEL_WAKE` | - |

- `GET /api/1.0/config` shows each as its value or `null` (the override only); `GET /api/1.0/devices` lists them under their column names. Both also carry two objects keyed by code: `tunables_built` (the values the unit reported it was built with, `null` for any it has not reported) and `tunables` (the value in use: the override if set, else the built value, else `null`). Use `tunables` to show what a unit really runs; `xoa` is a string with real commas, `""` for no rules.
- The unit reports its built values itself, as `tb=` in the record that carries `fw=` and `bd=` (once per boot, the built `CONFIG_APP_*` values, not the current overrides). The server keeps the text in `device`.`tunables_built` (VARCHAR(400), NULL until reported) only when it parses; unknown codes and bad values are ignored. Built values are not held to the override ranges.
- `POST /api/1.0/config` takes them by code. A whole number outside the range, a non-integer, or text where a number is expected is refused with HTTP 400, as is a `battery_poweroff_mv` that is not below `battery_warning_mv` once the change is applied (the stored value of the other one counts when only one is posted). `null` clears one back to NULL; for `xoa`, `""` clears it too.
- The `command` endpoint and `tools/command.py` take them by column name, for example `tow_tilt_deg=8`. They are applied straight away and never queued. `<name>=default` (or `<name>=` with nothing after it) clears one to NULL. For `obd_alerts` write `|` for the comma between rules, since a comma separates commands: `obd_alerts=coolant>=95|rpm>6500`.
- `obd_alerts` is the rule list of `CONFIG_APP_OBD_ALERTS`: comma-separated rules `<field><op><value>[/<hysteresis>]`, such as `coolant>=95,rpm>6500,stft<-12.5/2`. The operators are `>` `>=` `<` `<=` `=`. It is at most 120 characters of printable ASCII with no `;` or `|`, and every rule must match that shape (the field names are the unit's business). An empty list is not a list: it means no override.
- The unit also clamps what it is sent to the same ranges, and keeps the power-off level at least 100 mV below the warning level.

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
