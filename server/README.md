# l0destar server

Backend for the l0destar tracker: telemetry ingestion, storage, alerting,
over-the-air firmware delivery, and a map UI.

Self-hosted by design. There is no cloud service to sign up for and no
account anywhere but your own — which is most of the point of the project. A
vehicle tracker publishes where you are in real time, and that data should
live on hardware you control.

> **Status: draft.** This is a public implementation derived from a working
> private deployment. It is refactored and de-personalised rather than
> line-for-line, and has not been run end to end against real hardware in this
> form. Treat it as a reference to read and adapt, not something to point a
> vehicle at unreviewed.

## What it does

- **Ingests telemetry** over three transports — UDP with ChaCha20-Poly1305,
  modem-terminated TLS, and DTLS with Connection ID. A device speaks one of
  them; see [`docs/PROTOCOL.md`](docs/PROTOCOL.md).
- **Stores** positions, IMU, cell, thermal and power telemetry, and derives
  journeys from ignition transitions.
- **Alerts** on ignition, movement and low battery, through Pushover, a
  webhook, or nothing at all.
- **Delivers firmware** as per-device signed images with range support, over
  the same TLS port as telemetry.
- **Serves a map** with live position over a WebSocket and journey replay.
- **Queues commands** for delivery on a device's next check-in.

Authentication is passkeys only. There is no password column in the schema.

## Layout

```
tracker/            application package
  config.py           configuration loading
  db.py               MySQL handles, device and operator lookup
  logs.py             per-transport log channels
  notify.py           pluggable outbound notifications
  telemetry.py        parsing, storage, journeys, response building
  firmware.py         OTA manifests and the firmware HTTP server
  listeners/          udp.py, tls.py, dtls.py
  web/                Flask app, passkey auth, JSON API, WebSocket
tools/              enrolment and operations scripts
deploy/             nginx, systemd and launchd examples
docs/PROTOCOL.md    the device-facing wire protocol
schema.sql          MySQL schema
wsgi.py             entrypoint
```

The listeners and the web application share the telemetry layer and nothing
else; either runs without the other.

## Setup

Requires Python 3.9+ and MySQL or MariaDB.

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt

mysql -u root -p -e "CREATE DATABASE tracker CHARACTER SET utf8mb4"
mysql -u root -p tracker < schema.sql

cp config.yaml.example config.yaml
chmod 600 config.yaml
$EDITOR config.yaml          # at minimum: session_secret, database
```

Enrol a device and a user:

```sh
tools/adddevice.py 350000000000000 "Car" AB12CDE
tools/regtoken.py alice tracker.example.com
```

`adddevice.py` prints a pre-shared key once — it has to be built into that
unit's firmware and is not shown again. `regtoken.py` prints a single-use
enrolment URL valid for 24 hours; open it on the device that will hold the
passkey.

Run it:

```sh
.venv/bin/gunicorn -c gunicorn.conf.py wsgi:app
```

`deploy/` has nginx, systemd and launchd examples. Put nginx in front of the
web UI; the telemetry listeners bind their own ports and want forwarding at
the firewall rather than proxying.

### Certificates

For the TLS and DTLS transports the device needs to trust the server. The
firmware embeds a CA certificate, so a self-signed CA is the simplest thing
that works and avoids depending on a public CA's renewal cadence for something
that has to keep working with a device in a car park:

```sh
certs/gen_certs.sh tracker.example.com
```

This writes `certs/ca.{key,crt}`, `certs/server.{key,crt}` and a
`src/ca_cert.h` for the firmware build. The server key never leaves the
server. Certificates are gitignored.

## Migrations

`schema.sql` is the full current schema for a new database.  An existing one
takes the ALTER statements in `migrations/`, oldest first, each applied once:

```
mysql -u root -p tracker < migrations/2026-09-06_track_mode.sql
```

## Operating

```sh
tools/command.py 350000000000000 int=3600     # reporting interval
tools/command.py 350000000000000 locate       # position on next check-in
tools/command.py 350000000000000 alarm=1      # notify on ignition
tools/gentoken.py home-automation             # bearer token for the API
tools/import_plmn.py plmn.csv                 # operator names for the UI
```

Nothing is pushed to a device. It is asleep almost all of the time, so a
command waits in the queue until it next reports and rides back on the response
it was already going to receive — which at a long reporting interval can be an
hour away.

Logs are one file per transport under `log_dir`, plus `debug.log`, which only
receives records carrying debug counters or a reset cause. That keeps the
short chronological list of things that went wrong out of the bulk traffic.

## API

Browser endpoints are session-authenticated. Automation endpoints take
`Authorization: Bearer <token>` from `tools/gentoken.py`. Tokens are unscoped —
anything holding one can queue a command, so treat one as console-equivalent.

| Endpoint | Auth | Purpose |
|---|---|---|
| `GET /api/1.0/carpos` | session | latest position |
| `GET /api/1.0/journeys` | session | journey list, paged |
| `GET /api/1.0/journey/<id>/points` | session | one journey's track |
| `GET /api/1.0/devices` | session | enrolled devices |
| `GET /api/1.0/status` | session | settings, firmware, last seen |
| `GET,POST /api/1.0/trackmode` | session | read or set the track-mode switch (`{"on": 1}`) |
| `GET /ws/carpos` | session | live position stream |
| `GET /api/1.0/track` | bearer | redirect to a map at the last fix |
| `GET,POST /api/1.0/config` | bearer | read and write device settings |
| `POST /api/1.0/command` | bearer | queue a command |
| `POST /api/1.0/home` | bearer | stalled-tracker watchdog |

Endpoints take `?imei=` or `?device_id=`; the browser ones fall back to
`default_device` from the config.

## Tests

```sh
.venv/bin/pip install -r requirements-dev.txt
.venv/bin/pytest                      # parsing, crypto, OTA — no database
```

The integration tests need a throwaway database. They truncate every table
they touch, so do not point them at anything real:

```sh
mysql -u root -p -e "CREATE DATABASE tracker_test CHARACTER SET utf8mb4"
mysql -u root -p tracker_test < schema.sql
TRACKER_TEST_DB=root:pass@127.0.0.1:3306/tracker_test .venv/bin/pytest
```

The firmware tests drive the OTA HTTP server over a real socket rather than a
mock — what is being checked is byte-level framing, because the nRF91's
downloader is strict about `206` and `Content-Range` and a framing bug shows up
as a device that installs a corrupt image.

## Known gaps

- The DTLS Connection ID library is not in this repository. That listener
  disables itself when `dtls_lib` is unset; UDP and TLS are unaffected.
- Nothing here has been run against real hardware in this form. The tests
  exercise the protocol against synthetic records, which is not the same as a
  device in a car park on a marginal cell.
- Journey `from_place` and `to_place` are in the schema but nothing fills them
  in — a deployment wanting place names needs its own geocoder.
- `log` grows without bound and there is no retention policy. A device
  reporting once a minute writes roughly half a million rows a year.
- Bearer tokens have no scopes or expiry.

## Security notes

`config.yaml` holds every secret and is gitignored — keep it mode 600.
Certificates and private keys are gitignored too.

The app trusts `X-Forwarded-*` because passkeys bind to the origin. That is
only safe if the reverse proxy overwrites those headers and nothing else can
reach the backend port. Bind gunicorn to loopback.

If you find a security problem, please report it privately rather than opening
an issue — see the repository root.

## Licence

Apache-2.0, as the server-side component of
[l0destar](https://github.com/m4rkw/l0destar). See `LICENSE.md` at the
repository root.
