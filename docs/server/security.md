# Server security

A tracking server knows where your vehicles are, when they are parked at home and when they move. Whoever controls it can also change how the trackers behave, and the firmware update endpoint runs on the same machine. This page covers what needs protecting and how the server is meant to be run.

## What the server holds

- Every position, ignition change, battery reading and ECU value the trackers have sent, for as long as you keep it.
- Each device's pre-shared key. With a key, that device's captured telemetry can be decrypted and forged.
- The ability to queue commands, change settings and switch track mode on every tracker.
- Login sessions, passkey public keys and API tokens.
- Firmware images, and the manifests that decide which image each tracker installs.

Every user who can log in can see and control every device: there are no per-user device permissions.

## Keep the web interface private

The recommended setup makes the web interface reachable only over [Tailscale](https://tailscale.com/), so it never faces the internet. Only the device ports, UDP 65480 and TCP 65481, need to be public ([Telemetry port](/server/telemetry-port.html)).

Install Tailscale on the server and join it to your tailnet. In the Tailscale admin console, enable MagicDNS and HTTPS certificates for the tailnet. Then publish the web application - which the container makes available on `127.0.0.1:5000` - on the tailnet:

```sh
sudo tailscale serve --bg --https=443 http://127.0.0.1:5000
```

From a device on your tailnet, open `https://<machine>.<tailnet>.ts.net/`. Create enrolment links for that name rather than the public hostname:

```sh
sudo docker exec l0destar python tools/regtoken.py alice <machine>.<tailnet>.ts.net
```

Finally, restrict the Google Maps API key to `https://<machine>.<tailnet>.ts.net/*`.

This works because Tailscale's proxy passes `X-Forwarded-Host` and `X-Forwarded-Proto` to the application, and those are what the passkey origin checks use. Two things follow from that:

- Serve on port 443 only. A port number in the hostname makes the passkey relying party ID invalid.
- A passkey is bound to the hostname it was registered on. If the machine's tailnet name changes, every user needs a new enrolment link.

Do not enable Tailscale Funnel for this service: Funnel publishes it to the internet. Tailscale's access controls can narrow which devices on your tailnet reach the server at all.

### nginx on the tailnet instead

If you prefer nginx in front, bind it to the server's Tailscale address rather than all addresses, start from `server/deploy/nginx.conf.example`, and give it a certificate from `tailscale cert <machine>.<tailnet>.ts.net`. Tailscale certificates are short-lived, so re-run `tailscale cert` on a schedule and reload nginx afterwards.

## If the web interface has to be public

- Put nginx in front with a public certificate ([Server deployment](/server/deployment.html)) and keep its `proxy_set_header` lines. The application trusts `X-Forwarded-For`, `X-Forwarded-Host`, `X-Forwarded-Proto` and `X-Forwarded-Port`, which is only safe while the proxy overwrites them.
- Publish the web port on loopback only, `-p 127.0.0.1:5000:5000`. Docker opens the ports it publishes ahead of the host firewall, so a port published on every address cannot be hidden behind a firewall rule, and anything that reaches it can forge the forwarded headers.
- Keep the operating system, Docker, nginx and the `m4rkw/l0destar` image up to date ([Server deployment](/server/deployment.html#upgrading)).
- Read `audit.log` regularly.

## Accounts and passkeys

There are no passwords anywhere in the server.

- **Enrolment** is by invitation only. `tools/regtoken.py` creates a link that works once, within 24 hours, and there is no sign-up page.
- **Registration** requires a platform authenticator with user verification and a discoverable credential: a passkey kept by the phone or computer itself, such as the platform's password manager or Windows Hello, created on the device that will use it. Hardware security keys cannot be registered.
- **Re-enrolment** replaces a user's passkey. Running `regtoken.py` again for an existing username is how a lost phone is recovered - which also means anyone who can run the tools on the server can take over any account. Shell access to the server, as the owner of `/srv/l0destar` or as anyone who can run `docker`, is access to every vehicle.
- **Login** asks for the username, then issues a challenge that is valid for five minutes and bound to the IP address and browser that asked for it. Each IP address gets `rate_limit_request_count` challenges (5) per `rate_limit_reset_period` (an hour); a successful login resets the count.
- **Lockout**: five failed passkey verifications lock the account, and any session the account already has stops working as well.
- **Removing a user** is `DELETE FROM user WHERE username = 'alice';`. Every request checks the account, so their existing sessions stop working at their next request, and a map page they have open is disconnected within a minute.

The SQL on this page runs in the server's database, opened with `sudo docker exec -it l0destar mariadb tracker`. Unlock a locked account once you know why it happened:

```sql
UPDATE user SET locked = 0, failed_login_count = 0 WHERE username = 'alice';
```

`audit.log` in `/srv/l0destar/logs` records every enrolment, login and logout attempt with the client address and username. It is a plain file rather than a table, so it survives problems with the database. Keep it, and read it after anything suspicious.

Sessions last `session_lifetime_days` (30). Changing `session_secret` logs everyone out.

## API tokens

The automation endpoints - `/api/1.0/track`, `/api/1.0/config`, `/api/1.0/command` and `/api/1.0/home` - take a bearer token created by `tools/gentoken.py`. Tokens have no scopes and no expiry. Anyone holding one can find every vehicle's last position, change its settings and queue commands such as `reboot`, so treat a token like shell access to the server:

- create one token per consumer, named after it, so each can be revoked on its own;
- keep tokens out of shared scripts and repositories; the server stores them in plain text in the `api_token` table.

```sql
SELECT id, name, FROM_UNIXTIME(created_at) FROM api_token;
DELETE FROM api_token WHERE name = 'home-automation';
```

`tools/command.py` uses the token with the lowest id, so keep one for the command-line tools.

## Secrets on the server

| Secret | Protection |
|---|---|
| `config.yaml`: session secret, database password, notification credentials | mode 600, owned by the owner of `/srv/l0destar` |
| `certs/server.key` and `certs/ca.key` | mode 600, owned by the owner of `/srv/l0destar` |
| the database: device keys, tokens, history | reachable only inside the container, through an account with rights to the data only; encrypted backups |

The MCUboot signing key belongs on the build machine, not on the server, and the CA key needs looking after too; see [below](#signing-and-ca-keys).

## Device transports

- **UDP, port 65480**, carries all telemetry. Every datagram is encrypted and authenticated with ChaCha20-Poly1305 under the device's own 32-byte key, with replay protection, and each reply is bound to the request it answers. Datagrams that fail any check are dropped without a reply, so the port cannot be used to find out which IMEIs are enrolled. The IMEI itself travels in clear, because the server needs it to choose the key: someone on the network path can see which tracker reports and when, but not what it says.
- **TLS, port 65481**, serves firmware downloads and nothing else: it never accepts telemetry. What it does serve still needs care, as the next section explains.

## Firmware images contain device keys

!!! warning "Anyone who can download an image can read its device key"
    Each tracker's image is built with its pre-shared key (`CONFIG_APP_PSK_HEX`) compiled in, and images are signed, not encrypted. The TLS listener serves `/fw/manifest.txt?imei=<imei>` and the image it names to any client that can connect, because nothing in the update protocol authenticates the tracker.

    Anyone who can reach TCP 65481 and knows or guesses an IMEI - IMEIs travel in clear in every datagram and are printed on hardware and in logs - can download that device's image and extract its key. With the key they can decrypt the device's captured telemetry, send telemetry as the device, and, from a position on the network path, forge replies that change its settings.

With current firmware:

- Keep TCP 65481 closed except while you roll out an update, at the router or your provider's firewall ([Telemetry port](/server/telemetry-port.html)). A firewall on the server itself, such as ufw, cannot close a port Docker publishes. Trackers check for updates at power-on and when a reply advertises one; a check that cannot connect fails and is retried later.
- Delete superseded images from `/srv/l0destar/fw`, keeping the one each device's manifest names.
- If an image may have leaked, give the device a new key with `tools/device.py rekey` and reflash it ([Onboarding devices into the server](/board-setup/server-onboarding.html)).

Fixing this properly needs device keys provisioned separately from the image, which the firmware does not support yet.

## Signing and CA keys

- **`firmware/mcuboot_priv.pem`** signs firmware, and trackers install any newer image signed with it. Whoever holds it and can serve updates to your trackers can run code on all of them. Keep it off the server, backed up and ideally offline. If it is lost, no deployed tracker can ever be updated again.
- **`certs/ca.key`** issues the certificates trackers trust for the update endpoint. The server's first start creates it in `/srv/l0destar/certs`, and after that it is only needed to renew the server certificate. Keep a backup somewhere safe. To keep it off the server as well, move it away once the installation is done and renew the server certificate wherever you keep it ([Server installation](/server/installation.html#renewing-the-server-certificate)).
- **`certs/server.key`** is the update endpoint's identity. If it leaks, re-issue the server certificate from the CA - but the leaked certificate stays trusted until it expires, so treat a serious leak as a reason to create a new CA and reflash the trackers.

## Location data and privacy

- If you run trackers, you are responsible for the location data they collect, including when someone else drives the vehicle. See [Deployment: read this first](/deployment/read-this-first.html).
- Nothing is deleted automatically. Decide how long to keep history ([Server deployment](/server/deployment.html)).
- Notifications leave your server. Pushover or a webhook receives device names, alert text and, for `locate`, a position.
- `udp.log` records IMEIs and source addresses, `device.log` firmware messages, and `audit.log` usernames and client addresses. The database holds everything, so encrypt its backups.

## Checklist

- The web interface is only reachable over Tailscale, or behind nginx, and the container publishes port 5000 on `127.0.0.1` only.
- Only UDP 65480 and, when needed, TCP 65481 are reachable from the internet, controlled at the router or provider firewall.
- `config.yaml`, `certs/server.key` and `certs/ca.key` are mode 600.
- Each API consumer has its own token, and unused tokens are deleted.
- The MCUboot signing key is on the build machine, and the CA key is backed up.
- Backups are encrypted, and you have decided how long to keep history.
