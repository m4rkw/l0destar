# Verifying telemetry

With the device enrolled, check the whole path - device, network, server, database and web interface - while the board is still on the bench, where problems are easy to see and fix.

Keep a console open on the device (see [minimal config and initial flashing](/board-setup/initial-flashing.html#watch-the-console)) and a shell on the server.

## On the device

Every send prints the records it carries and the size of the datagram. When the device waits for the server's reply, it prints that too:

```text
<inf> data: [1/1] 13/09/26,11:02:01.000000+00,51.500000,-0.100000,0.00,31.00,90.00,9,9,12.41,1,512,0,...
<inf> transport: sent 212 bytes
<inf> data: resp: 1,0,1,track=0
```

Each record starts with the clock (UTC), latitude, longitude, speed (km/h), altitude, heading, HDOP in tenths, satellites, battery voltage, ignition (`1` on), seconds since the firmware started and a power-on flag. The `key=value` extras after that are described in the [telemetry protocol](https://github.com/m4rkw/l0destar/blob/master/server/docs/PROTOCOL.md). A reply always starts with `1`, then the engine-off interval and the movement alarm flag, then any queued commands, an update advert and the track mode switch.

The device does not wait for a reply after every send - while driving it mostly does not - so switch pin 5 on with the supply below 13.0V: in that state it sends and reads a reply about every 30 seconds.

A device that keeps printing `sent` but never `resp:` is being ignored by the server; see [troubleshooting](#troubleshooting).

## In the server logs

```sh
tail -f /srv/l0destar/logs/udp.log
```

| Line | Meaning |
|---|---|
| `UDP listening on 0.0.0.0:65480` | The listener started (once, at start-up). |
| `3 records from 350000000000000 (203.0.113.7)` | A datagram was accepted. The count includes alert and log lines that rode along. |
| `alert from 350000000000000 (pri=0): ...` | The device raised an alert, and it was passed to your notification backend. |
| `delivered 1 commands to 350000000000000: config` | Queued commands went out in a reply. |
| `decrypt failed from 203.0.113.7 (212 bytes)` | Something reached the server but did not authenticate. The first failure from an address in a minute is logged, then every twentieth. |
| `record error IMEI=350000000000000: ...` | A record was malformed and skipped; the rest of the datagram was stored. |

Two other files in the same directory are worth a look after anything goes wrong: `device.log` holds the warnings and errors the firmware captured and sent up with later records, and `debug.log` lists the records that reported a reset cause or fault counters.

## In the database

```sh
sudo docker exec -it l0destar mariadb tracker
```

That opens the database inside the container, as its administrator.

The last ten records from the device:

```sql
SELECT l.id, l.timestamp, l.latitude, l.longitude, l.speed, l.satellites,
       l.battery_level, l.ignition_state, l.rat, l.fw
FROM log l JOIN device d ON d.id = l.device_id
WHERE d.imei = '350000000000000'
ORDER BY l.id DESC LIMIT 10;
```

`timestamp` is when the server received the record and `speed` is in mph. Journeys opened and closed by the ignition:

```sql
SELECT id, start_time, end_time, miles
FROM journey
WHERE device_id = (SELECT id FROM device WHERE imei = '350000000000000')
ORDER BY id DESC LIMIT 5;
```

## Checklist

| Check | How | What you should see |
|---|---|---|
| Position | Latest records, or the map | Coordinates of where the antenna is, and four or more satellites once it has a fix. A device that has never had a fix sends `0.000000`. |
| Battery | `battery_level` | Within a few tens of millivolts of your bench supply. |
| Ignition | Switch pin 5 on, wait for a record, switch it off | `ignition_state` follows the switch. A journey opens when the ignition comes on and closes after it goes off; switching back on within 5 minutes continues the same journey. |
| Firmware | `fw` | `0.4.0` for a bench build. It is sent after a restart and copied onto every later record. |
| Network | `rat`, `mcc`, `mnc` | `rat` is `CATM1`. The map shows the operator's name once the server has operator data (`tools/import_plmn.py`, see the [server reference](/reference/server.html)). |
| Replies and commands | `command.py <imei> config` | Once the device next reads a reply, an alert such as `fw=0.4.0 int=0 ma=1 tm=0 bat=12.4V ign=on up=512s`. |
| Web interface | Sign in | The device in the device list, with a recent last-seen time, and its position on the map. |

To queue the `config` command, on the server:

```sh
sudo docker exec l0destar python tools/command.py 350000000000000 config
```

If no notification backend is configured yet, the alert still appears in `udp.log` as `alert from ...`.

## Troubleshooting

| Symptom | Likely cause | What to do |
|---|---|---|
| No `connected` at boot | The modem has not registered on the network. | Give it a few minutes; if it still does not connect, fix the SIM, APN, antenna or coverage, then reset. See [no connected line](/board-setup/initial-flashing.html#no-connected-line). |
| `connected`, `sent`, but nothing at all in `udp.log` | The datagrams never arrive: DNS, no IPv4 address for the hostname, a firewall or missing port forward, or a wrong APN (the modem can register without a working data connection). | Test the port from outside, as in [telemetry port](/server/telemetry-port.html). Check `CONFIG_APP_SERVER_HOST` resolves to an IPv4 address. |
| `decrypt failed from ...` in `udp.log` | The device is not enrolled, or the key or IMEI does not match. | Check the IMEI with `device.py show`. If in doubt about the key, give the server the one in `local.conf` again with `device.py rekey <imei> --psk <key>`. |
| `records from ...` in the log, but never a `resp:` line | The server's replies do not get back, or the device is not waiting for them. | Test with pin 5 on and the supply below 13.0V, where every send waits for a reply. Check nothing between the server and the internet drops outgoing UDP. |
| `no GPS fix`, `no fix, skipping send`, or `reporting from last known position` | No usable GNSS signal. Until the first fix since start-up nothing is sent at all. | Active antenna on the GNSS connector, a view of the sky, and patience: an unassisted first fix takes 2 to 5 minutes. |
| `battery_level` near zero | No 12V on pin 4, or an INA228 fault. | Check the bench lead, then the voltage stage of the [board test](/assembly/board-test.html). |
| `ignition_state` never changes | Nothing reaches pin 5. | Check the bench lead's switch and wiring. |
| A command never arrives | The device has not read a reply since it was queued. | `device.py show <imei>` lists queued commands. Switch pin 5 on to make the device read replies. |
