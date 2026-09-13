# Deployment configuration

A deployed tracker gets its firmware configuration from its own section of `remote.conf`, published over the air with `push_fw.sh`, and its runtime settings from the server. This page covers what to set up for a vehicle, the K-wire discovery for a new vehicle, and what reporting while parked costs in battery.

## Firmware for the vehicle

`push_fw.sh` builds each device from the `[common]` section of `firmware/remote.conf` plus the section named after the device's IMEI. Your bench `local.conf` is not used at all, so everything the vehicle needs has to be in `remote.conf`. The file and the script are covered in [OTA updates](/board-setup/ota-updates.html); a typical vehicle looks like this:

```ini
[common]
CONFIG_APP_SERVER_HOST="tracker.example.com"
CONFIG_APP_APN="your.apn"
CONFIG_LTE_NETWORK_MODE_LTE_M_GPS=y

[350000000000000]
name    = Car
profile = makerdiary
CONFIG_APP_PSK_HEX="<64 hex characters>"
CONFIG_APP_BOARD_L0DESTAR_V3_4=y
CONFIG_APP_OBD_MODE=2
```

- `CONFIG_APP_APN` belongs here even if it is already in your `local.conf`. Without it the image falls back to the APN in `prj.conf`, which is not your SIM's.
- `CONFIG_APP_PSK_HEX` goes in each device's own section, because the server gives every device its own key.
- `CONFIG_APP_BOARD_L0DESTAR_V3_4=y` selects a v3.4 board (a v3.3 board uses `..._V3_3`); other revisions are listed in [Hardware](/reference/hardware.html).
- `CONFIG_APP_OBD_MODE` must match the board's interface selection pads: `0` for none, `1` for CAN, `2` for K-wire.

Never put these in a deployed unit's configuration:

- `CONFIG_APP_FOTA_INHIBIT=y` - the unit would never take another update;
- `CONFIG_APP_KLINE_DISCOVER=y` - the board stops after the discovery run instead of tracking;
- the debug overrides `CONFIG_APP_DEBUG_IGNITION` and `CONFIG_APP_DEBUG_BATTERY_MV`, or the boot-time test harnesses `CONFIG_APP_CAN_TEST`, `CONFIG_APP_KLINE_TEST` and `CONFIG_APP_ACCEL_TEST`. `push_fw.sh` refuses to publish a build with any of these five away from its production value.

After changing a device's section, publish for that device:

```sh
# in firmware/
./push_fw.sh --device 350000000000000
```

The tracker downloads the update once a reply it reads advertises it. That can happen while the vehicle is being driven: GNSS is stopped and no telemetry is sent while the image downloads, which typically takes a few minutes, and the tracker then restarts into the new image.

### Per-vehicle tuning

These are the settings most worth reviewing for a particular vehicle. Every symbol is described in [Firmware build options](/reference/firmware.html).

| Symbol | Default | Why you might change it |
|---|---|---|
| `CONFIG_APP_ACCEL_ALERT_PRIORITY` | 2 | Priority of movement, tilt and impact alerts; lower it for a vehicle that gets bumped a lot |
| `CONFIG_APP_ACCEL_ALERT_BACKOFF_S` | 300 | How long further accelerometer alerts are demoted after one |
| `CONFIG_APP_TOW_TILT_DEG` | 6 | Tilt that raises a tow or jack alert; raise it if the vehicle settles on its suspension, or 0 to disable |
| `CONFIG_APP_PARKED_IMPACT_MG` | 800 | Peak that turns a parked wake into an impact alert |
| `CONFIG_APP_IMPACT_IMMEDIATE_MG` | 700 | Short hits at least this hard are reported without waiting; 0 disables |
| `CONFIG_APP_CRASH_THRESHOLD_MG` | 4000 | Impact alert threshold while the tracker is awake |
| `CONFIG_APP_BATTERY_WARNING_MV` | 11900 | Low battery alert level |
| `CONFIG_APP_SLEEP_SAFETY_MV` | 12000 | Timed reports while parked are skipped below this |
| `CONFIG_APP_BATTERY_POWEROFF_MV` | 11800 | Below this, timed reports stop and the battery is only checked again a day later |
| `CONFIG_APP_FOTA_MIN_BATTERY_MV` | 12000 | Update downloads wait until the battery is above this |
| `CONFIG_APP_BATCH_SIZE` | 3 | Records per datagram while driving |
| `CONFIG_APP_TRACK_MODE` | y | Set `n` if track mode should not be available for this vehicle |

## K-wire vehicle discovery

A K-wire build needs to know which address the vehicle's engine control unit answers on, and at what data rate, before it can read engine data and fault codes. The firmware finds out with a discovery run, once per vehicle. The full reference is in [KWIRE_QUICKSTART.md](https://github.com/m4rkw/l0destar/blob/master/firmware/KWIRE_QUICKSTART.md) and [KWIRE.md](https://github.com/m4rkw/l0destar/blob/master/firmware/KWIRE.md).

!!! warning "Stationary vehicles only"
    Run discovery with the vehicle parked and the ignition on. On a vehicle that does not answer the standard address, it sweeps every address and can occupy the K line for up to 15 minutes.

You need a K-wire board (OBD mode 2, K pads bridged) wired to the vehicle's OBD pin 7, and a laptop with the firmware toolchain connected to the tracker's USB-C port.

### 1. Build and flash a discovery image

Put this in `firmware/local.conf`, with the values for your board, server and device:

```text
CONFIG_APP_BOARD_L0DESTAR_V3_4=y
CONFIG_APP_OBD_MODE=2
CONFIG_APP_SERVER_HOST="tracker.example.com"
CONFIG_APP_APN="your.apn"
CONFIG_APP_PSK_HEX="<64 hex characters>"
CONFIG_APP_FOTA_INHIBIT=y
CONFIG_APP_KLINE_DISCOVER=y
CONFIG_APP_KLINE_IDENT=y
CONFIG_APP_KLINE_DTC=y
```

`CONFIG_APP_FOTA_INHIBIT=y` stops the tracker replacing the discovery image with its published build. With the ignition on, build and flash, then open the console as in [watch the console](/board-setup/initial-flashing.html#watch-the-console):

```sh
# in firmware/
./build.sh && ./flash.sh
```

### 2. Wait for the summary

On an unknown vehicle the run can take up to 15 minutes. The board then stops instead of starting the tracker, so the output stays on the console. It ends with a summary and a suggested configuration block - on the reference vehicle, a 2006 Toyota Harrier:

```text
  Suggested local.conf:
    CONFIG_APP_KLINE_BAUD=9600
    CONFIG_APP_KLINE_ECU_ADDR=0x13
    CONFIG_APP_KLINE_INIT_ADDRS="13"
    CONFIG_APP_KLINE_INIT_FAST=n
    CONFIG_APP_KLINE_INIT_SWEEP=n
    CONFIG_APP_KLINE_DISCOVER=n
```

If nothing answers, the console says where the handshake stopped. The usual cause is the wiring or the interface pads rather than the protocol; see the troubleshooting section of KWIRE.md. Do not connect the L line just to try it - see [Deployment: read this first](/deployment/read-this-first.html).

### 3. Add the result to remote.conf

Add the address and data rate to the device's section, and turn on engine data and fault codes:

```ini
[350000000000000]
name    = Car
profile = makerdiary
CONFIG_APP_PSK_HEX="<64 hex characters>"
CONFIG_APP_BOARD_L0DESTAR_V3_4=y
CONFIG_APP_OBD_MODE=2
CONFIG_APP_KLINE_BAUD=9600
CONFIG_APP_KLINE_ECU_ADDR=0x13
CONFIG_APP_KLINE_TELEMETRY=y
CONFIG_APP_KLINE_DTC_REPORT=y
```

Only `CONFIG_APP_KLINE_BAUD` and `CONFIG_APP_KLINE_ECU_ADDR` are used at run time; the other suggested lines only affect discovery.

### 4. Put the tracker back on production firmware

The discovery image does not take updates, so it will not replace itself. Build and publish the production image, then flash that same image over USB:

```sh
# in firmware/
./push_fw.sh --device 350000000000000
pyocd load -t nrf91 --no-reset build_remote_350000000000000/merged.hex
pyocd reset -t nrf91 -m hw -O auto_unlock=false
```

The tracker is then running the version the server advertises, so it does not download it again. Take the discovery lines back out of your `local.conf`.

## Settings on the server

The server holds a few settings per device. Change them with `tools/command.py` or the config endpoint, both described in [Device settings and commands](/reference/device-settings.html).

| Setting | New device | Set with | Effect |
|---|---|---|---|
| Engine-off interval | 0 | `int=<seconds>` | Timed reports while parked - see below |
| Movement alarm flag | 1 | `movealarm=0` or `movealarm=1` | Not acted on by firmware 0.4.x |
| Ignition alarm | off | `alarm=1` | Notify when the ignition comes on |
| Garage mode | off | `garage=1` | Demote urgent alerts while the vehicle is expected to be moved |
| Overnight alarm | off, 23 to 6 | `overnightalarm=1`, `overnight_alarm_hour_from=<hour>`, `overnight_alarm_hour_to=<hour>` | Notify when the ignition comes on overnight |

For example, an hourly check-in:

```sh
# on the server
sudo docker exec l0destar python tools/command.py 350000000000000 int=3600
```

The alarms are covered in [Configure alerts](/deployment/alerts.html).

## Engine-off reporting and battery use

With the engine-off interval (`int`) above 0, a parked tracker wakes every `int` seconds, reads the battery, connects, sends one record, reads the server's reply - which is how settings, commands and update adverts reach a parked unit - and goes back to sleep. The record carries the last known position: GNSS is only started for a timed report when movement has been detected since the previous one.

Each wake costs battery. The author's bench figures at 12V are about 35.5µA asleep, roughly 0.85mAh a day, and about 10 seconds at 15mA on average for a timed report, roughly 0.04mAh each. From those:

| `int` | Reports a day | Added per day | Total per day | Over 30 days |
|---|---|---|---|---|
| 0 | 0 | 0 | about 0.85mAh | about 26mAh |
| 3600 (hourly) | 24 | about 1mAh | about 1.9mAh | about 56mAh |
| 900 (15 minutes) | 96 | about 4mAh | about 4.9mAh | about 146mAh |
| 300 (5 minutes) | 288 | about 12mAh | about 13mAh | about 386mAh |
| 60 (1 minute) | 1440 | about 60mAh | about 61mAh | about 1.8Ah |

Treat these as estimates. A report takes longer on a weak signal or when the modem has to search for the network, a cold battery delivers less of its capacity, and the vehicle's own standby drain is often much larger than the tracker's. If the vehicle is left for weeks at a time, measure its drain with the tracker fitted.

What the setting trades:

- With `int` 0 the server only hears from a parked tracker when something happens: the ignition, movement, an impact or a tilt. A tracker that has lost power or coverage looks exactly like a quiet one until the next drive. For a vehicle normally parked at home, the home check in [Configure alerts](/deployment/alerts.html) helps with that.
- With `int` above 0 the reports are a heartbeat, and queued commands and updates reach the parked unit without waiting for the next drive. Hourly roughly doubles the parked consumption, and is a reasonable place to start.
- The `int=` command raises values from 1 to 9 to 10, but the config endpoint does not. Intervals close to the length of a report keep the tracker awake almost all the time.

Other behaviour worth knowing:

- After every restart, including one after an update, the tracker reports every 900 seconds until its first reply brings back the server's value, so one lost reply cannot leave a parked unit silent. Those reports need a position, though: a unit that restarts where GNSS cannot see the sky - an underground car park after a key-off update, say - sends nothing until it gets a fix, and looks for one on timed wakes after 15 and 30 minutes, 1, 2 and 4 hours, then every 4 hours.
- A confirmed movement on a unit whose interval is 0, or longer than four hours, schedules one report four hours later, with GNSS; the reply to that report restores the server's interval.
- Ignition, movement, impact and tilt wake the tracker whatever the interval. The tilt check wakes the processor every 30 seconds for a single accelerometer read, which costs very little.
- Battery gates apply to timed reports: below 12.0V (`CONFIG_APP_SLEEP_SAFETY_MV`) the report is skipped, and below 11.8V (`CONFIG_APP_BATTERY_POWEROFF_MV`) timed reports stop and the battery is checked again a day later. Update downloads wait for 12.0V.

## Low battery alerts

The tracker raises its `low battery` alert when it builds a record with the ignition off for at least a minute and the battery below `CONFIG_APP_BATTERY_WARNING_MV` (11.9V). While parked, the only records it builds are timed reports, and with the default thresholds a timed report is skipped below 12.0V before its record is built. In practice the alert rarely arrives from a parked vehicle, and effectively never with `int` 0.

If you want the warning while parked, set `CONFIG_APP_SLEEP_SAFETY_MV` below `CONFIG_APP_BATTERY_WARNING_MV` in the device's section - 11850, for example - and use an engine-off interval above 0. Timed reports between the two levels are then sent and raise the alert, at the cost of sending those reports on a low battery. Otherwise, keep an eye on the battery voltage the tracker reports on the map page.

## Track mode

Track mode is not set per vehicle: it is switched on from the web page for a session and ends when the ignition goes off. It is built in by default; set `CONFIG_APP_TRACK_MODE=n` in a device's section if it should not be available for that vehicle. See [Web interface and API](/board-setup/web-interface.html), and the driving note in [Deployment: read this first](/deployment/read-this-first.html).
