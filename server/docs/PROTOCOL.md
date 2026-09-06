# Device protocol

Everything a device sends is a list of newline-separated ASCII lines, and
everything it gets back is one short comma-separated string. The three
transports differ only in how those bytes are wrapped and authenticated.

The format is shaped by one constraint: on LTE-M every byte is radio time, and
radio time is the whole power budget. A field that rarely changes is not sent
on every record, and the server reconstructs it.

## Records

```
ts,lat,lon,spd,alt,hdg,hdop,sat,bat,ign,waketime,pon[,extras...]
```

| Field | Meaning |
|---|---|
| `ts` | modem clock, `dd/mm/yy,HH:MM:SS+NN` — **contains a comma** |
| `lat`, `lon` | decimal degrees |
| `spd` | km/h (the server stores mph) |
| `alt` | metres |
| `hdg` | degrees true |
| `hdop` | horizontal dilution of precision |
| `sat` | satellites used |
| `bat` | vehicle battery, volts |
| `ign` | ignition state, 0 or 1 |
| `waketime` | seconds awake for this send |
| `pon` | powered-on flag |

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
buffer. `slim_response` drops the movement_alarm field — and with it the OTA
indication, which rides the same field.

Commands are deleted as they are handed over, so delivery is at-most-once. A
command lost to a dropped reply is re-queued by whoever issued it, which is
safer than replaying a `poweroff` after the operator has changed their mind.

## Transports

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
That is the price of not paying for a handshake; use DTLS if it matters more
than radio time.

### TLS over TCP — port 65481

```
[2] payload length, big-endian, <= 8192
[N] IMEI '\n' record '\n' record ...
```

The modem terminates TLS itself. The device authenticates only by presenting
an enrolled IMEI, which is not a secret — so either restrict who can reach the
port, or configure `tls_client_ca` for mutual TLS. The UDP transport's
per-device PSK is stronger in that respect.

The handshake timeout is deliberately much longer than the read timeout. A
device on LTE-M in weak signal has to get the certificate chain across before
it can reply, and setting this too low shows up as repeated handshake timeouts
on firmware downloads that never reach the HTTP layer at all.

### DTLS 1.2 with Connection ID — port 65482

LTE-M's Release Assistance Indication lets the device drop the radio the
instant it has nothing more to send, which is most of what makes multi-week
standby possible. The cost is that the operator's NAT rebinds the device to a
new source port on the next transmission, often on every wake. A plain DTLS
session is keyed on the 4-tuple and dies there, so every wake would pay for a
fresh handshake — the most expensive thing the device does.

Connection ID (RFC 9146) puts a session identifier in each record, so the
server matches by CID rather than address. The session survives an arbitrary
number of rebinds and the device pays for one handshake across its whole
deployment.

**This transport needs an out-of-tree library.** Python's `ssl` module has no
DTLS support at all and no maintained binding exposes CID, so the listener
drives a small C shared library wrapping mbedTLS with
`MBEDTLS_SSL_DTLS_CONNECTION_ID` enabled. That library is not yet part of this
repository. Point `dtls_lib` at a build of it, or leave it unset — the
listener disables itself cleanly and the other transports are unaffected.

Expected ABI:

```c
void *dtls_cid_init(const char *cert, const char *key,
                    const char *host, int port);
void *dtls_cid_accept(void *ctx, int timeout_ms);
int   dtls_cid_read(void *session, void *buf, int len, int timeout_ms);
int   dtls_cid_write(void *session, const char *buf, int len);
void  dtls_cid_session_free(void *session);
void  dtls_cid_free(void *ctx);
```

`dtls_cid_read` returns bytes read, 0 on timeout, negative on error. Payload
framing inside the record matches the TLS transport.

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

### Downloads share the telemetry TLS port

The public HTTPS name may terminate elsewhere, but port 65481 is already open
and already has a certificate the device trusts, so downloads ride it too. The
two protocols cannot be confused: a telemetry frame starts with a big-endian
length capped at 8192, while an HTTP request starts with `GE` or `HE` —
`0x4745` and `0x4845`, far above the cap. The first two bytes decide.

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

The download read timeout is much longer than the telemetry one because the
device goes quiet between ranges for as long as the radio makes it. A single
RRC re-establishment in weak signal outlasts a short timeout, and the
downloader has no resume — so one timed-out read costs the whole transfer and
the next attempt restarts at byte zero.
