# Test drive

A test drive shows whether the installed tracker reports reliably from where you have fitted it: a good GNSS fix, LTE-M coverage, no long gaps while driving, and the expected behaviour at key-off and while parked.

!!! warning "Drive first, look later"
    Do not watch the web interface while you drive. Have a passenger watch it, or review the drive afterwards - everything on this page works from stored data.

## Before you drive

1. With the vehicle parked and the ignition off, check that the tracker has reported recently: its last-seen time on the device list, or the latest rows in the `log` table.
2. Turn the ignition on without starting the engine. A record with the ignition on should arrive within a few seconds, and the map page should show "ignition on".
3. Start the engine. The page should change to "engine on" - from the engine speed on a K-wire build with engine data, otherwise from the charging voltage.
4. Note the time, so you can find the journey afterwards.

The queries on this page run against the tracker database. Open it on the server with:

```sh
sudo docker exec -it l0destar mariadb tracker
```

The latest rows for a device:

```sql
SELECT l.`timestamp`, l.ignition_state, l.battery_level,
       l.satellites, l.hdop, l.rat, l.fw
FROM `log` l
JOIN `device` d ON d.id = l.device_id
WHERE d.imei = '350000000000000'
ORDER BY l.id DESC
LIMIT 5;
```

## On the road

Drive a route that covers the places the vehicle normally goes, including any areas where you expect coverage to be weak.

What good looks like, from the author's own driving on LTE-M in the UK:

- new positions every 3 to 5 seconds while moving - records go out three to a datagram, so the page moves in steps;
- no gaps longer than a few tens of seconds;
- `rat` staying `CATM1`.

At the end, turn the ignition off. The tracker sends a final record - from its last fix if it cannot see the sky - the journey closes, and the tracker goes to sleep. If the vehicle is still rolling when the ignition goes off, the tracker keeps reporting until it has stopped.

## After the drive

Replay the journey first: open the map page, choose history and pick the drive. Look for straight lines cutting corners, which are gaps, and for positions away from the road.

Then check the numbers. These queries use window functions, so they need MySQL 8 or MariaDB 10.2 or later.

### Recent journeys

```sql
SELECT j.id, j.start_time, j.end_time, j.miles,
       COUNT(l.id) AS records,
       TIMESTAMPDIFF(SECOND, j.start_time, j.end_time) AS seconds,
       ROUND(COUNT(l.id) * 60 / NULLIF(TIMESTAMPDIFF(SECOND, j.start_time, j.end_time), 0), 1) AS records_per_minute
FROM `journey` j
JOIN `log` l ON l.device_id = j.device_id AND l.id BETWEEN j.start_log_id AND j.end_log_id
WHERE j.device_id = (SELECT id FROM `device` WHERE imei = '350000000000000')
  AND j.end_time IS NOT NULL
GROUP BY j.id
ORDER BY j.id DESC
LIMIT 10;
```

With the default batch of three, the firmware builds roughly 0.6 records a second while driving, so a drive without problems comes out at around 30 or more records a minute. A much lower figure means gaps.

### Gaps in a journey

Set `@journey` to the id of the drive from the query above:

```sql
SET @journey = 7;

SELECT prev_arrived AS gap_start, arrived AS gap_end,
       TIMESTAMPDIFF(SECOND, prev_arrived, arrived) AS arrival_gap_s,
       TIMESTAMPDIFF(SECOND, prev_device_time, device_time) AS device_clock_gap_s,
       combined_speed AS mph_after, rat, satellites, cell_location
FROM (
  SELECT l.`timestamp` AS arrived, l.gsm_timestamp AS device_time,
         l.combined_speed, l.rat, l.satellites, l.cell_location,
         LAG(l.`timestamp`) OVER (ORDER BY l.id) AS prev_arrived,
         LAG(l.gsm_timestamp) OVER (ORDER BY l.id) AS prev_device_time
  FROM `journey` j
  JOIN `log` l ON l.device_id = j.device_id AND l.id BETWEEN j.start_log_id AND j.end_log_id
  WHERE j.id = @journey
) t
WHERE prev_arrived IS NOT NULL
  AND TIMESTAMPDIFF(SECOND, prev_arrived, arrived) > 30
ORDER BY arrival_gap_s DESC;
```

- `arrival_gap_s` is the time between consecutive records reaching the server - the gap the live page saw.
- `device_clock_gap_s` is the same gap by the tracker's own clock. When it is much smaller than the arrival gap, the records were built on time but delivered late: the tracker holds on to records it could not send and sends them once the link is back. When both are large, no records were built at all - usually no fix or no network.
- Rows are in the order they arrived, so records delivered late can show a negative clock gap.
- The query only works for a closed journey; a journey still in progress has no end record yet.

Change the `30` to whatever you count as a gap.

### Fix quality and coverage

```sql
SELECT l.rat, COUNT(*) AS records,
       SUM(l.cell_location = 1) AS cell_positions,
       ROUND(AVG(l.satellites), 1) AS avg_sats,
       ROUND(MAX(l.hdop), 1) AS worst_hdop,
       MIN(CASE WHEN l.ignition_state = 1 THEN l.battery_level END) AS min_volts_ign_on,
       SUM(l.rst IS NOT NULL) AS resets
FROM `journey` j
JOIN `log` l ON l.device_id = j.device_id AND l.id BETWEEN j.start_log_id AND j.end_log_id
WHERE j.id = @journey
GROUP BY l.rat;
```

- `cell_positions` counts records built from the last known position instead of a fresh fix. A few at the ends of a drive are normal, particularly when parking under cover.
- A low `avg_sats` or a high `worst_hdop` points at the antenna's view of the sky.
- `rat` should only ever be `CATM1`, because the firmware only uses LTE-M.
- `resets` counts records reporting a restart. Look up the cause in `debug.log`.
- `min_volts_ign_on` shows how low the charging system let the voltage fall while driving. On many vehicles that is normal behaviour rather than a fault.

### Logs

On the server, in the log directory (`/srv/l0destar/logs`):

- `udp.log` has a line for each datagram received, such as `3 records from 350000000000000 (203.0.113.7)`. `decrypt failed` lines from the vehicle's address mean its key or IMEI does not match the server.
- `device.log` has the warnings and errors the firmware recorded, stamped with the time the tracker logged them. Look at what it logged around each gap: registration and send errors point at coverage or the antenna.
- `debug.log` has reset causes (`rst=`) and fault counters.

```sh
grep 350000000000000 /srv/l0destar/logs/device.log | tail -50
```

## While parked

1. After switching off, check that the ignition-off record arrived and the journey has an end time.
2. If you have set an engine-off interval, check that a record with the ignition off arrives every `int` seconds.
3. Once the tracker has gone to sleep, a minute or so after switching off, get in and close the door firmly or rock the vehicle on its suspension. A movement or parked impact alert should arrive - at priority 2 unless you have changed it, so set up notifications first (see [Configure alerts](/deployment/alerts.html)). A single light knock may be ignored as a transient bump.

Tilt alerts need the vehicle lifted by several degrees. Do not jack up a vehicle just to test the tracker unless you can do it safely.

## When there are dropouts

| What you see | Likely cause | What to try |
|---|---|---|
| Few satellites, high HDOP, wandering positions or many cell positions | The antenna cannot see enough sky | Move the antenna closer to the glass and away from metal; check the GNSS lead is on the GNSS port and the antenna is an active type |
| Gaps in the same places on every drive | Holes in LTE-M coverage | Nothing to fix in the tracker; records built during the gap are sent afterwards, thinned out if the outage was long |
| Gaps anywhere, with registration or send errors in `device.log` | Weak LTE at the antenna position | Move the antenna; check your network's LTE-M coverage |
| A gap of a few minutes with the engine off, followed by `fota:` alerts | An update was downloaded and installed | Expected: GNSS and telemetry pause while an update downloads, which waits until the engine is off |
| `decrypt failed` in `udp.log` from the vehicle's address | The tracker's key or IMEI does not match the server | See [Verifying telemetry](/board-setup/verifying-telemetry.html) |
| Restarts (`rst=`) during drives, especially when starting the engine | Power wiring: a poor ground, a loose crimp, a fuse holder | Re-check the harness, the crimps and the ground point |
| A journey split in two at a short stop | The stop was longer than `journey_resume_seconds` (300) | Raise `journey_resume_seconds` in `config.yaml` if you want longer stops joined up |
| The page flicking between "engine on" and "ignition on" | A charging system that lets the voltage fall while driving | Adjust `engine_running_voltage` and `engine_stopped_count` in `config.yaml`; engine data from the K wire settles it |
