# Configure alerts

Alerts come from two places. The tracker raises them for things only it can see - movement, impacts and tilt while parked, a low battery, hardware faults, update progress and replies to commands. The server raises its own for ignition alarms, fault codes, update results and the home check. Both go out through one notification backend configured in the server's `config.yaml`.

## Choose a notification backend

The `notify` section of `config.yaml` selects the backend; every key is listed in [config.yaml](/reference/server.html#configyaml). Restart the server after changing it.

| Backend | What happens |
|---|---|
| `none` | Nothing is sent. Device alerts are still written to the transport's log, such as `udp.log`. |
| `pushover` | Push notifications through [Pushover](https://pushover.net/). |
| `webhook` | A JSON `POST` to a URL of your choice. |

A notification that fails is logged in `app.log` and never stops the telemetry being stored.

### Pushover

1. Create a Pushover account, install the app on your phone and note your user key.
2. Create an application in your Pushover account and note its API token.
3. Add both to `config.yaml`:

```yaml
notify:
  backend: pushover
  user: <pushover user key>
  app: <pushover application token>
  retry: 30
  expire: 300
```

Alert priorities are passed to Pushover as they are:

| Priority | Pushover behaviour |
|---|---|
| -2 | No notification |
| -1 | Quiet notification, no sound or vibration |
| 0 | Normal notification |
| 1 | High priority, bypasses quiet hours |
| 2 | Emergency: repeated every `retry` seconds until acknowledged or until `expire` seconds have passed |

Pushover requires `retry` to be at least 30 seconds and `expire` to be no more than 10800 seconds. Position replies (`google:` and `tomtom:`) arrive with a link that opens the Google Maps or TomTom app.

### Webhook

```yaml
notify:
  backend: webhook
  url: https://example.invalid/hook
  token: <optional, sent as Authorization: Bearer ...>
```

Every alert is sent as a `POST` with a JSON body and a 10 second timeout:

```json
{
  "title": "Tracker",
  "message": "Car: movement: 4.2deg tilt, 312mg",
  "priority": 2,
  "url": null,
  "url_title": null
}
```

`priority` uses the same -2 to 2 scale as Pushover; map it to whatever your receiver understands. `url` and `url_title` carry the map link for position replies. Anything that accepts a JSON `POST` will do - a home automation webhook, a small script, a chat bridge. To check your receiver before involving the tracker:

```sh
curl -X POST https://example.invalid/hook \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer <token>' \
  -d '{"title": "Tracker", "message": "Car: test", "priority": 0, "url": null, "url_title": null}'
```

## Alert catalogue

Device alerts arrive as `<name>: <text>`, where `<name>` is the device name the server has for the tracker. Priorities shown as *accel* come from `CONFIG_APP_ACCEL_ALERT_PRIORITY` (default 2) and drop to `CONFIG_APP_ACCEL_ALERT_BACKOFF_PRIORITY` (default 0) for `CONFIG_APP_ACCEL_ALERT_BACKOFF_S` (default 300 seconds) after one has been sent; see [Priorities](#priorities).

### From the tracker while parked

| Alert text | Priority | When |
|---|---|---|
| `movement: <tilt>deg tilt, <change>mg` | accel | Movement is confirmed after an accelerometer wake. The next can follow after 5, 15, 30 and then 60 minutes; the steps reset after 30 minutes without movement. |
| `parked impact <g>g (<ms>ms)` | accel | A sharp hit of at least 700mg (`CONFIG_APP_IMPACT_IMMEDIATE_MG`) lasting no longer than 300ms (`CONFIG_APP_IMPACT_IMMEDIATE_MAX_MS`), reported straight away. |
| `parked impact <g>g` | accel | A wake that turns out not to be movement, but peaked at 800mg or more (`CONFIG_APP_PARKED_IMPACT_MG`). |
| `tilt <angle>deg - possible tow/jack` | accel | The unit has tilted at least 6 degrees (`CONFIG_APP_TOW_TILT_DEG`, 0 disables) from its attitude when it went to sleep, checked every 30 seconds. Raised once, and re-armed when the vehicle is level again or after 15 minutes still at a new attitude (`CONFIG_APP_TOW_REARM_S`). |
| `low battery: <volts>V` | 0 | A record built with the ignition off for at least a minute reads below 11.9V (`CONFIG_APP_BATTERY_WARNING_MV`). Once, until the voltage recovers. See [Low battery alerts](#low-battery-alerts). |

The movement alarm setting (`ma`, `movealarm=`) does not affect any of these in firmware 0.4.x.

### From the tracker while awake

| Alert text | Priority | When |
|---|---|---|
| `impact <g>g x=<mg> y=<mg> z=<mg> gyr=<dps>dps dur=<ms>ms spd=<km/h>` | accel | An acceleration change above 4g (`CONFIG_APP_CRASH_THRESHOLD_MG`) with the ignition on. When the detail cannot be read it is sent as `impact: ><g>g (src=<code>) now=<x>/<y>/<z>mg spd=<km/h>`. |

### Hardware and updates

| Alert text | Priority | When |
|---|---|---|
| `SELFTEST:GPS rail fail` | 1 | At boot, the GPS rail did not report that it is up. |
| `SELFTEST:CAN 3V3 stuck`, `SELFTEST:K 3V3 stuck`, `SELFTEST:K 12V stuck` | 1 | At boot, a rail the self-test switched on and off again did not switch off. |
| `RAIL:<rail> fail` | 1 | A switched rail did not come up when the firmware turned it on. |
| `fota: <old> -> <new> available, downloading` | 0 | An update download is starting. Once per version. |
| `fota: <old> -> <new>, rebooting` | 0 | The image has downloaded and the tracker is restarting into it. |
| `fota: <old> -> <new> failed after <n> attempts (err <e>, cause <c>)` | 0 | Every download attempt in one check failed. Once per version. |
| `fota: <staged> failed to boot, reverted to <running>` | 0 | The new image did not confirm itself and MCUboot put the previous one back. |

The self-test and rail alerts mean a hardware fault; the [board test](/assembly/board-test.html) is the place to chase it.

### Replies to commands

| Alert text | Priority | Command |
|---|---|---|
| `engine-off interval changed; <old> -> <new>` | 0 | `int=` |
| `movement alarm ON`, `movement alarm OFF` | 0 | `movealarm=` |
| `movement alarm reset` | 0 | `movereset` |
| `google: <lat>,<lon>` | 0 | `locate`, `locatenow` |
| `tomtom: <lat>,<lon>` | 0 | `tomtom`, `tomtomnow` |
| `fw=<version> int=<n> ma=<n> tm=<n> bat=<volts>V ign=<on or off> up=<seconds>s` | 0 | `config` |
| `fota: check queued` | 0 | `fota` |
| `rebooting` | 0 | `reboot` |
| `track mode ON`, `track mode OFF` | 0 | Track mode switched from the web page |

What each command does is in [Commands](/reference/device-settings.html#commands).

### From the server

| Alert text | Priority | When | Controlled by |
|---|---|---|---|
| `<name> ignition on, battery <volts>V` | 2, or 0 in garage mode | The ignition comes on | `alarm` |
| `<name> overnight ignition on, battery <volts>V` | 2, or 0 in garage mode | The ignition comes on inside the overnight window | `overnight_alarm` and its hours |
| `<name>: fault code <code> raised`, `<name>: fault codes <codes> raised` | 1 | A fault code report from the K wire contains codes that were not already active | `CONFIG_APP_KLINE_DTC_REPORT` in the firmware |
| `<name>: fault code <code> cleared`, `<name>: fault codes <codes> cleared` | 0 | Codes that were active are missing from a report | As above |
| `<name>: fota: updated to <version>` | 0 | The tracker reports running the version it staged | - |
| `<name>: fota: <version> failed to boot (running <old>) — updates withheld until retried` | 1 | An update was reverted; the server stops offering that version to that device | `command.py <imei> fota-retry` |
| `<name>: tracker may be stalled - vehicle is <n>m from home` | 0 | The home check finds the last position away from home | `home_check`, garage mode |

The ignition alarms have no colon after the name.

## Priorities

The tracker picks the priority of each alert it raises, and the server passes it on with two adjustments:

- For a device in garage mode, priority 2 becomes 0 - for device alerts and the ignition alarms alike - and the home check leaves the device out.
- A `low battery` alert is dropped while the device's latest record has the ignition on, because a charging system moves the voltage around by design; otherwise it is sent at 0.

To change the accelerometer alert priorities for a vehicle, set these in its `remote.conf` section and publish with `push_fw.sh` (see [Deployment configuration](/deployment/configuration.html)):

| Symbol | Default | Meaning |
|---|---|---|
| `CONFIG_APP_ACCEL_ALERT_PRIORITY` | 2 | Priority of movement, tilt and impact alerts |
| `CONFIG_APP_ACCEL_ALERT_BACKOFF_S` | 300 | After one is sent, further ones within this many seconds are demoted; 0 disables |
| `CONFIG_APP_ACCEL_ALERT_BACKOFF_PRIORITY` | 0 | Priority used inside that window |

The window is fixed rather than sliding: once it ends, the next event is urgent again. For a vehicle that gets bumped all day, or a unit on the bench, 0 or -1 is a better base priority.

Garage mode is the quick switch for when someone else is going to move the vehicle, such as a workshop:

```sh
# on the server
sudo docker exec l0destar python tools/command.py 350000000000000 garage=1
```

Set it back to 0 afterwards.

## Ignition alarms

The ignition alarm notifies whenever the ignition comes on. The overnight alarm does the same only inside a window of hours, in the server's local time: from the first hour up to the start of the last one, wrapping past midnight when the first hour is later than the last. Both are server settings and apply straight away.

```sh
# on the server
sudo docker exec l0destar python tools/command.py 350000000000000 alarm=1
sudo docker exec l0destar python tools/command.py 350000000000000 overnightalarm=1 overnight_alarm_hour_from=0 overnight_alarm_hour_to=5
```

The same through the API, with a bearer token:

```sh
curl -X POST https://tracker.example.com/api/1.0/config \
  -H 'Authorization: Bearer <token>' \
  -H 'Content-Type: application/json' \
  -d '{"imei": "350000000000000", "al": 1, "oa": 1, "oaf": 0, "oat": 5}'
```

The alarm fires on the record where the ignition goes from off to on. The tracker sends that record as soon as the ignition comes on if it has a position to send, so the notification is quick.

## Fault code alerts

Fault code alerts need a K-wire build with `CONFIG_APP_KLINE_DTC_REPORT=y` and the vehicle's address found by discovery (see [Deployment configuration](/deployment/configuration.html)). The tracker reads the stored codes a few seconds after the ignition comes on, and again whenever the count of stored codes changes during a drive. It always sends the complete set, so the server alerts once per report on codes that appeared and on codes that cleared, and keeps the history in the `dtc` table:

```sql
SELECT t.code, t.raised_at, t.cleared_at, t.active
FROM `dtc` t
JOIN `device` d ON d.id = t.device_id
WHERE d.imei = '350000000000000'
ORDER BY t.raised_at DESC;
```

The firmware only reads fault codes; it never clears them.

## Home check

A tracker that stops reporting looks, from its last record, like a vehicle sitting wherever it last reported. The home check catches the case where that is not home: run it at a time the vehicle should be at home, and the server compares each configured device's last position with its home location.

Configure one entry per vehicle in `config.yaml` and restart the server:

```yaml
home_check:
  - imei: '350000000000000'
    latitude: 51.5000
    longitude: -0.1000
    radius_m: 300
  - imei: '350000000000001'
    latitude: 51.5000
    longitude: -0.1000
    radius_m: 300
```

Then call it from cron with a bearer token. The server listens on `127.0.0.1:5000`, so a cron job on the same machine can call it directly:

```text
# every night at 03:00
0 3 * * * curl -fsS -X POST -H 'Authorization: Bearer <token>' http://127.0.0.1:5000/api/1.0/home > /dev/null
```

Add `?imei=<imei>` to the URL to check a single device. For each device more than `radius_m` from home and not in garage mode, the server sends `<name>: tracker may be stalled - vehicle is <n>m from home`. If a vehicle is away for the night on purpose, put it in garage mode first.

## Low battery alerts

With the default thresholds, a parked tracker skips its timed report once the battery is below 12.0V, before it builds the record that would raise the 11.9V `low battery` alert - so the alert rarely arrives from a parked vehicle, and effectively never with no engine-off interval set. [Deployment configuration](/deployment/configuration.html) describes how to change the thresholds if you want the warning while parked.

## Test your alerts

With the ignition on, queue a `config` command. Within about 30 seconds the tracker reads it and sends its settings back as an alert, which proves the whole path from the tracker through the server to your phone. A parked tracker only picks up commands when it next reads a reply (see [Device settings and commands](/reference/device-settings.html)).

```sh
# on the server
sudo docker exec l0destar python tools/command.py 350000000000000 config
```

Then:

1. Queue `locate` and check that the alert's map link opens.
2. Set `alarm=1`, wait for the tracker to go to sleep, then turn the ignition on: an emergency priority notification should arrive. Set `alarm=0` afterwards if you do not want it permanently.
3. Test a parked movement alert as described in [Test drive](/deployment/test-drive.html).
