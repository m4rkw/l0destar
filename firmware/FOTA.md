# Over-the-air firmware updates

**Before building, installing or relying on any of this, read the [project disclaimer](../DISCLAIMER.md).**

The tracker pulls updates; the server never pushes - and it doesn't poll
either. Every telemetry response carries `fota=<latest>` (appended by
the server from that device's `fw/manifest-<imei>.txt`), the device
compares that against its running build locally, and only when the server has
something newer does it GET the manifest and image over HTTPS. The steady
state costs zero extra requests.

Release with **`./push_fw.sh`** - it builds, signs, uploads the image, flips
the manifest atomically, and verifies the endpoint. Devices pick the release
up on their next telemetry exchange (or power-on) and install it unattended.

**Every device gets its own build.** Units differ in carrier board and in
which interfaces are populated - a v3.4 lays out both CAN and K-wire and is
built with one - and MCUboot checks only the signature, not the hardware the
image expects, so an image for the wrong unit installs cleanly and then
misbehaves. `remote.conf` describes the fleet by IMEI; `push_fw.sh` builds one
image per device from it and never layers `local.conf` into a published image.

## Disabling updates on a bench build

`CONFIG_APP_FOTA_INHIBIT=y` leaves the update machinery compiled in but never
uses it: no manifest fetch at power-on, no download, and a server advertising
a newer version or sending a manual `fota` command is ignored.

A bench build needs it.  The power-on check is unconditional and a local build
carries no patch number, so if the server has a build published for its IMEI
it reports a lower version and is replaced within seconds of booting — the
change under test never gets to run.

Use this rather than `CONFIG_APP_FOTA=n`.  Turning the subsystem off also
stubs out `fota_confirm_image()`, and an image installed over the air boots on
probation: without that call MCUboot reverts it on the next boot.  A test
build delivered by FOTA would roll straight back.  With the inhibit the
running image is still confirmed.

Never set it in a production image; the unit would never take another update.


## When it checks

| Trigger | Where |
|---|---|
| **Power-on** - the one unconditional check, so a freshly flashed or long-offline unit converges without waiting for a response | `main()`, once start-up has finished |
| **Telemetry response advertised a newer version** (`fota=X.Y.Z` → `fota_notify_available`) | serviced at the next safe moment: `STATE_IDLE` while the engine is not running, after a send in `do_ignition_sleep()`, at key-off before sleeping, and on the `do_sleep()` telemetry wake |
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

## The fleet: `remote.conf`

`remote.conf` (gitignored - it holds the PSK; `remote.conf.example` is the
committed template) lists deployed units by IMEI. INI sections: `[common]` is
layered under every device, the per-IMEI section wins. Lowercase `key = value`
lines are build metadata (`name`, `profile`, `board`), `CONFIG_*` lines go into
that device's Kconfig fragment verbatim.

```
[common]
CONFIG_APP_SERVER_HOST="tracker.example.com"

[350000000000000]
name    = car
profile = makerdiary
CONFIG_APP_BOARD_L0DESTAR_V3_4=y
CONFIG_APP_OBD_MODE=2
```

`./push_fw.sh --list` prints the resolved config per device without building.

A device not listed here gets no manifest, so the server stops advertising
`fota=` to it and it never updates. That is deliberate: no update is a safe
failure, someone else's build is not.

`local.conf` is **not** layered into a published image - `push_fw.sh` passes
its generated fragment as `LOCAL_CONF` instead, which replaces it. Overlays
that are not bench-specific by name still reach a deployed build though
(`makerdiary.conf` applies to every Connect Kit build, PCB units included), so
`push_fw.sh` asserts the production value of every debug knob in the built
`.config` before it publishes and refuses the release otherwise.

## Manifest

`GET https://<host>/fw/manifest.txt?imei=<imei>&v=<running version>`

The device has always sent its IMEI here; the server now uses it, rewriting
the request onto `fw/manifest-<imei>.txt` and 404ing when there is no such
file. Deployed firmware needed no change for this. For a version that failed to
boot on that device it answers `version=<that version>` and `status=blocked`
instead, so the device refuses that build and still takes a newer one.

```
version=0.5.0
file=fw/l0destar-0.5.0-350000000000000.bin
board=v3.4+kline
```

One `key=value` per line; `#` comments, blank lines and unknown keys are
ignored. `file` is relative to the same host.

`board` is the carrier board plus its fitted interfaces
(`<APP_BOARD_ID>[+can][+kline][+aio]`, composed identically by `fota.c` and by
`push_fw.sh` from the build's `.config`). The device compares it against its
own build and **refuses an image that doesn't match** - the backstop for a
wrong or missing IMEI mapping, since nothing below this layer can tell one
board's image from another's. A manifest with no `board=` line predates the
check and is accepted with a warning.

Versions are `MAJOR.MINOR.PATCH`, each field 0–255 (the MCUboot image header's
range). The device installs **strictly newer** only - equal is the steady
state, and older would loop forever. Since the patch is derived from what is
already published rather than from the repo, every push is newer than the last
by construction. To roll a fleet back, rebuild the old code and push it: it
goes out under the next number.

## Server side

The public server in `server/` keeps firmware in its `fw_dir`, `/srv/l0destar/fw`
in the Docker installation, and `server/tracker/firmware.py` does the rest:

- **`fw/manifest-<imei>.txt` + `fw/l0destar-<ver>-<imei>.bin`** - written by
  `push_fw.sh` over ssh (`FW_SERVER`, `FW_DIR`), one pair per device.
- **`fota=<version>` indication** - every telemetry reply to a device with a
  manifest carries the version that manifest names (`latest_version(imei)`,
  cached on mtime), newer or not, unless that version has failed to boot on the
  device. A device with no manifest gets no `fota=` at all. Old firmware ignores
  the unknown key.
- **Failed updates** - the device's `F,fota,staged` line records what it staged.
  The server confirms the update, and raises `fota: updated to <version>`, when
  the device reports that version running. `F,fota,failed`, or another version
  still running ten minutes after staging, withholds that version from the
  device until a newer one is published or it is retried.
- **`fw/published.txt`** - synthesised per request from the image filenames and
  manifests in `fw/`; every `MAJOR.MINOR.PATCH` ever published, oldest first.
  This is how `push_fw.sh` picks the next patch number without shell access on
  the server.
- **The download endpoint** - the TLS listener on **65481**, which serves
  firmware and nothing else: a connection that does not open with `GET` or
  `HEAD` is closed. It implements exactly what the nRF91's FOTA stack needs:
  GET/HEAD on `/fw/*`, HTTP/1.1 keep-alive, and Range support - over
  modem-offloaded TLS the modem decodes ~2 KB at a time, so the downloader
  fetches the image as sequential 2048-byte ranged GETs and expects `206` +
  `Content-Range` for each. `push_fw.sh` reads through this same endpoint rather
  than over ssh, so its checks exercise the port forward a device depends on
  instead of loopback on the server; ssh is left doing only the writes (upload
  and manifest swap).

[`server/docs/PROTOCOL.md`](../server/docs/PROTOCOL.md) describes the requests in
full.

TLS uses the server's private CA: the listener serves `certs/server.crt` and
`certs/server.key`, issued by that CA for the hostname in
`CONFIG_APP_SERVER_HOST`, and the device trusts it via a **dedicated FOTA
sec_tag** (`APP_FOTA_SEC_TAG`, default 42) that `modem_provision_tls()` fills
with `src/ca_cert.h` on first boot.  It is deliberately not `TLS_SEC_TAG` (1),
which gets the same CA: past DTLS/PSK experiments left extra credential types
on tag 1 in modem NVM, and a tag mixing PSK and CA entries makes
certificate-mode TLS `connect()` fail.  No
manual provisioning needed either way.

One field gotcha worth recording: on mfw 2.0.4 a TLS `connect()` that cannot
reach the server reports `EINVAL` (22) rather than a timeout - if FOTA fails
that way while telemetry works, suspect the TCP port forward, not the TLS
config (UDP telemetry proves nothing about TCP).

Set `CONFIG_APP_FOTA_SEC_TAG=-1` to fetch over plain HTTP instead. The image is
still signature-checked by MCUboot, but the manifest and the URL it names are
then unauthenticated.

## Releasing an update

`./push_fw.sh` (or `--device <imei>` for one unit). There is nothing to bump.

`VERSION` reads `0.4` and nothing else - **no patch number is committed**.
`push_fw.sh` derives it from what is actually published on the server (the
highest `MAJOR.MINOR.x` among the images and manifests in `fw/`, plus one) and
hands it to `build.sh` as `FW_PATCH`. So the counter lives with the fleet, a
fresh clone can't reuse a number, and there is no bump to forget.

Zephyr won't read `0.4`: `cmake/modules/version.cmake` hard-errors unless
`PATCHLEVEL` is present, and an absent line silently reuses the previous
regex capture (the minor number). `-DVERSION_FILE` doesn't help either - it
doesn't survive sysbuild's per-image cmake; pointed at a file saying `42` it
still built `0.4.0`. So `build.sh` **generates** the long form in place for the
duration of the build and restores `0.4` on the way out, Ctrl-C included:

```
VERSION_MAJOR = 0      # generated, never committed
VERSION_MINOR = 4
PATCHLEVEL = <FW_PATCH>
```

Both forms are accepted on the way in, so a build killed hard enough to skip
the trap self-heals on the next run. The generated file keeps its previous
mtime when the content is unchanged, so it doesn't force a cmake re-run every
build.

That generated file still feeds Zephyr's `<zephyr/app_version.h>`, the version
imgtool stamps into the image header, and the comparison in `fota.c` - one
file, so those three cannot drift. A plain `./build.sh` on the bench gets
`FW_PATCH=0` and builds `MAJOR.MINOR.0`, which is harmless: bench units aren't
in `remote.conf` and are never offered an update.

The number comes from `GET /fw/published.txt` - a plain request to the same
endpoint a tracker uses, public hostname and private CA, no shell access. The
server answers from its own `fw/` directory, reading the **image filenames** as
well as the manifests: a manifest gets overwritten, an image file never is, so
the filenames are the durable record of what has gone out.

Undershooting would publish a release the fleet silently ignores, so the probe
fails closed - an unreachable endpoint or a non-200 aborts the push rather than
falling back to `0`. `--patch <n>` pins a number explicitly; a patch above 255
is refused (the MCUboot image header holds one byte per field), and bumping the
minor in `VERSION` (`0.4` -> `0.5`) restarts the count.

`./push_fw.sh --list` shows the version that would be published without
building anything.

For each device in `remote.conf` the script generates its Kconfig fragment,
builds into `build_remote_<imei>/`, refuses a stale build (image version must
match the one being released), a non-newer one (devices only install strictly
newer; `--force` overrides) or one carrying bench settings, uploads
`zephyr.signed.bin` as `fw/l0destar-<ver>-<imei>.bin`, updates that device's
manifest last and
atomically (a device fetching mid-push sees the old release or the new one,
never a manifest naming a half-uploaded file), and then verifies the endpoint
the way that device will: CA-verified TLS, the manifest fetched through the
same `?imei=` query the firmware sends, and `206` to a ranged GET.

A unit reports its running version to the server as `fw=` on the first
telemetry record after each boot, on any settings-sync record, and in the reply
to the `config` command; the server stores it on every `log` row by carrying
the last value forward. The
update itself is visible as three alerts: `fota: x -> y available, downloading`
as the transfer starts, `fota: x -> y, rebooting` before the swap and
`fota: updated to y` from the server once the device reports running it. The
first is raised
once per advertised version, so a download that has to be retried on a later
wake does not repeat it; a download that fails every attempt in a wake raises
`fota: x -> y failed after N attempts`, also once per version.

## What happens on the device

1. Manifest fetched, version compared, battery gate checked.
2. A `fota: x -> y available, downloading` alert is sent. GNSS is stopped (it
   shares the antenna path with LTE) and the image streams into
   `mcuboot_secondary`. The watchdog is fed throughout;
   `CONFIG_APP_FOTA_DOWNLOAD_TIMEOUT_S` (20 min) bounds the whole transfer.
3. `dfu_target` marks the slot `BOOT_UPGRADE_TEST`.
4. A `fota: x -> y, rebooting` alert is sent, the radio is powered down, and the
   device reboots.
5. MCUboot swaps the slots and boots the new image.
6. The new image runs its whole init sequence, then calls
   `boot_write_img_confirmed()`. The server raises `fota: updated to y` once
   the device reports that version running.

Step 6 is the safety net: **an image that hangs or faults during bring-up never
confirms itself, and MCUboot reverts to the previous one on the next boot.** A
failed download is also harmless - the primary slot is untouched, the failure
count backs the retry off, and GNSS is restarted.

## Flash layout

MCUboot splits the 1 MB flash into two 416 KB slots (`pm_static.yml`); TF-M
plus the application is about 300 KB today.

**The first build with MCUboot must be flashed over SWD.** A unit running a
pre-MCUboot image has no bootloader to swap slots and cannot update itself into
one.

`pm_static.yml` pins the map so later builds stay installable by the bootloader
already on deployed units. Read its header before changing anything about the
layout - in particular the TF-M partition is 99.4% full, and buying headroom
there costs application space in 32 KB steps.

## Signing key

MCUboot installs only images signed with the project key, `mcuboot_priv.pem`
beside `build.sh` (gitignored), which `build.sh` passes to sysbuild as
`SB_CONFIG_BOOT_SIGNATURE_KEY_FILE`. A bench build creates the key with the
SDK's imgtool when it is missing. A release (`push_fw.sh`, which sets
`FW_PATCH`) stops instead, because it has to be signed with the key the fleet's
bootloaders already trust.

Back it up - the public half is baked into every deployed bootloader, so losing
the private half means no device already in the field can ever be updated
again.

## Configuration

| Symbol | Default | Purpose |
|---|---|---|
| `APP_FOTA` | `y` | Master switch; pulls in the FOTA libraries |
| `APP_FOTA_HOST` | `""` | Update host; empty reuses `APP_SERVER_HOST` |
| `APP_FOTA_PORT` | `65481` | The server's TLS listener, which serves firmware only |
| `APP_FOTA_SEC_TAG` | `42` | Modem sec_tag holding the server CA; `-1` = plain HTTP |
| `APP_FOTA_MANIFEST_PATH` | `/fw/manifest.txt` | |
| `APP_FOTA_MIN_BATTERY_MV` | `12000` | Below this, defer the download |
| `APP_FOTA_REQUIRE_BATTERY_READING` | `n` | Also defer when no INA228 is fitted |
| `APP_FOTA_RETRY_HOLDOFF_S` | `600` | After a failed attempt; doubles up to 8× |
| `APP_FOTA_MANIFEST_TIMEOUT_S` | `30` | |
| `APP_FOTA_DOWNLOAD_TIMEOUT_S` | `1200` | |
| `APP_FOTA_FRAGMENT_SIZE` | `0` | Range size; clamped to 2048 over modem TLS |
