# Server configuration reference

Everything that configures the l0destar server: `config.yaml`, the container and its data directory, environment variables, the per-device settings stored in the database, the command-line tools, log files, database tables and HTTP endpoints. [Server configuration](/server/configuration.md) explains how to choose the settings; this page lists them.

## `config.yaml`

Read once at start-up from `/srv/l0destar/config.yaml`, which is `/data/config.yaml` inside the container (`TRACKER_CONFIG`). The container's first start writes it with the keys the container needs. Restart the server after changing it, with `sudo docker restart l0destar`. Keys not listed here are ignored.

### Web

| Key | Default | Meaning |
|---|---|---|
| `session_secret` | required | Signs login sessions. Changing it logs everyone out. The first start generates one. |
| `session_lifetime_days` | `30` | How long a login lasts, in days. |
| `secure_cookies` | `true` | Only send the session cookie over HTTPS. Set `false` only for a local test over plain HTTP. |
| `google_maps_api_key` | empty | Maps JavaScript API key for the map page. Without it the page shows a note instead of the map. |
| `default_device` | unset | IMEI the web interface and read-only API use when a request does not name a device. |
| `rate_limit_request_count` | `5` | Login challenges allowed per client address in each period. |
| `rate_limit_reset_period` | `3600` | Length of that period, in seconds. |

### Storage

| Key | Default | Meaning |
|---|---|---|
| `database` | required | Mapping with `host` (default `127.0.0.1`), `port` (`3306`), `database` (required), `user` (required) and `password` (empty). The first start points it at the MariaDB inside the container, with a generated password that the container sets on the database account at every start. |
| `log_dir` | `logs/` in the server directory | Directory for the log files: `/data/logs` in the container. Created if missing. |
| `fw_dir` | `fw/` in the server directory | Published firmware images and manifests: `/data/fw` in the container. |

### UDP listener

| Key | Default | Meaning |
|---|---|---|
| `udp_enabled` | `true` | Start the UDP listener. |
| `udp_host` | `0.0.0.0` | Address to bind. |
| `udp_port` | `65480` | Port to bind. Compiled into the firmware, so keep 65480, and publish it as 65480. |

### TLS listener

The TLS listener serves firmware downloads and nothing else.

| Key | Default | Meaning |
|---|---|---|
| `tls_cert` | empty | Server certificate: `/data/certs/server.crt` in the container. The listener only starts when this and `tls_key` are both set. Relative paths are relative to the server's working directory. |
| `tls_key` | empty | Server private key: `/data/certs/server.key` in the container. |
| `tls_host` | `0.0.0.0` | Address to bind. |
| `tls_port` | `65481` | Port to bind. Also the firmware update port compiled into the firmware. |
| `tls_backlog` | `8` | Listen backlog for incoming connections. |
| `tls_handshake_timeout` | `45` | Seconds allowed for the TLS handshake. |
| `tls_read_timeout` | `10` | Seconds to wait for a request once the handshake is done. |
| `fw_download_timeout` | `120` | Read timeout, in seconds, while serving a firmware download. |

### Behaviour

| Key | Default | Meaning |
|---|---|---|
| `engine_running_voltage` | `13.0` | Battery voltage at or above which the web interface assumes a charging alternator. |
| `engine_stopped_count` | `10` | How many recent records the web interface checks for that voltage before showing the engine as stopped. |
| `journeys` | `true` | Build journeys from ignition changes. |
| `journey_resume_seconds` | `300` | If the ignition comes back on within this many seconds, the previous journey continues. |

### Notifications

| Key | Default | Meaning |
|---|---|---|
| `notify.backend` | `none` | `none`, `pushover` or `webhook`. |
| `notify.user` | - | Pushover user key. |
| `notify.app` | - | Pushover application token. |
| `notify.retry` | `30` | Pushover: seconds between repeats of a priority 2 alert until it is acknowledged. |
| `notify.expire` | `300` | Pushover: how many seconds a priority 2 alert keeps repeating. |
| `notify.url` | - | Webhook: the URL a JSON body is posted to. |
| `notify.token` | unset | Webhook: sent as `Authorization: Bearer <token>`. |

How each backend behaves is described in [Configure alerts](/deployment/alerts.md).

### Home check

`home_check` is a list of vehicles for `POST /api/1.0/home` to check. A single mapping is also accepted. An entry without `imei`, `latitude` or `longitude` stops the server at start-up.

| Key | Default | Meaning |
|---|---|---|
| `imei` | required | Device to check. |
| `latitude`, `longitude` | required | Where the vehicle is normally parked. |
| `radius_m` | `300` | How far, in metres, its last position may be from that point. |

### Command-line tools

| Key | Default | Meaning |
|---|---|---|
| `api_base` | `http://127.0.0.1:5000` | Where `tools/command.py` sends its API requests. |

## The container

`m4rkw/l0destar` runs the server and its MariaDB database together; [Server installation](/server/installation.md) has the `docker run` command.

| Option | Meaning |
|---|---|
| `-v <directory>:/data` | The data directory. Required: the container will not start without one. |
| `-e L0DESTAR_HOSTNAME=<hostname>` | The hostname trackers reach the server at. Needed on the first start, to issue the server certificate. |
| `-p 65480:65480/udp` | Telemetry. |
| `-p 65481:65481/tcp` | Firmware downloads. |
| `-p 127.0.0.1:5000:5000` | The web interface and API. Publish it on loopback only. |
| `--restart unless-stopped` | Start again after a crash or a reboot. |

### Data directory

| Path | Contents |
|---|---|
| `config.yaml` | The configuration, written on the first start. Mode 600. |
| `certs/ca.key`, `certs/ca.crt` | The CA trackers trust, created on the first start unless `certs/` already holds certificates. |
| `certs/ca_cert.h` | The CA certificate as a C string. Not needed by the firmware build, which makes `src/ca_cert.h` from `ca.crt` itself. |
| `certs/server.crt`, `certs/server.key` | The TLS listener's certificate and key, issued for `L0DESTAR_HOSTNAME`. |
| `mysql/` | The database. Back it up with `mariadb-dump`, not by copying these files. |
| `fw/` | Firmware images and manifests, published from the build machine. |
| `logs/` | The log files. |

The server and the database run as the owner of the directory, or as uid 10001 if that is root. At every start the container gives `mysql/`, `certs/`, `logs/` and `config.yaml` to that account, sets the database account's password from `config.yaml`, and applies any migrations the database has not had yet.

## Environment variables

Set them with `-e` on `docker run`.

| Variable | Default | Used by | Meaning |
|---|---|---|---|
| `L0DESTAR_HOSTNAME` | unset | the container's first start | Hostname the server certificate is issued for. Only needed while `certs/` is empty. |
| `TRACKER_CONFIG` | `/data/config.yaml` in the container, otherwise `config.yaml` in the server directory | server and tools | Path to the configuration file. |
| `TRACKER_BIND` | `0.0.0.0:5000` in the container, otherwise `127.0.0.1:5000` | gunicorn | Address of the web application. In the container it listens on every address inside the container, and the `-p` option decides who can reach it. |
| `TRACKER_WORKERS` | `2` | gunicorn | Worker processes. |
| `TRACKER_THREADS` | `16` | gunicorn | Threads per worker. Each open map page holds one. |
| `TRACKER_TEST_DB` | unset | tests | `user:password@host:port/database` of a throwaway database for the integration tests, which empty every table they use. |

`GUNICORN_MASTER_PID` is set by `gunicorn.conf.py` itself so that the listeners start in the master process only. Do not set it.

## Per-device settings

Each device's settings live in its `device` row. The first three are sent to the tracker in every reply; the rest are acted on by the server and never reach the tracker.

| Column | Default | Set with | Meaning |
|---|---|---|---|
| `int` | `0` | `int=<seconds>` command, or `int` in `POST /api/1.0/config` | Engine-off timed wake interval in seconds; 0 means no timed wakes. |
| `movement_alarm` | `1` | `movealarm=0` or `movealarm=1` command, or `ma` | Movement alarm flag, reported back by the tracker. Current firmware does not act on it. |
| `track_mode` | `0` | the web interface, or `POST /api/1.0/trackmode` | Track mode switch. Cleared at ignition off, or when the device is next heard from after more than an hour of silence. |
| `alarm` | `0` | `alarm=0` or `alarm=1`, or `al` | Notify when the ignition comes on. |
| `garage` | `0` | `garage=0` or `garage=1`, or `ga` | The vehicle is expected to move: priority 2 alerts are sent at priority 0 and the home check does not alert for it. |
| `overnight_alarm` | `0` | `overnightalarm=0` or `overnightalarm=1`, or `oa` | Notify when the ignition comes on inside the overnight window. |
| `overnight_alarm_hour_from` | `23` | `overnight_alarm_hour_from=<hour>`, or `oaf` | Start of the window, as an hour in the server's local time. |
| `overnight_alarm_hour_to` | `6` | `overnight_alarm_hour_to=<hour>`, or `oat` | End of the window. |

An `int=` or `movealarm=` command is queued for the tracker, which applies it and reports the new value; `POST /api/1.0/config` changes the column directly and the next reply carries it. Either way the `int` and `movement_alarm` columns follow what the tracker reports. The server-side settings take effect immediately. What each setting does on the tracker, and every command, is in [Device settings and commands](/reference/device-settings.md#commands).

The other columns identify the device (`imei`, `name`, `registration`), hold its key (`psk`), and track firmware updates (`fw_staged`, `fw_staged_at`, `fw_blocked`, `fw_fail_count`); `command.py <imei> fota-retry` clears the update state.

## Tools

The tools run inside the container, where they read `config.yaml`:

```sh
sudo docker exec l0destar python tools/<tool>.py ...
```

### `adddevice.py`

```text
adddevice.py <imei> <name> [registration] [--psk <64 hex characters>]
```

Enrols a device. With `--psk` it stores the key the tracker's firmware was already built with; without it, it generates a key and prints it once, to build into the firmware. The IMEI must be 14 to 16 digits ([Onboarding devices into the server](/board-setup/server-onboarding.md)).

### `device.py`

```text
device.py list
device.py show <imei>
device.py rename <imei> <name> [registration]
device.py rekey <imei> [--psk <64 hex characters>]
device.py remove <imei> --yes
```

| Command | What it does |
|---|---|
| `list` | Lists every enrolled device. |
| `show` | Shows one device's settings, update state, last record and queued commands. |
| `rename` | Changes the name used in alerts and the web interface, and optionally the registration. |
| `rekey` | Replaces the device's key with a new random one, or with the one given, and prints it once. The tracker then needs firmware built with the new key. |
| `remove` | Deletes the device and all of its history. |

### `command.py`

```text
command.py <imei> <command> [command ...]
command.py <imei> fota-retry
```

Queues a command for the tracker's next reply, through the HTTP API at `api_base`, using the first token in `api_token` (create one with `gentoken.py` first). Several commands given together are sent together. The server-side settings - `alarm=`, `garage=`, `overnightalarm=`, `overnight_alarm_hour_from=` and `overnight_alarm_hour_to=` - are applied straight away instead of being queued. `fota-retry` clears a firmware version withheld from the device after a failed update and queues `fota`. The commands the firmware understands are listed in [Device settings and commands](/reference/device-settings.md#commands).

### `gentoken.py`

```text
gentoken.py <name>
```

Creates a bearer token for the automation API and prints it. Tokens have no scope or expiry; revoke one by deleting its row from `api_token`.

### `regtoken.py`

```text
regtoken.py <username> <hostname>
```

Prints a single-use enrolment link, `https://<hostname>/register?...`, valid for 24 hours. Creating a link cancels any earlier one for the same username. For an existing username, the passkey registered from the new link replaces the old one.

### `import_plmn.py`

```text
import_plmn.py <file.csv>
```

Loads mobile network operator names, so the web interface can show which network a tracker is on. The CSV needs a header row with `mcc`, `mnc` and `operator` columns, and optionally `country`. Existing entries are updated. No list is bundled. The file has to be inside the container, so pipe it in: `sudo docker exec -i l0destar python tools/import_plmn.py /dev/stdin < plmn.csv`.

## Log files

All in `/srv/l0destar/logs` (`log_dir`):

| File | Contents |
|---|---|
| `app.log` | Application errors, and every notification the server sends, whatever the backend. |
| `udp.log` | The UDP listener: records received per exchange, commands delivered, alerts relayed, update reports, decryption failures. Also in the container's output. |
| `tls.log` | The TLS listener: failed handshakes, firmware requests and update confirmations. Also in the container's output. |
| `debug.log` | Records that carry debug counters or a reset cause, one line each: a short list of incidents. |
| `device.log` | Warnings and errors the firmware captured between sends, stamped with the time the tracker logged them. |
| `audit.log` | Enrolment, login and logout attempts in the web interface, with client address and username. |

`sudo docker logs l0destar` shows the container's output: the start-up messages, MariaDB, gunicorn, and the `udp.log` and `tls.log` lines.

## Database tables

| Table | Contents |
|---|---|
| `device` | One row per tracker: IMEI, name, registration, key, settings and update state. |
| `log` | Every telemetry record. Grows without limit. |
| `journey` | Each ignition-on to ignition-off period, with the range of `log` rows it covers and its distance. |
| `dtc` | Fault codes reported over the K wire, with when each was raised and cleared. |
| `command` | Commands waiting for a tracker's next reply. Deleted as they are delivered. |
| `device_nonce` | Every nonce each tracker has used in the last 30 days, for replay protection. Prunes itself. |
| `plmn` | Mobile network operator names. Optional. |
| `user` | Web interface users and their passkey public keys. |
| `registration` | Enrolment links not yet used. |
| `regoptions`, `authoptions`, `authoptions_ip` | Passkey challenges in progress and the login rate limit. Safe to empty. |
| `api_token` | Bearer tokens for the automation API. |
| `schema_migration` | The files from `migrations/` the database has had. Kept by the container's start-up; the server itself does not use it. |

Deleting a `device` row deletes its `log`, `journey`, `dtc`, `command` and `device_nonce` rows with it. The `last_nonce` column is no longer used.

`schema.sql` creates the current schema. A database created from an older version needs the files in `migrations/` that are newer than it, oldest first. The container applies them itself at start-up, logging `l0destar: applying migration <file>` for each:

| Migration | Adds |
|---|---|
| `2026-09-06_track_mode.sql` | the track mode switch, and the track mode columns of `log` |
| `2026-09-12_fota_state.sql` | per-device update state, so a failing image is not downloaded over and over |
| `2026-09-14_device_nonce.sql` | replay protection that survives a restart |

## HTTP endpoints

The web application listens on `TRACKER_BIND`, which the container publishes on `127.0.0.1:5000`. Examples and response formats are in [Web interface and API](/board-setup/web-interface.md).

### Pages

| Path | Purpose |
|---|---|
| `/` | Redirects to the login page, the map or the device list. |
| `/login`, `/logout` | Passkey login and logout. |
| `/register` | Enrolment page opened from a `regtoken.py` link. |
| `/devices` | Every enrolled device. |
| `/track` | Map and live data for one device (`?imei=`). |

The login and enrolment pages call `/regoptions`, `/authoptions` and `/authenticate` themselves.

### Session API

For a logged-in browser. Read endpoints choose the device from `imei` or `device_id` in the query string or JSON body, then the `X-Imei` header, then `default_device`, then the only enrolled device. `POST` endpoints need the device named explicitly, and fail with `imei or device_id required` otherwise. A named device that is not enrolled fails with `device not found`, never falling back to another vehicle.

| Method and path | Purpose |
|---|---|
| `GET /api/1.0/devices` | Every device with its last position, battery, ignition, firmware and network. |
| `GET /api/1.0/carpos` | The latest position and live values for a device. |
| `GET /api/1.0/status` | A device's settings, firmware version and last contact. |
| `GET /api/1.0/journeys` | Completed journeys, newest first (`page`, and `per_page` up to 200). |
| `GET /api/1.0/journey/<id>/points` | Every record of one journey. |
| `GET` or `POST /api/1.0/trackmode` | Read or set the track mode switch (`{"on": 1}`). |
| `GET /ws/carpos` | WebSocket stream of new records for a device. |

### Automation API

Takes `Authorization: Bearer <token>` with a token from `gentoken.py`.

| Method and path | Purpose |
|---|---|
| `GET /api/1.0/track` | Redirects to a map at a device's last position (`google=1` for Google Maps, `return=1` for JSON instead of a redirect). |
| `GET /api/1.0/config` | A device's settings as `int`, `ma`, `al`, `ga`, `oa`, `oaf` and `oat`. |
| `POST /api/1.0/config` | Change any of those settings. |
| `POST /api/1.0/command` | Queue a command, for example `{"imei": "350000000000000", "command": "locate"}`. |
| `POST /api/1.0/home` | Run the home check. |

### Firmware endpoint

Served by the TLS listener on port 65481, not by the web application.

| Path | Purpose |
|---|---|
| `/fw/manifest.txt?imei=<imei>&v=<version>` | The manifest published for that device, or `status=blocked` for a version withheld from it. A request whose `v=` is the version staged for that device marks the update as installed; nothing authenticates it. |
| `/fw/<file>` | A firmware image, with range requests. |
| `/fw/published.txt` | Every version ever published; `push_fw.sh` uses it to number the next release. |
