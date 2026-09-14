# Server configuration

The server reads `/srv/l0destar/config.yaml` once at start-up. The first start writes it with what the container needs; add the settings on this page to it, then restart the server:

```sh
sudo docker restart l0destar
```

This page goes through the decisions you need to make. Every key and its default is listed in the [server configuration reference](/reference/server.html#configyaml), and the image carries a fully commented example:

```sh
sudo docker exec l0destar cat config.yaml.example
```

The file holds the session secret, the database password and any notification credentials. It is created with mode 600, owned by the owner of `/srv/l0destar`; keep it that way.

## What the first start writes

```yaml
session_secret: <generated>
google_maps_api_key: ''
notify:
  backend: none
database:
  host: 127.0.0.1
  port: 3306
  database: tracker
  user: tracker
  password: <generated>
log_dir: /data/logs
fw_dir: /data/fw
tls_cert: /data/certs/server.crt
tls_key: /data/certs/server.key
```

- `database` is the MariaDB inside the container. The password is set on the database account at every start, so to change it, edit it here and restart.
- `log_dir`, `fw_dir`, `tls_cert` and `tls_key` are paths inside the container, where `/data` is `/srv/l0destar`. Leave them as they are.

## Web interface

```yaml
session_secret: <random string>
session_lifetime_days: 30
secure_cookies: true
google_maps_api_key: '<key>'
```

- `session_secret` signs login sessions. Changing it logs everyone out, which is also how to end every session after a suspected compromise. Generate a new one with `openssl rand -base64 48`.
- `session_lifetime_days` is how long a login lasts.
- `secure_cookies` must stay `true` for anything but a local test over plain HTTP. Passkeys need HTTPS anyway.
- `google_maps_api_key` is needed for the map. Without it the map page shows a note instead and everything else still works. Create a key for the Maps JavaScript API in the Google Cloud console and restrict it to your site's address with an HTTP referrer restriction such as `https://tracker.example.com/*`: the key is sent to every browser that opens the map and cannot be kept secret.

The login rate limit, `rate_limit_request_count` and `rate_limit_reset_period`, rarely needs changing; see [Server security](/server/security.html).

## Which vehicle the web interface opens on

With one tracker enrolled, the web interface and the read-only API use it automatically. With several, logging in lands on the device list and requests name the device they are about. `default_device` picks a vehicle to open on instead:

```yaml
default_device: '350000000000000'
```

Leave it unset unless you want a particular vehicle to open by default. Requests that change something - settings, commands, the track mode switch - always have to name their device, whatever this is set to.

## Listeners

The server listens for trackers on two ports:

| Listener | Port | Carries |
|---|---|---|
| UDP with ChaCha20-Poly1305 | 65480/udp | all telemetry, alerts and replies |
| TLS | 65481/tcp | firmware update downloads, and nothing else |

Both are on without any settings: the TLS listener starts once `tls_cert` and `tls_key` are set, which the first start does. Keep both ports at their defaults, in `config.yaml` and in the `-p` options of `docker run`, because both numbers are compiled into the firmware.

The TLS timeouts (`tls_read_timeout`, `tls_handshake_timeout` and `fw_download_timeout`) are tuned for LTE-M in weak signal and are best left alone. Opening the ports is covered in [Telemetry port](/server/telemetry-port.html).

## Engine state and journeys

```yaml
engine_running_voltage: 13.0
engine_stopped_count: 10
journeys: true
journey_resume_seconds: 300
```

The ignition input cannot tell "key on" from "engine running". When a tracker reports engine RPM from the ECU (a K-wire build) the web interface uses that. Otherwise it shows the engine as running if any of the last `engine_stopped_count` records had the battery above `engine_running_voltage`, because only a charging alternator lifts the rail that high. These two keys only change what the web interface shows; the tracker makes its own decision using its firmware thresholds.

A journey opens on the first record with the ignition on and closes when the ignition goes off. If the ignition comes back on within `journey_resume_seconds`, the journey that just closed is reopened rather than a new one started, so a fuel stop does not split a trip in two.

## Notifications

```yaml
notify:
  backend: pushover
  user: <Pushover user key>
  app: <Pushover application token>
  retry: 30
  expire: 300
```

`backend` is `none` (the default: alerts are only logged), `pushover`, or `webhook` with a `url` and an optional `token` sent as a bearer token. Which alerts exist, and how to set up each backend, is covered in [Configure alerts](/deployment/alerts.html).

## Home check

A tracker that stops reporting while parked at home is easy to miss: its last position still looks like a car at home. The home check catches the other case, where the last position is somewhere the vehicle should not be. List each vehicle to check:

```yaml
home_check:
  - imei: '350000000000000'
    latitude: 51.5
    longitude: -0.12
    radius_m: 300
  - imei: '350000000000001'
    latitude: 51.5
    longitude: -0.12
    radius_m: 300
```

Nothing happens until something calls `POST /api/1.0/home`, usually a cron job at a time the vehicles are normally at home ([Server deployment](/server/deployment.html)). Each listed vehicle whose last position is further than `radius_m` metres from its point, and which is not marked as garaged, raises a notification. A single mapping, without the list, is also accepted. An entry missing its `imei`, `latitude` or `longitude` stops the server at start-up with an error naming the entry, rather than leaving that vehicle silently unchecked.

## Command-line tools

```yaml
api_base: http://127.0.0.1:5000
```

`tools/command.py` queues commands through the server's own HTTP API at `api_base`. The default is the web application inside the container, which is where the tools run, so leave it unset.
