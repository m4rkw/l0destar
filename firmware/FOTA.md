# Over-the-air firmware updates

**Before building, installing or relying on any of this, read the [project disclaimer](../DISCLAIMER.md).**

The tracker pulls updates; the server never pushes - and it doesn't poll
either. Every telemetry response carries `fota=<latest>` (appended by
`_process_telemetry` in the server from `fw/manifest.txt`), the device
compares that against its running build locally, and only when the server has
something newer does it GET the manifest and image over HTTPS. The steady
state costs zero extra requests.

Release with **`./push_fw.sh`** - it builds, signs, uploads the image, flips
the manifest atomically, and verifies the endpoint. Devices pick the release
up on their next telemetry exchange (or power-on) and install it unattended.

## When it checks

| Trigger | Where |
|---|---|
| **Power-on** - the one unconditional check, so a freshly flashed or long-offline unit converges without waiting for a response | `main()`, after `watchdog_init()` |
| **Telemetry response advertised a newer version** (`fota=X.Y.Z` → `fota_notify_available`) | serviced from `STATE_IDLE` or the `do_sleep()` telemetry wake, right after response processing |
| Bare `fota` command from the server (manual force, skips the failure holdoff) | same |

Failed attempts set a holdoff (`CONFIG_APP_FOTA_RETRY_HOLDOFF_S`, 10 min
doubling to 8×) so a broken image or endpoint doesn't burn a download attempt
on every send while the server keeps advertising it.

## Battery gate

A download is deferred - not failed - when the battery is below
`CONFIG_APP_FOTA_MIN_BATTERY_MV` (12.0 V). Nothing touches flash before that
gate, so an update can't drain a weak battery, and a brownout mid-download only
leaves an unusable secondary slot; the running image is never at risk.

A reading below 5 V (`IMPLAUSIBLE_VOLTAGE`) means no INA228 rather than a flat
battery - the same convention `data.c` uses for the low-battery alert. That
case updates anyway; set `CONFIG_APP_FOTA_REQUIRE_BATTERY_READING=y` to require
a real measurement instead.

## Manifest

`GET https://<host>/fw/manifest.txt?imei=<imei>&v=<running version>`

```
version=0.5.0
file=fw/l0destar-0.5.0.bin
```

One `key=value` per line; `#` comments, blank lines and unknown keys are
ignored. `file` is relative to the same host. The query string lets a dynamic
endpoint stage rollouts per device; a static file server ignores it.

Versions are `MAJOR.MINOR.PATCH`, each field 0–255 (the MCUboot image header's
range). The device installs **strictly newer** only - equal is the steady
state, and older would loop forever against a build whose `VERSION` file was
never bumped. To roll a fleet back, republish the old image under a higher
version.

## Server side

Everything lives on machine `a` in `/var/www/tracker`:

- **`fw/manifest.txt` + `fw/l0destar-<ver>.bin`** - written by `push_fw.sh`.
- **`fota=<version>` indication** - `_process_telemetry()` in `main.py` reads
  the manifest (cached on mtime) and appends the version to every telemetry
  response, over UDP, TLS and DTLS alike. Old firmware ignores the unknown
  key.
- **The download endpoint** - public 443 terminates on a different host, so
  the firmware is served on the telemetry TLS port **65481**, the one
  forwarded TCP path to the server. `handle_tls_connection()` sniffs the
  first two bytes after the handshake: telemetry frames start with a 2-byte
  length ≤ 8192, HTTP starts with `GE`/`HE`, so the two protocols share the
  port and certificate. `_handle_fw_http()` implements exactly what the
  nRF91's FOTA stack needs: GET/HEAD on `/fw/*`, HTTP/1.1 keep-alive, and
  Range support - over modem-offloaded TLS the modem decodes ~2 KB at a time,
  so the downloader fetches the image as sequential 2048-byte ranged GETs and
  expects `206` + `Content-Range` for each.

TLS uses the existing private CA: the listener serves `certs/server.{crt,key}`
(CN matching `CONFIG_APP_SERVER_HOST`, ECDSA P-256, issued by that CA), and
the device trusts it via a **dedicated FOTA sec_tag** (`APP_FOTA_SEC_TAG`,
default 42) that `modem_provision_tls()` fills with `src/ca_cert.h` on first
boot.  It is deliberately not the telemetry `TLS_SEC_TAG` (1): past DTLS/PSK
experiments left extra credential types on tag 1 in modem NVM, and a tag
mixing PSK and CA entries makes certificate-mode TLS `connect()` fail.  No
manual provisioning needed either way.

One field gotcha worth recording: on mfw 2.0.4 a TLS `connect()` that cannot
reach the server reports `EINVAL` (22) rather than a timeout - if FOTA fails
that way while telemetry works, suspect the TCP port forward, not the TLS
config (UDP telemetry proves nothing about TCP).

Set `CONFIG_APP_FOTA_SEC_TAG=-1` to fetch over plain HTTP instead. The image is
still signature-checked by MCUboot, but the manifest and the URL it names are
then unauthenticated.

## Releasing an update

1. Bump `VERSION` (`VERSION_MAJOR` / `VERSION_MINOR` / `PATCHLEVEL`). This one
   file feeds Zephyr's `<zephyr/app_version.h>`, the version imgtool stamps
   into the image header, and the comparison in `fota.c` - they cannot drift.
2. `./push_fw.sh`

That's it. The script builds, refuses a stale build (image version must match
the `VERSION` file) or a non-newer one (devices only install strictly newer;
`--force` overrides for re-publishing the same version), uploads
`zephyr.signed.bin` as `fw/l0destar-<ver>.bin`, updates the manifest last and
atomically (a device fetching mid-push sees the old release or the new one,
never a manifest naming a half-uploaded file), and then verifies the endpoint
the way a device will: CA-verified TLS, correct manifest, `206` to a ranged
GET.

A unit reports its running version to the server as `fw=` on the first
telemetry record after each boot, on any settings-sync record, and in the reply
to the `config` command; the server stores it on every `log` row by carrying
the last value forward. The
update itself is visible as two alerts: `fota: x -> y, rebooting` before the
swap and `fota: updated to y` from the new image after it confirms.

## What happens on the device

1. Manifest fetched, version compared, battery gate checked.
2. GNSS is stopped (it shares the antenna path with LTE) and the image streams
   into `mcuboot_secondary`. The watchdog is fed throughout;
   `CONFIG_APP_FOTA_DOWNLOAD_TIMEOUT_S` (20 min) bounds the whole transfer.
3. `dfu_target` marks the slot `BOOT_UPGRADE_TEST`.
4. A `fota: x -> y, rebooting` alert is sent, the radio is powered down, and the
   device reboots.
5. MCUboot swaps the slots and boots the new image.
6. The new image runs its whole init sequence, then calls
   `boot_write_img_confirmed()` and raises a `fota: updated to y` alert.

Step 6 is the safety net: **an image that hangs or faults during bring-up never
confirms itself, and MCUboot reverts to the previous one on the next boot.** A
failed download is also harmless - the primary slot is untouched, the failure
count backs the retry off, and GNSS is restarted.

## Flash layout

MCUboot splits the 1 MB flash into two 416 KB slots (`pm_static.yml`); TF-M
plus the application is ~277 KB today.

**The first build with MCUboot must be flashed over SWD.** A unit running a
pre-MCUboot image has no bootloader to swap slots and cannot update itself into
one.

`pm_static.yml` pins the map so later builds stay installable by the bootloader
already on deployed units. Read its header before changing anything about the
layout - in particular the TF-M partition is 99.4% full, and buying headroom
there costs application space in 32 KB steps.

## Signing key

Left unconfigured, MCUboot signs with the public test key in the mcuboot repo,
which anyone can use to forge an image; the build prints a warning. Before
shipping, generate a project key and point `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE`
in `sysbuild.conf` at it:

```sh
uv pip install imgtool
imgtool keygen -t ecdsa-p256 -k mcuboot_priv.pem
```

`mcuboot_priv.pem` is gitignored. Back it up - the public half is baked into
every deployed bootloader, so losing the private half means no device already in
the field can ever be updated again.

## Configuration

| Symbol | Default | Purpose |
|---|---|---|
| `APP_FOTA` | `y` | Master switch; pulls in the FOTA libraries |
| `APP_FOTA_HOST` | `""` | Update host; empty reuses `APP_SERVER_HOST` |
| `APP_FOTA_PORT` | `65481` | The telemetry TLS port doubles as the fw endpoint |
| `APP_FOTA_SEC_TAG` | `42` | Modem sec_tag holding the server CA; `-1` = plain HTTP |
| `APP_FOTA_MANIFEST_PATH` | `/fw/manifest.txt` | |
| `APP_FOTA_MIN_BATTERY_MV` | `12000` | Below this, defer the download |
| `APP_FOTA_REQUIRE_BATTERY_READING` | `n` | Also defer when no INA228 is fitted |
| `APP_FOTA_RETRY_HOLDOFF_S` | `600` | After a failed attempt; doubles up to 8× |
| `APP_FOTA_MANIFEST_TIMEOUT_S` | `30` | |
| `APP_FOTA_DOWNLOAD_TIMEOUT_S` | `1200` | |
| `APP_FOTA_FRAGMENT_SIZE` | `0` | Range size; clamped to 2048 over modem TLS |
