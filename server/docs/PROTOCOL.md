# Device protocol

Everything a device sends is a list of newline-separated ASCII lines, and
everything it gets back is one short comma-separated string. Both travel over
UDP, encrypted and authenticated with the device's own key. Firmware updates
are downloaded separately, over TLS.

The format is shaped by one constraint: on LTE-M every byte is radio time, and
radio time is the whole power budget. A field that rarely changes is not sent
on every record, and the server reconstructs it.

## Records

```
ts,lat,lon,spd,alt,hdg,hdop,sat,bat,ign,up,pon[,extras...]
```

| Field | Meaning |
|---|---|
| `ts` | modem clock, `dd/mm/yy,HH:MM:SS+NN` — **contains a comma** |
| `lat`, `lon` | decimal degrees |
| `spd` | km/h (the server stores mph) |
| `alt` | metres |
| `hdg` | degrees true |
| `hdop` | horizontal dilution of precision **× 10**, a whole number: `12` is an HDOP of 1.2. The server stores the HDOP itself |
| `sat` | satellites used |
| `bat` | vehicle battery, volts |
| `ign` | ignition, 1 on, 0 off |
| `up` | seconds since boot, the same figure as the `up=` extra. Stored in the `waketime` column, whose name is older than the field's current meaning |
| `pon` | the device's powered-on flag; the server derives its own `powered_on` from the ignition transition |

Records built in track mode send `0` for `hdop` and `sat`, and `1` for `ign`.

`+NN` in the timestamp is quarter-hours east of UTC, per 3GPP `AT+CCLK`.

### Extras

Anything after the twelve fixed fields is a comma-separated group of
`key=value` pairs joined by semicolons:

```
...,mcc=234;mnc=10;lac=1a2b;cid=00112233,ax=12;ay=-4;az=1010,vs=4.54
```

| Key | Column | Sticky? |
|---|---|---|
| `ri` | request config in the response | per-packet |
| `int`, `ma` | settings the device believes it has | per-packet |
| `fw` | running firmware version | **carried forward** |
| `mcc`, `mnc`, `lac`, `cid` | serving cell | **carried forward** |
| `rat` | `CATM1` or `NBIOT` | **carried forward** |
| `cl` | position is cell-derived, not GNSS | per-packet |
| `ax`, `ay`, `az` | accelerometer, raw LSB | per-packet |
| `gx`, `gy`, `gz` | gyro, raw LSB at ±250 dps | per-packet |
| `mt`, `it` | MCU and IMU temperature, °C | per-packet |
| `up` | seconds since boot | per-packet |
| `vs` | SiP supply rail, volts | per-packet |
| `dr` | dead-reckoning flag | per-packet |
| `dbg` | debug counters — only after a fault | per-packet |
| `rst` | reset cause — only after a reset | per-packet |
| `orpm`, `ormin`, `ormax`, `oravg` | engine RPM, and its min/max/mean over the cycle | per-packet |
| `ospd` | ECU road speed, km/h | per-packet |
| `ocl`, `oit` | coolant and intake temperature, °C | per-packet |
| `old`, `oth` | engine load and throttle, % × 10 | per-packet |
| `omaf` | mass air flow, g/s × 100 | per-packet |
| `otim`, `ostft`, `oltft` | timing advance and fuel trims, × 10 | per-packet |
| `ofs`, `omil`, `odtc` | fuel system status bitmap, MIL lamp, stored code count | per-packet |
| `tm` | 1 = built in track mode: GNSS off, position is the last fix, speed the ECU's | per-packet |
| `acc` | IMU burst: `ax/ay/az/gx/gy/gz` per sample, samples joined by `:`, oldest first, 26 Hz; accel milli-g, gyro raw LSB | per-packet |

"Carried forward" means the device sends the field only when it changes or on
the first record after a wake, and the server copies the previous row's value
onto every row that omits it. A firmware version costs about ten bytes per
boot instead of ten bytes per record, and every row is still attributable to a
build.

`dbg` is compound — its own body uses semicolons — so it is parsed whole rather
than split like the others, and a trailing `;rst=` is separated off.

IMU readings are stored as raw LSB, unscaled. A change of full-scale range in
firmware would otherwise silently reinterpret every historical row.

The `o*` keys are OBD-II values read over the K wire, present only on vehicles
with a K interface and only for the PIDs that ECU supports. The device sends
integers with the scale factors above so a packet never carries a decimal
point; the server unscales them, converts the ECU's km/h to mph, and stores a
`combined_speed` column that is the ECU's road speed when reported and the
GNSS speed otherwise. That is what the interface shows: the vehicle's own
figure does not wander with a poor fix and reads a clean zero when stationary.

### Fault codes

A line beginning `D,` is the vehicle's complete set of stored diagnostic
trouble codes:

```
D,P0133,P0420
```

Because it is always the complete set, the server reconciles it against the
`dtc` table as a set difference: a code not already active is newly raised, and
an active code missing from the report has cleared. `D,` alone means "no
stored codes" and clears everything. Rows are never deleted, so the table is a
history of when each fault appeared and disappeared. One notification goes out
per report, not per code.

### Captured log lines

A datagram may carry, after the record, lines beginning `L,`:

```
L,<uptime_ms>,<E|W>,<module>: <text>
```

Each is a warning or error the firmware logged since its last successful send,
held in RAM until a datagram carrying it gets through. They are not stored;
each is appended to `device.log` on receipt, stamped with the wall-clock time
the device logged it, worked out from the `up=` field of the record it arrived
with. A line that arrives with no record to anchor it is stamped with the
receipt time, marked `(rx)`.

### Alerts

A line beginning `A,` is an alert rather than a position:

```
A,<priority>,<message>
```

Priority follows Pushover's scale: `-1` quiet, `0` normal, `2` requires
acknowledgement. A message prefixed `google: ` or `tomtom: ` carries
`lat,lon` and is turned into a tappable navigation link.

Two rules are applied server-side. A device marked `garage` has priority-2
alerts downgraded, because a vehicle that is expected to be moved should not
demand acknowledgement every time it is. And a low-battery alert is suppressed
while the ignition is on: the device tests an instantaneous voltage with no
engine gate, and smart or regenerative charging swings the bus from roughly
11.8 V to 14.9 V by design, so a low reading mid-drive is a false alarm. A
genuine resting low reading still relays.

## Response

```
1,<interval>[,<movement_alarm>][,<commands>][,fota=<version>][,track=<0|1>]
```

The leading `1` is the ack the firmware checks before clearing its send
buffer. `slim_response` drops the movement_alarm field, and with it everything
that follows: current firmware applies a reply only when it carries both the
interval and movement_alarm, so a slim reply delivers no settings, commands,
OTA indication or track-mode switch. Leave it off.

Commands are deleted as they are handed over, so delivery is at-most-once. A
command lost to a dropped reply is re-queued by whoever issued it, which is
safer than replaying a `reboot` after the operator has changed their mind.

## Transport

### UDP + ChaCha20-Poly1305 — port 65480

```
request:   [1] imei_len  [imei_len] IMEI ASCII  [12] nonce  [N] ciphertext  [16] tag
response:  [12] nonce  [N] ciphertext  [16] tag
```

AAD is the IMEI on both directions; the response additionally binds the
request's nonce, so a response captured from one exchange cannot be replayed
into another. The key is a 32-byte per-device PSK, stored as hex in
`device`.`psk`.

Replay protection is a 1024-nonce in-memory window per device, with the most
recent nonce persisted so a restart cannot reopen a hole for the single
most-recently captured datagram. The window is only updated *after* the tag
verifies — otherwise an unauthenticated packet could poison it with a nonce
the real device is about to use.

Every failure mode — malformed, unknown IMEI, missing key, bad tag, replay —
looks identical from outside, so responses cannot be used to enumerate which
IMEIs are enrolled.

The IMEI travels in the clear, because the server needs it to pick a key. An
observer on path learns which device is reporting, though not where it is.
That is the price of not paying for a handshake.

## Firmware updates

Every telemetry response carries `fota=<version>` when a newer build is
published for that unit. The device compares it against its own running
version locally and fetches nothing unless there is something newer — the
steady state costs no extra requests from the field.

Each unit gets its own image. Carrier board revision and fitted interfaces
(CAN vs K-line) differ between units, and an image for the wrong one installs
cleanly — MCUboot verifies the signature, not the hardware — and then
misbehaves in the field. So the publisher writes:

```
fw/l0destar-<version>-<imei>.bin
fw/manifest-<imei>.txt          version=<version>
                                file=<filename>
                                board=<board>
```

A device with no manifest is told about no update and gets a 404 on the
manifest, so it simply never updates. Failing to update is the safe direction.

### Downloads over TLS — port 65481

The device downloads manifests and images from the server's TLS listener on
port 65481. The modem terminates TLS itself, and trusts the server only if its
certificate was issued by the CA compiled into the firmware. The listener
serves firmware and nothing else: a connection that does not open with `GET`
or `HEAD` is closed without a reply.

The server implements only what the nRF91 FOTA stack issues: `GET` and `HEAD`
under `/fw/`, HTTP/1.1 keep-alive, and `Range`. The modem decodes about 2 KB
per TLS record, so an image arrives as a long run of sequential 2048-byte
ranged GETs on one connection, each expecting `206` with `Content-Range`.

Three endpoints:

| Path | Purpose |
|---|---|
| `/fw/manifest.txt?imei=<imei>&v=<running>` | redirected to that unit's manifest |
| `/fw/<filename>` | the image, with `Range` support |
| `/fw/published.txt` | every version ever published, oldest first |

`published.txt` exists so the publisher can pick the next patch number without
shell access on the server — the fleet's own state is the counter. Image
filenames count as well as manifests: a manifest is overwritten on each
publish, an image file never is, so the filenames are the durable record of
what has actually gone out.

The handshake timeout is deliberately much longer than the read timeout. A
device on LTE-M in weak signal has to get the certificate chain across before
it can reply, and setting this too low shows up as repeated handshake timeouts
on firmware downloads that never reach the HTTP layer at all.

The download read timeout is much longer than the one for the first request,
because the device goes quiet between ranges for as long as the radio makes it. A single
RRC re-establishment in weak signal outlasts a short timeout, and the
downloader has no resume — so one timed-out read costs the whole transfer and
the next attempt restarts at byte zero.
