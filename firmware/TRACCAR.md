# Reporting to Traccar

The firmware can report to a [Traccar](https://www.traccar.org) server
instead of the l0destar server. It speaks Traccar's **OsmAnd protocol**: one
HTTP request per record, alert or fault-code report, on the port Traccar's
OsmAnd listener runs on (5055 by default). Nothing on the Traccar side needs
configuring beyond registering the device.

This is a build-time choice. `src/traccar.c` takes the place of
`src/transport.c`; the record builder, batching, the backlog and the alert
queue are the ones the l0destar server gets, so everything in
[PROTOCOL.md](../server/docs/PROTOCOL.md) still describes what the device
*produces* — this page describes how it lands in Traccar.

## Setting it up

1. In Traccar, add the device with its **IMEI as the identifier** (the
   firmware prints it at boot as `imei=...`), or whatever `APP_TRACCAR_ID`
   is set to. Traccar answers an unregistered identifier with HTTP 400, and
   the firmware drops such a report rather than retrying it, so do this
   before the unit first reports.

2. In `local.conf`:

   ```
   CONFIG_APP_TRACCAR=y
   CONFIG_APP_TRACCAR_HOST="traccar.example.org"
   # CONFIG_APP_TRACCAR_PORT=5055
   # CONFIG_APP_TRACCAR_ID=""          # empty: the IMEI
   # CONFIG_APP_TRACCAR_SEC_TAG=-1     # -1: plain HTTP
   CONFIG_APP_APN="..."
   ```

   `CONFIG_APP_SERVER_HOST` and `CONFIG_APP_PSK_HEX` are unused and can be
   left out. The build fails if `APP_TRACCAR_HOST` is empty.

3. `./build.sh` and `./flash.sh` as usual. The console shows
   `connected to traccar.example.org:5055` on the first send and
   `sent N requests` after each.

Traccar's OsmAnd listener is plain HTTP. For HTTPS, put a reverse proxy in
front of it and set `APP_TRACCAR_SEC_TAG` to a modem security tag holding
the CA its certificate chains to; the firmware does not store one there
itself (nothing in `certs/` is likely to be the right CA), so provision it
with `nrfcredstore` or `AT%CMNG`. The same value as `APP_FOTA_SEC_TAG` (42)
reuses the l0destar CA that `modem_provision_tls()` installs at boot, for a
proxy whose certificate that CA issued.

Point it at the OsmAnd port, not the web interface's 8082: the web server
answers every request with the page, and the firmware would take that as
delivered.

## What Traccar gets

Every telemetry record becomes one position. Traccar's own attribute keys
are used where it has one for the field, so its interface, computed
attributes and alarm notifications see the data as they would from any
other tracker; the rest keep the record's key.

| Record field | Traccar | Notes |
|---|---|---|
| `ts` | `timestamp` | Unix seconds, UTC. Omitted for a record built before the clock was set, and Traccar uses the time of receipt |
| `lat`, `lon` | `lat`, `lon` | Omitted when both are exactly 0 (a track-mode record on a unit that has never had a fix); Traccar then attaches the last position it holds |
| `spd` | `speed` | Converted from km/h to knots, which is what the protocol takes |
| `alt`, `hdg` | `altitude`, `bearing` | |
| `hdop` | `hdop` | Unscaled (the record carries HDOP × 10). Omitted when 0 |
| `sat` | `sat` | Omitted when 0 |
| `bat` | `power` | The vehicle battery. `power` is Traccar's name for the supply a tracker is wired to; its `battery` is the tracker's own, which is `vs` below |
| `ign` | `ignition` | `true` / `false`, so Traccar raises its ignition on/off events |
| `up` | `uptime` | Seconds since boot |
| `pon` | — | The powered-on flag; `resetCause` says the same |
| `cl` | `valid` | `valid=false` when the record was built from the stored position because there was no fix (`cl=1`) |
| `mcc`, `mnc`, `lac`, `cid` | `cell` | `mcc,mnc,lac,cid`, Traccar's cell-tower format, so its geolocation can use it |
| `rat` | `rat` | `CATM1` / `NBIOT` |
| `fw` | `versionFw` | |
| `rst` | `resetCause` | |
| `mt`, `it` | `deviceTemp`, `imuTemp` | °C |
| `vs` | `battery` | The nRF9151's supply rail, volts |
| `ax`..`gz` | `ax`..`gz` | Accelerometer in milli-g, gyro in raw LSB, as in the record |
| `tm` | `trackMode` | |
| `rid` | `recordId` | The device's own id for the record: consecutive within a boot, seeded at random at each one, so a gap is a record that never arrived |
| `rsrp`, `snr`, `band` | `rssi`, `snr`, `band` | Serving cell, dBm / dB / LTE band. `rssi` is Traccar's name for serving-cell power |
| `wt` | `wakeRecordId` + `wakeMs` + `wakeAttachMs` | Milliseconds awake for the send of record `wakeRecordId`, an earlier one, and how much of that was the LTE attach. Traccar has no way to amend a position it has already stored, so the pair goes out as two attributes of the record carrying them and the reader joins them on `recordId`; filing the duration against this record would say it took that long to send this one, which is the one thing it does not mean |
| `orpm`, `ormin`, `ormax`, `oravg` | `rpm`, `rpmMin`, `rpmMax`, `rpmAvg` | |
| `ospd` | `obdSpeed` | km/h |
| `ocl`, `oit` | `coolantTemp`, `intakeTemp` | °C |
| `old`, `oth` | `engineLoad`, `throttle` | % (the record's × 10 unscaled) |
| `omaf` | `maf` | g/s (× 100 unscaled) |
| `otim`, `ostft`, `oltft` | `timingAdvance`, `shortFuelTrim`, `longFuelTrim` | × 10 unscaled |
| `ofs`, `omil`, `odtc` | `fuelStatus`, `mil`, `dtcCount` | |
| `int`, `ma`, `ri` | — | The settings sync with the l0destar server |
| `acc` | — | The track-mode IMU burst: a kilobyte of raw samples, past the request-line limit and no use as an attribute |
| `dbg` | — | |

Anything the record carries that is not in the table goes out under its own
key, so a field added to the firmware later reaches Traccar without a
change here.

The other lines the device sends:

| Line | Traccar |
|---|---|
| `A,<priority>,<message>` | A position with no coordinates of its own — Traccar attaches the device's last — carrying `alert=<message>`, `priority=<n>` and, where the message is one Traccar has an alarm type for, `alarm=<type>`: movement → `movement`, impact while driving → `accident`, parked impact → `vibration`, tilt → `tow`, tamper → `tampering`, low battery → `lowPower`, backup power → `powerCut`, car power restored → `powerRestored`, self-test and rail faults → `fault`, a coolant or intake threshold alert (`CONFIG_APP_OBD_ALERTS`) → `temperature`. Any other alert at priority 1 or above is a `general` alarm; the informational ones (the replies to `locate` and `config`, update progress) carry only `alert` |
| `D,<codes>` | `dtcs=<codes>`, the complete set of stored fault codes, empty when there are none |
| `L,...` | Dropped. Captured warnings and errors are for the l0destar server's device log |
| `F,...` | Dropped. The failed-update verdict is also queued as an alert, which does get through |

A request is at most 1536 bytes, well inside the 4096 Traccar accepts. A
record that would not fit — there is none today — loses whole parameters
from the end, never part of one, and says so on the console.

## Commands

Traccar's **custom** command reaches the device. Send one from the device's
command menu (type *Custom*, the *data* field), or through the API:

```
POST /api/commands/send
{"deviceId": 7, "type": "custom", "attributes": {"data": "int=600"}}
```

Traccar keeps it until the device next reports and returns it in the
response to that request; the firmware hands it to the same command parser
the l0destar server's commands go through, so the vocabulary is the one in
[device settings](../docs/reference/device-settings.md): `int=<seconds>`,
`movealarm=<0|1>`, `movereset`, `locate`, `config`, `fota`, `reboot` and
the rest, several joined with commas. Whatever the command answers with
(`locate` answers with the position, `config` with the settings) arrives as
an alert. The device acts on a command when it reads a response, which is
every send while stationary or the ignition is off and every
`APP_RESP_POLL_S` (30 s) while driving — the same latency as with the
l0destar server.

## What the l0destar server does that Traccar cannot

- **Settings from the server.** The engine-off report interval and the
  movement alarm are set from the l0destar server's response; with Traccar
  they start at `APP_ENGINE_OFF_LOOP_INTERVAL` (0, meaning the 900 s boot
  default) and `APP_MOVEMENT_ALARM`, and change only by the `int=` and
  `movealarm=` commands above — until the next reboot, since settings live
  in RAM.
- **Track mode** is switched on by the l0destar server's response and has no
  Traccar equivalent.
- **Firmware updates.** The l0destar server advertises new builds on every
  response; Traccar cannot. The power-on check still runs against
  `APP_FOTA_HOST` if one is set — with none, the check is skipped and updates
  are over SWD only — and the `fota` command forces a check. See
  [FOTA.md](FOTA.md) for what the update server has to serve.
- **The device log.** Warnings and errors the firmware captures
  (`APP_DEBUG_LOG`) are dropped; they are on the console.
- **Encryption.** Telemetry is plain HTTP unless a TLS proxy is put in front
  of Traccar; the l0destar transport seals every datagram with the device
  key.

## On the radio

The l0destar transport is one UDP datagram per send and the modem releases
the radio the moment it is out. HTTP costs a TCP connection and a round
trip per request. The firmware keeps one connection open across sends
rather than opening one for each: with release assistance the modem drops
the RRC connection straight after each response, and closing the TCP
connection after that would need a fresh one just to carry the FIN, so
`transport_close()` only hints the release and keeps the socket, and the
socket is dropped before the modem is powered off for sleep. A request on
a connection the server has meanwhile closed — Traccar's own idle timeout,
or the modem having been cycled — fails and is retried once on a new one.

Records that cannot be delivered (no network, or Traccar answering 5xx)
stay in the backlog as they do with the l0destar server and go out when it
is back. A 4xx answer means a configuration problem — 400 is Traccar not
knowing the identifier — and such a report is dropped with an error on the
console rather than retried forever.
