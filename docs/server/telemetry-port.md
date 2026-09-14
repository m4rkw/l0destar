# Telemetry port

Trackers send everything to the server over **UDP port 65480** and download firmware updates over **TCP port 65481**. Both have to be reachable from the internet. This page explains why, what is exposed, and how to open and test them.

## Why the ports have to be public

A tracker is on a cellular network, not on your LAN or your tailnet. It starts every exchange itself: it sends one UDP datagram to `CONFIG_APP_SERVER_HOST` on port 65480 and, when it wants settings or commands back, waits up to four seconds for the reply on the same socket. The server never opens a connection to a tracker. Its reply goes back to the address and port the datagram came from, through whatever NAT the mobile operator uses.

Updates work the same way over TCP: the tracker connects to port 65481 when it needs a manifest or an image.

## What is exposed on UDP 65480

Every datagram in either direction is encrypted and authenticated with ChaCha20-Poly1305 under the device's own 32-byte key:

```text
request:   [1] IMEI length  [IMEI]  [12] nonce  [ciphertext]  [16] tag
response:  [12] nonce  [ciphertext]  [16] tag
```

- The IMEI is authenticated as additional data, and a reply also binds the nonce of the request it answers, so a captured reply cannot be replayed into another exchange.
- Every nonce a device uses is kept in the database for 30 days, so a replayed datagram is refused, even after a restart. One captured more than 30 days earlier is not detected.
- A datagram that is malformed, names an unknown IMEI, fails authentication or repeats a nonce is dropped without a reply. All of these look the same from outside, so the port does not reveal which IMEIs are enrolled.
- Failures are logged as `decrypt failed from <address> (<n> bytes)`, rate limited per source address: the first in any minute, then every twentieth.
- The IMEI is sent in clear so the server can choose the key. An observer on the path learns which tracker reports and when, but not what it says.

The listener handles one datagram at a time. Junk costs at most a database lookup, but a flood could still delay real trackers, so a rate limit at the router or provider firewall is reasonable if the port attracts traffic.

The wire format is described in full in [PROTOCOL.md](https://github.com/m4rkw/l0destar/blob/master/server/docs/PROTOCOL.md).

## What the firmware has built in

- **The port numbers.** 65480 and 65481 are the defaults of `CONFIG_APP_SERVER_PORT` and `CONFIG_APP_FOTA_PORT`, compiled into every tracker image. The container listens on those ports and `docker run` publishes them under the same numbers; keep it that way, because changing them means rebuilding every tracker with the new numbers.
- **IPv4 only.** The firmware looks the hostname up for IPv4 addresses and nothing else. The server needs a public IPv4 address and the hostname needs an A record; an AAAA record on its own does not work.
- **One DNS lookup per boot.** A tracker resolves `CONFIG_APP_SERVER_HOST` the first time it sends and keeps using that address until it restarts. If the server's public address changes, running trackers carry on sending to the old one, so use a static address. A tracker that has lost the server this way needs a power cycle.
- **The certificate name.** The update client checks the TLS certificate against your CA and the hostname, so the certificate must be issued for `CONFIG_APP_SERVER_HOST` - the `L0DESTAR_HOSTNAME` the server was first started with ([Server installation](/server/installation.html)).

If your DNS is hosted with Cloudflare, the record the trackers use must be DNS only, not proxied: Cloudflare's proxy carries web traffic, not UDP 65480 or TCP 65481. If you want the web interface proxied, give it a different hostname.

## Opening the ports

`docker run` publishes both ports on every address of the server, so on the server itself there is nothing more to open.

### Port forwarding on a router

Give the server a fixed address on your LAN, for example with a DHCP reservation, then forward from the router's WAN side:

| Protocol | External port | Forward to | Internal port |
|---|---|---|---|
| UDP | 65480 | the server's LAN address | 65480 |
| TCP | 65481 | the server's LAN address | 65481 |

Check that the router's WAN address is your public address. If the router shows a WAN address in `100.64.0.0/10`, or in any private range, your connection is behind carrier-grade NAT and port forwarding cannot work. Run the server on a VPS instead, or forward the two ports from a VPS to your server over a tunnel; depending on how you forward them, the server may then see every tracker as coming from the VPS.

### Firewalls

Docker adds its own firewall rules for the ports a container publishes, ahead of the host's rules. A firewall on the server - ufw, or nftables rules of your own - therefore neither needs opening for these two ports nor can close them while the container publishes them: with ufw denying every incoming connection, both still reach the server.

On a cloud server, add inbound rules for UDP 65480 and TCP 65481 from anywhere to the instance's security group or network firewall. That firewall is outside the machine, so it does apply.

Never publish the web application's port (5000) on anything but `127.0.0.1`.

### Opening TCP 65481 only for updates

Firmware images contain device keys, and nothing authenticates who downloads them, or who reports a staged update as installed ([Server security](/server/security.html)). If that matters to you, keep TCP 65481 closed and open it only while you publish and roll out an update. Do it where the port reaches the server - the router's port forwarding rule, or the provider's firewall - or leave `-p 65481:65481/tcp` out of `docker run` and create the container again with it for a rollout.

A tracker that checks for an update while the port is closed fails the check and tries again later.

## Checking reachability

Test UDP from a machine outside your network, such as a laptop on a phone hotspot or a VPS:

```sh
printf test | nc -u -w1 tracker.example.com 65480
```

Then on the server:

```sh
tail -n 5 /srv/l0destar/logs/udp.log
```

A line such as `decrypt failed from 203.0.113.7 (4 bytes)`, showing your outside address, means the datagram arrived. If nothing appears it did not: check DNS, the router, the provider's firewall and carrier-grade NAT. The failure log is rate limited, so wait a minute between attempts. If your router loops traffic for its own public address back into the LAN, the test also works from inside your network, with the router's LAN address in the log. That loopback can lose datagrams which would arrive intact from outside, though, so confirm a failure from outside before suspecting the server.

Test TCP 65481 from the build machine, which also checks the certificate:

```sh
# in firmware/
curl --cacert certs/ca.crt https://tracker.example.com:65481/fw/published.txt
```

Any reply - an empty one is normal before anything is published - means the port, the certificate and the hostname are right. A certificate error means the server certificate was not issued by that CA, or not for that name.

A working tracker appears in `udp.log` as `<n> records from <imei> (<address>)` each time it sends.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Nothing in `udp.log` when the tracker sends | wrong `CONFIG_APP_SERVER_HOST`, no A record, port forwarding or the provider's firewall, carrier-grade NAT, or the tracker is not registered on the mobile network |
| `decrypt failed from ...` every time the tracker sends | the key built into the firmware is not the device's key on the server, or the device is not enrolled |
| Records arrive but settings and commands never take effect | replies do not get back through NAT or an outbound firewall |
| It worked until the server's public address changed | trackers resolve the hostname once per boot; power-cycle them |
| Update checks fail on the tracker while telemetry works | TCP 65481 is not reachable; on the tracker's console a failed TLS connection can show as error 22 (`EINVAL`) rather than a timeout |
| `TLS handshake failed` or handshake timeouts in `tls.log` during updates | weak signal; keep `tls_handshake_timeout` at its default of 45 seconds or raise it |

The tracker's own console shows what it resolved and sent; see [Verifying telemetry](/board-setup/verifying-telemetry.html).
