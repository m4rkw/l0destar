# Web interface and API

The server's web interface shows every enrolled vehicle, its live position, its journeys, and the engine and motion data it reports. The same server offers a small HTTP API for scripts and home automation.

Examples use `https://tracker.example.com`; use whatever hostname you serve the interface on (see [server security](/server/security.html) for serving it privately over Tailscale).

## Signing in

The server uses passkeys only - there are no passwords.

1. On the server, create a sign-up link for the person:

```sh
sudo docker exec l0destar python tools/regtoken.py alice tracker.example.com
```

It prints a link like `https://tracker.example.com/register?username=alice&token=...`, valid once, for 24 hours. Creating another link for the same username cancels this one.

2. Open the link **on the device that will hold the passkey** and press **register**. Registration asks for the device's own authenticator, with user verification - iCloud Keychain, Google Password Manager, Windows Hello and the like - so browsers do not offer roaming security keys.
3. When it says `passkey registered successfully`, follow **continue to login**, enter the username and approve the passkey prompt.

Things to know:

- **Hostname.** A passkey belongs to the hostname it was created on. Always open the interface on the same hostname that was in the sign-up link: a Tailscale name and a public name are different sites as far as passkeys are concerned.
- **Lost phone.** Create a new link for the same username. Registering replaces the old passkey and signs the lost phone out.
- **Sessions** last 30 days (`session_lifetime_days`). **logout** ends one.
- **Lockouts.** Five failed passkey verifications lock the account. Separately, after five sign-in attempts from one address without a successful login, further attempts get `too many requests` until an hour after the last one. [Server security](/server/security.html) covers unlocking an account.

## Choosing a vehicle

Signing in lands on the map for your default vehicle - `default_device` in the server's configuration, or the only enrolled device if there is just one. Otherwise it lands on the device list at `/devices`.

The device list shows every enrolled device by name, with its registration, IMEI, when it was last heard from, ignition or engine state, battery voltage, speed, firmware version and network. Choose one to open its map at `/track?imei=<imei>`. With no devices enrolled it says so and points at `tools/adddevice.py`.

## The map page

The top of the page shows:

- The vehicle's **registration**, or its name if it has none.
- Links: **gps** opens the last position in Google Maps; **history**, **track** and **logout**; and **devices**, back to the device list, when more than one device is enrolled.
- The mobile **operator** and radio technology (`CATM1` for LTE-M), and the **battery voltage**. The voltage turns red when the vehicle is moving with the ignition on but no charging is detected - worth checking the alternator.
- The **date and time** the latest record reached the server.
- **Speed** in mph: the ECU's own figure when the device reports one, GNSS otherwise.
- **State**: `engine on`, `ignition on` or `ignition off`. The engine counts as running when the ECU reports RPM above zero, or, without ECU data, when any of the last `engine_stopped_count` (10) battery readings was at or above `engine_running_voltage` (13.0V) - a charging alternator holds the voltage up, and the window stops smart charging systems flickering the display.

The page follows the vehicle live over a WebSocket and reconnects by itself if the connection drops. It needs a Google Maps key (`google_maps_api_key`); without one, a note takes the place of the map and everything else still works.

### ECU panel

Shown only when records carry OBD-II data, which means a K-wire build with OBD telemetry enabled ([deployment configuration](/deployment/configuration.html)). It shows RPM, coolant and intake temperature, engine load, throttle, mass air flow, timing advance, short and long-term fuel trims and fuel system status. Coolant at 110°C or more, fuel trims beyond ±10% and a fault in the fuel system status are highlighted, and a line appears when the check engine light is on, with the number of stored fault codes. A value the ECU did not report in that record shows as a dash.

### IMU panel

Built from the accelerometer and gyroscope. Because the tracker can be mounted in any orientation, everything is measured against gravity as the device sees it while stationary:

- **g force** - horizontal acceleration from braking, accelerating and cornering, coloured by whether the vehicle is speeding up or slowing down.
- **bump** - vertical acceleration, from potholes and speed humps.
- **yaw** - turning rate in degrees per second.
- **tilt** - the angle away from the resting position, shown only while stationary, where it means jacking or towing.
- **imu temp** - the sensor's temperature.

### History and replay

**history** lists completed journeys, newest first, with their start time and distance; **Load more** fetches older ones. Place names show as `?` because nothing in the server fills them in.

Choose a journey to replay it: the route is drawn on the map and the vehicle moves along it, with the panels showing what was recorded at each point. The controls stop the replay and return to the live view, skip back or forward ten points, pause, and scrub with the slider.

### Track mode

Track mode is for track days and bench runs. **track** asks for confirmation and then switches it on for that vehicle: the tracker stops GNSS, polls the ECU continuously and sends a record about once a second with a burst of motion samples, and the map is replaced by a dashboard - RPM with a redline bar, speed, throttle and load, a friction circle and a 60 second trace of RPM, throttle and g, and tiles for the slower values, battery, data rate and the age of the latest record.

- The device picks up the switch the next time it reads a reply - within about 30 seconds with the ignition on - and the dashboard shows "waiting for the device to enter track mode" until then.
- It only works with the ignition on, and most of the dashboard needs a K-wire build with OBD telemetry.
- It ends by itself when the ignition goes off. If the device lost power or coverage before it could report that, the server clears the switch when it next hears from the device after more than an hour of silence. Either way, switch it on again for each session.
- The dashboard learns which way is forward from the first firm braking or acceleration (it shows "orienting… brake or accelerate" until then) and remembers it in your browser for that vehicle.

!!! danger "Drive safely"
    Track mode is for closed circuits, private land and the bench. On a public road obey the law and keep your eyes on the road, not on the dashboard - its figures are for looking at afterwards. You alone are responsible for how you drive.

## HTTP API

All responses are JSON with `"status": "ok"`, or `"status": "error"` and a `"message"`.

### Which device a request is about

- Requests that only read data use the device named by `imei` or `device_id` (in the query string or the JSON body), or by the `X-Imei` header. When they name none they use `default_device`, then the only enrolled device.
- Requests that change something - `POST /api/1.0/config`, `POST /api/1.0/command` and `POST /api/1.0/trackmode` - must name the device with `imei`, `device_id` or `X-Imei`, and fail with `imei or device_id required` otherwise.
- A request that names a device that is not enrolled fails with `device not found`. It never falls back to another vehicle.

### Browser endpoints

These use the signed-in session, and are what the web page itself calls. Without a session they redirect to the sign-in page.

| Endpoint | Returns |
|---|---|
| `GET /api/1.0/devices` | Every device: `id`, `imei`, `name`, `registration`, `last_seen`, `latitude`, `longitude`, `speed`, `battery_level`, `ignition_state`, `fw`, `rat`, `operator`, `track_mode`. |
| `GET /api/1.0/carpos` | The latest `position`, the resting accelerometer reading (`accel_baseline`) and the `track_mode` switch. |
| `GET /api/1.0/status` | The device's IMEI, name, synced settings (`int`, `ma`), firmware version and last-seen time. |
| `GET /api/1.0/journeys` | Completed journeys, newest first: `page` (from 0) and `per_page` (default 50, at most 200). |
| `GET /api/1.0/journey/<id>/points` | Every point of one journey. With `imei`, an error if the journey belongs to another device. |
| `GET /api/1.0/trackmode`, `POST /api/1.0/trackmode` | Read, or set with `{"imei": "...", "on": 1}`, the track mode switch. |
| `GET /ws/carpos?imei=<imei>` | The live WebSocket stream (below). |

### Automation endpoints

These take a bearer token instead of a session. Create one per consumer on the server:

```sh
sudo docker exec l0destar python tools/gentoken.py home-automation
```

A token is not limited to particular devices or actions: anything holding one can change settings and queue commands such as `reboot`. Treat it like shell access, and see [server security](/server/security.html) for revoking one.

| Endpoint | What it does |
|---|---|
| `GET /api/1.0/track` | Redirects to a map at the device's last position: an Apple Maps link by default, Google Maps with `google=1`. With `return=1` it returns `{"url": ...}` instead of redirecting. |
| `GET /api/1.0/config` | The device's settings: `int`, `ma`, `al` (alarm), `ga` (garage), `oa` (overnight alarm), `oaf` and `oat` (overnight window hours). |
| `POST /api/1.0/config` | Sets any of those fields. Values must be integers. |
| `POST /api/1.0/command` | Queues a command for the device, or applies a server-side setting. |
| `POST /api/1.0/home` | The home check (below). |

```sh
TOKEN=<token from gentoken.py>

# read a device's settings
curl -s -H "Authorization: Bearer $TOKEN" \
  "https://tracker.example.com/api/1.0/config?imei=350000000000000"

# report every hour while parked
curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"imei": "350000000000000", "int": 3600}' \
  https://tracker.example.com/api/1.0/config

# ask for the current position as an alert
curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"imei": "350000000000000", "command": "locatenow"}' \
  https://tracker.example.com/api/1.0/command

# a Google Maps link to the last position
curl -s -H "Authorization: Bearer $TOKEN" \
  "https://tracker.example.com/api/1.0/track?imei=350000000000000&google=1&return=1"
```

### Commands

A command waits on the server until the device next reads a reply, and is handed over once: if that reply is lost, queue it again. Queuing a command replaces a pending one it supersedes (`locate` and `locatenow`, for instance), and several can be sent at once separated by commas. Server-side settings in the same request - `alarm=`, `garage=`, `overnightalarm=` and the overnight hours - take effect immediately and never go to the device. The full list is in [device settings and commands](/reference/device-settings.html#commands); from the server's shell, `tools/command.py` does the same job:

```sh
sudo docker exec l0destar python tools/command.py 350000000000000 locate
```

### Live stream

`/ws/carpos?imei=<imei>` is the WebSocket the map page uses. It needs a signed-in session, so it is for browsers rather than scripts. On connecting it sends the latest record, then every new record as it arrives, each as the `position` object that `carpos` returns - coordinates, speeds, heading, timestamp, battery, ignition, operator, the OBD and IMU readings, and in track mode the unpacked motion burst - but not `carpos`'s `accel_baseline`. When nothing new has arrived for 10 seconds it sends `{"ping": true, "track_mode": 0}`, carrying the track mode switch, so the connection is not dropped.

### Home check

A tracker that stops reporting while parked at home is hard to notice: its last record looks exactly like a car parked at home. The home check catches the opposite case - the last known position is not where the vehicle should be.

List each vehicle's home in `home_check` in the server's configuration (see [server configuration](/server/configuration.html)), then call the endpoint on a schedule from the server itself, at a time the vehicles should be home - [server deployment](/server/deployment.html#scheduling-the-home-check) has the details:

```text
# /etc/cron.d/l0destar-home-check: every night at 03:00
0 3 * * * nobody curl -fsS -X POST -H "Authorization: Bearer <token>" http://127.0.0.1:5000/api/1.0/home > /dev/null
```

It checks every configured vehicle, or only the one `?imei=` names (an IMEI with no `home_check` entry fails with `no home_check entry for <imei>`), and returns a `devices` list with each vehicle's `imei`, `name`, `at_home`, `distance_m` and `garage` flag. A vehicle that cannot be checked carries an `error` instead - `device not found` or `no position recorded`. A vehicle away from home that is not in garage mode raises `<name>: tracker may be stalled - vehicle is <n>m from home`.

## Google Maps key

The map uses the Google Maps JavaScript API. Create a key in the Google Cloud console with that API enabled, restrict it to your interface's address with an HTTP referrer restriction - the key is sent to every browser that loads the page and cannot be kept secret - and set it as `google_maps_api_key` in `config.yaml`. The [server configuration](/server/configuration.html) page covers the setting.
