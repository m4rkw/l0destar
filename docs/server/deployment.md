# Server deployment

This page covers running the server from day to day: keeping it running, giving the web interface an HTTPS address, the firewall, publishing firmware from the build machine, logs, backups and upgrades. It assumes the installation in [Server installation](/server/installation.html).

## Keeping it running

`--restart unless-stopped` starts the container again if it exits, and Docker, which starts at boot, brings it back with the machine. A few seconds after a reboot:

```sh
sudo docker ps --format '{{.Names}}: {{.Status}}'
```

```text
l0destar: Up 38 seconds
```

| Command | What it does |
|---|---|
| `sudo docker restart l0destar` | Restarts the server, for example after changing `config.yaml`. |
| `sudo docker stop l0destar` | Stops it. It stays stopped, across reboots too, until you start it again. |
| `sudo docker start l0destar` | Starts it again. |
| `sudo docker logs -f l0destar` | Follows its output. |

Stopping the container stops the server first and then the database, so neither is cut off in the middle of a write.

### Workers, threads and listeners

gunicorn serves the web interface and API with `TRACKER_WORKERS` worker processes (2) of `TRACKER_THREADS` threads each (16). Every open map page holds a WebSocket for as long as it stays open, and each one occupies a thread, so raise `TRACKER_THREADS` if several people keep maps open.

The variables are options of the container, so change them by creating it again with `-e`. That loses nothing, because everything the server keeps is in `/srv/l0destar`:

```sh
sudo docker rm -f l0destar
sudo docker run -d --name l0destar --restart unless-stopped \
    -e L0DESTAR_HOSTNAME=tracker.example.com -e TRACKER_THREADS=32 \
    -v /srv/l0destar:/data \
    -p 65480:65480/udp -p 65481:65481/tcp -p 127.0.0.1:5000:5000 \
    m4rkw/l0destar
```

The telemetry listeners do not run in the workers. gunicorn loads the application once in its master process and the UDP and TLS listeners start there, so each port is bound exactly once however many workers you run.

## Web access

The web interface needs HTTPS: browsers refuse passkeys in an insecure context. The container publishes it on `127.0.0.1:5000` only, so something on the same machine has to serve it. Choose one of:

- **Tailscale** (recommended). The interface is only reachable from devices on your tailnet, with a certificate Tailscale provides. See [Server security](/server/security.html).
- **nginx with a public certificate**, if people must reach it from anywhere.

For the public option, `server/deploy/nginx.conf.example` in the repository is a complete virtual host. It redirects HTTP to HTTPS, overwrites the forwarded headers the application trusts, passes WebSocket upgrades through for the live map and sets a content security policy that allows the Google Maps script.

```sh
sudo apt install -y nginx certbot python3-certbot-nginx
sudo certbot certonly --nginx -d tracker.example.com
sudo curl -fsSL -o /etc/nginx/sites-available/tracker \
    https://raw.githubusercontent.com/m4rkw/l0destar/master/server/deploy/nginx.conf.example
sudoedit /etc/nginx/sites-available/tracker
sudo ln -s /etc/nginx/sites-available/tracker /etc/nginx/sites-enabled/tracker
sudo nginx -t && sudo systemctl reload nginx
```

When editing, change `server_name` and the certificate paths, and keep the `proxy_set_header` lines as they are. The application trusts `X-Forwarded-For`, `X-Forwarded-Host`, `X-Forwarded-Proto` and `X-Forwarded-Port`, because the passkey checks and the login rate limit depend on them. That is only safe while nginx overwrites them and nothing else can reach port 5000 - which is why the container publishes it on `127.0.0.1` alone.

## Firewall

| Port | Open to | Why |
|---|---|---|
| UDP 65480 | the internet | telemetry from trackers |
| TCP 65481 | the internet, at least while rolling out updates | firmware downloads |
| TCP 80 and 443 | the internet, public nginx only | web interface and certificate renewal |
| TCP 5000 | this machine only | the web application, published on `127.0.0.1` |

!!! warning "Docker opens published ports itself"
    Docker adds its own firewall rules for the ports a container publishes, ahead of the host firewall. A firewall on the server, such as ufw, can neither open nor close UDP 65480 and TCP 65481 while the container publishes them: with ufw denying every incoming connection, both still reach the server. Filter them at the router or at your provider's firewall instead ([Telemetry port](/server/telemetry-port.html)).

[Telemetry port](/server/telemetry-port.html) has port forwarding examples and a reachability test.

## Publishing firmware from the build machine

`push_fw.sh` uploads each image with `scp` and writes that device's manifest with `ssh`, straight into `/srv/l0destar/fw`, then checks the result through port 65481 the way a tracker would ([OTA updates](/board-setup/ota-updates.html)). The directory belongs to you, so your own ssh login is all it needs. Check from the build machine:

```sh
ssh you@tracker.example.com 'touch /srv/l0destar/fw/.write-test && rm /srv/l0destar/fw/.write-test'
```

On the build machine, `push_fw.sh` takes the ssh destination from `FW_SERVER` and the directory from `FW_DIR`:

```sh
# in firmware/
FW_SERVER=you@tracker.example.com FW_DIR=/srv/l0destar/fw ./push_fw.sh --list
```

The server picks up a new manifest on the next telemetry exchange; it does not need a restart.

## Logs

`sudo docker logs l0destar` shows the container's output: its start-up, MariaDB, gunicorn, and every line the UDP and TLS listeners log. The server also writes one file per channel into `/srv/l0destar/logs`, listed in the [server configuration reference](/reference/server.html#log-files).

The server never rotates those files and keeps each one open, so rotate them with logrotate's `copytruncate`. A minimal installation may not have logrotate yet: `sudo apt install -y logrotate`. Then save this as `/etc/logrotate.d/l0destar`:

```text
/srv/l0destar/logs/*.log {
    weekly
    rotate 12
    compress
    delaycompress
    missingok
    notifempty
    copytruncate
}
```

Without `copytruncate` the server would carry on writing into the rotated file. Docker also keeps the container's output; it grows slowly, and adding `--log-opt max-size=10m --log-opt max-file=3` to `docker run` caps it.

## The database

Open the database as its administrator:

```sh
sudo docker exec -it l0destar mariadb tracker
```

### Database growth

The `log` table gains a row for every record a tracker sends, and nothing ever removes them. A driving tracker sends about 0.6 records a second, so an hour behind the wheel adds roughly two thousand rows; a parked one adds a row per timed wake.

Journeys refer to ranges of `log` rows, so deleting old rows loses the replay for those journeys. If you want a retention limit, delete by id in batches, which uses the primary key rather than scanning the table:

```sql
-- the first row to keep
SELECT MIN(id) FROM log WHERE timestamp >= NOW() - INTERVAL 2 YEAR;
-- repeat until it affects no rows
DELETE FROM log WHERE id < <that id> LIMIT 10000;
```

## Backups

Back up the database and the files that go with it, and encrypt the copies: they contain every position your trackers have reported.

```sh
sudo docker exec l0destar mariadb-dump --single-transaction tracker | gzip > tracker-$(date +%F).sql.gz
```

Copying `/srv/l0destar/mysql` while the server runs does not make a usable backup; use the dump.

| What | Where | Why |
|---|---|---|
| the database dump | server | history, device keys, users and settings |
| `/srv/l0destar/config.yaml` | server | secrets and settings |
| `/srv/l0destar/certs/` | server | the CA trackers trust, and the update endpoint's certificate |
| `firmware/mcuboot_priv.pem` | build machine | without it no deployed tracker can ever be updated again |
| `firmware/remote.conf` | build machine | the fleet's build settings and device keys |
| `firmware/onboarding/` | build machine | the nRF Cloud device CA, if you use one |

To restore a dump into the running server:

```sh
gunzip -c tracker-2026-09-13.sql.gz | sudo docker exec -i l0destar mariadb tracker
```

Restore into a test installation now and then to be sure the backups work.

## Upgrading

```sh
sudo docker pull m4rkw/l0destar
sudo docker rm -f l0destar
sudo docker run -d --name l0destar --restart unless-stopped \
    -e L0DESTAR_HOSTNAME=tracker.example.com \
    -v /srv/l0destar:/data \
    -p 65480:65480/udp -p 65481:65481/tcp -p 127.0.0.1:5000:5000 \
    m4rkw/l0destar
```

The new container carries on with everything in `/srv/l0destar`. As it starts it applies any database migrations the new version brings, logging `l0destar: applying migration <file>` for each. Take a backup first, and look for new settings by comparing your `config.yaml` with the new image's example, `sudo docker exec l0destar cat config.yaml.example`.

## Scheduling the home check

`POST /api/1.0/home` checks every vehicle listed under `home_check` ([Server configuration](/server/configuration.html)). Run it at a time the vehicles are normally at home, from the server itself. Create a token for it:

```sh
sudo docker exec l0destar python tools/gentoken.py home-check
```

Then save this as `/etc/cron.d/l0destar-home-check`, with the token in place:

```text
0 3 * * * nobody curl -fsS -X POST -H "Authorization: Bearer <token>" http://127.0.0.1:5000/api/1.0/home > /dev/null
```

```sh
sudo chmod 600 /etc/cron.d/l0destar-home-check
```

A vehicle that is legitimately away that night raises a notification too, unless you mark it as garaged first with `garage=1` ([Device settings and commands](/reference/device-settings.html#commands)).
