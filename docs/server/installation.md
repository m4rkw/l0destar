# Server installation

The l0destar server receives telemetry from your trackers, stores it in a database, sends notifications, serves over-the-air firmware updates and runs the map. It is self-hosted: there is no service to sign up to, and the location history lives only on the machine you install it on.

This page installs it with Docker. One image, `m4rkw/l0destar`, holds the server and the MariaDB database it uses, and everything the server keeps is stored in a directory on the host. [Server configuration](/server/configuration.html) explains the settings and [Server deployment](/server/deployment.html) covers HTTPS, backups and upgrades. Read [Server security](/server/security.html) before exposing anything to a network.

!!! note "Status"
    The server in the l0destar repository is a public, de-personalised version of the author's private deployment. Its automated tests pass and these steps have been followed on a fresh Ubuntu 26.04 machine, but it has not been run end to end against real hardware in this form. Read the code before you trust it with a vehicle.

## What you need

- A Linux machine that stays on, amd64 or arm64. These steps were tested on Ubuntu 26.04; anything that runs Docker will do. A tracker only reports when it wakes, and anything it sends while the server is down is lost.
- A public **IPv4** address the trackers can reach on UDP 65480 and TCP 65481, directly or through port forwarding, and a DNS name with an A record pointing at it. The firmware cannot use IPv6; see [Telemetry port](/server/telemetry-port.html).
- An HTTPS address for the web interface, because passkeys only work in a secure context: Tailscale ([Server security](/server/security.html), recommended) or nginx with a public certificate ([Server deployment](/server/deployment.html)).

## Install Docker

On Ubuntu or Debian:

```sh
sudo apt update
sudo apt install -y docker.io
```

The package starts Docker straight away and at every boot. On other distributions, follow [Docker's installation instructions](https://docs.docker.com/engine/install/).

The commands on these pages run `docker` with `sudo`. Adding your account to the `docker` group avoids that, but gives the account the equivalent of root.

## Create the data directory

```sh
sudo install -d -o "$USER" -g "$USER" /srv/l0destar
```

Everything the server keeps lives in this directory: its configuration, certificates, database, firmware and logs. Nothing is kept only inside the container, so the container can be deleted and created again - to upgrade it, for example - without losing anything.

The server runs as the owner of the directory, so its files belong to you: you can read the logs, edit the configuration and take backups without root, and the build machine can publish firmware into it over ssh as you. If root owns the directory, the server runs as uid 10001 instead.

## Start the server

```sh
sudo docker run -d --name l0destar --restart unless-stopped \
    -e L0DESTAR_HOSTNAME=tracker.example.com \
    -v /srv/l0destar:/data \
    -p 65480:65480/udp -p 65481:65481/tcp -p 127.0.0.1:5000:5000 \
    m4rkw/l0destar
```

| Option | What it does |
|---|---|
| `--restart unless-stopped` | Starts the server again after a crash or a reboot, unless you stopped it yourself. |
| `-e L0DESTAR_HOSTNAME=...` | The name trackers reach the server at: the `CONFIG_APP_SERVER_HOST` they are built with. The first start issues the server's certificate for it. |
| `-v /srv/l0destar:/data` | The data directory. The container refuses to start without one. |
| `-p 65480:65480/udp` | Telemetry from the trackers, open to the internet. |
| `-p 65481:65481/tcp` | Firmware update downloads, open to the internet. |
| `-p 127.0.0.1:5000:5000` | The web interface and API, reachable from this machine only. Tailscale or nginx publishes them over HTTPS. |

Keep the device ports at 65480 and 65481: both numbers are built into the firmware.

Follow the first start:

```sh
sudo docker logs -f l0destar
```

```text
l0destar: creating a CA and a certificate for tracker.example.com in /data/certs
l0destar: writing /data/config.yaml
l0destar: creating the database in /data/mysql
...
l0destar: loading the schema
[2026-09-13 15:48:00 +0000] [131] [INFO] Starting gunicorn 26.2.0
2026-09-13 15:48:00 UDP listening on 0.0.0.0:65480
[2026-09-13 15:48:00 +0000] [131] [INFO] Listening at: http://0.0.0.0:5000 (131)
2026-09-13 15:48:00 TLS listening on 0.0.0.0:65481
```

The lines in between come from MariaDB starting. `/data` in these messages is `/srv/l0destar` on the host. The server is ready when both listeners have reported, a few seconds after the image has downloaded. Press Ctrl-C to stop following the log - the server keeps running - and check the web interface answers:

```sh
curl -sI http://127.0.0.1:5000/login | head -1
```

It should print `HTTP/1.1 200 OK`. The first start created:

| Path | What it is |
|---|---|
| `/srv/l0destar/config.yaml` | The configuration, with a generated session secret and database password. |
| `/srv/l0destar/certs/` | The CA your trackers will trust, and the server certificate issued from it. |
| `/srv/l0destar/mysql/` | The database. |
| `/srv/l0destar/fw/` | Where firmware updates are published. |
| `/srv/l0destar/logs/` | The server's log files. |

| Problem | Cause |
|---|---|
| `l0destar: mount a directory from the host at /data` | The `-v` option is missing. |
| `l0destar: set L0DESTAR_HOSTNAME to the name trackers will reach this server at` | The first start needs the hostname to issue the certificate. Remove the container with `sudo docker rm -f l0destar` and run it again with `-e L0DESTAR_HOSTNAME=...`. |
| `docker run` fails with `port is already allocated` or `address already in use` | Another program on the machine already uses one of the ports. |
| `l0destar: /data/certs needs both server.crt and server.key` | `certs/` holds a CA but no server certificate. Issue one as in [renewing the server certificate](#renewing-the-server-certificate). |

## Give the build machine the CA

Trackers download updates over TLS, and trust the server only if its certificate was issued by the CA built into their firmware. The first start created that CA. The firmware build needs its certificate, `ca.crt`: you copy it to the build machine before your first firmware build, in [minimal config and initial flashing](/board-setup/initial-flashing.html#copy-your-servers-ca-certificate).

| File in `/srv/l0destar/certs/` | What it is | Where it belongs |
|---|---|---|
| `ca.key` | the CA's private key | the server, and a backup kept somewhere safe; it is only needed to issue a new server certificate |
| `ca.crt` | the CA certificate | also `firmware/certs/ca.crt` on the build machine: the firmware build compiles it in, and `push_fw.sh` checks the update endpoint with it |
| `ca_cert.h` | the CA certificate as a C string | not needed: the firmware build makes its own `src/ca_cert.h` from `ca.crt` |
| `server.crt`, `server.key` | the certificate and key for your hostname | the server only |

!!! warning "Keep the CA"
    Keep `/srv/l0destar/certs/` for as long as trackers use it. If it is lost, the next start creates a new CA, and every tracker built with the old one stops accepting updates from your server. Each then needs its stored CA replaced over USB, as described in [the CA certificate](/reference/firmware.html#the-ca-certificate).

If you already have a CA - from an earlier installation, say - put its `ca.crt`, and a `server.crt` and `server.key` issued from it, into `/srv/l0destar/certs/` before the first start. The container then uses them rather than creating new ones.

### Renewing the server certificate

The CA is valid for ten years and the server certificate for 825 days. Issue a new server certificate from the same CA, so trackers go on trusting it:

```sh
cd /srv/l0destar/certs
openssl ecparam -genkey -name prime256v1 -out server.key
openssl req -new -key server.key -out server.csr -subj "/CN=tracker.example.com"
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
    -out server.crt -days 825 \
    -extfile <(printf "subjectAltName=DNS:%s" tracker.example.com)
rm -f server.csr ca.srl
chmod 600 server.key
sudo docker restart l0destar
```

These are the commands the first start runs for the server certificate. If you keep `ca.key` somewhere other than the server, run them there and copy `server.crt` and `server.key` back.

## Create an API token and the first user

```sh
sudo docker exec l0destar python tools/gentoken.py admin-cli
sudo docker exec l0destar python tools/regtoken.py alice tracker.example.com
```

`gentoken.py` prints a bearer token for the HTTP API. `tools/command.py` uses the first token in the database to queue commands, so create one even if nothing else will use the API, and treat it like a password ([Server security](/server/security.html)).

`regtoken.py` prints a single-use enrolment link, valid for 24 hours:

```text
https://tracker.example.com/register?username=alice&token=<64 hex characters>
```

The passkey created from it is bound to the hostname in the link, so use the name people will actually browse to. With Tailscale that is the machine's `.ts.net` name, not the public hostname the trackers use. Open the link once the web interface is reachable over HTTPS, on the phone or computer that should hold the passkey.

All the server's tools run like this, inside the container: `sudo docker exec l0destar python tools/<tool>.py`. They are listed in the [server configuration reference](/reference/server.html#tools).

## Without Docker

`server/README.md` in the repository describes running the server straight from a checkout, with a MySQL or MariaDB of your own.
