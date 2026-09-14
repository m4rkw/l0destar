# OTA updates

l0destar devices update themselves over the air from your server. Nothing is pushed: you publish a build, each device notices the next time it hears from the server, downloads it and installs it with the MCUboot bootloader, which puts the old firmware back if the new one does not start.

Every device gets its own build. Boards differ in revision and in which OBD interface is fitted, and MCUboot only checks that an image is signed, not that it suits the hardware - an image built for another board installs cleanly and then misbehaves. So the fleet is described device by device, and a device the server has no build for is never offered one.

## How an update reaches a device

1. You run `push_fw.sh` on your build machine. For each device in `remote.conf` it builds an image, uploads it to the server's firmware directory as `l0destar-<version>-<imei>.bin` and writes that device's `manifest-<imei>.txt`.
2. From then on, every reply the server sends that device carries `fota=<version>`, and the device compares it with the version it is running.
3. When the published version is newer, the device checks for the update at the next safe moment: from its main loop while the engine is not running, on a timed engine-off wake, or on the way to sleep when the ignition is switched off. It also checks every time it boots, and when it is sent the `fota` command.
4. It fetches `/fw/manifest.txt?imei=<imei>&v=<running version>` from `CONFIG_APP_SERVER_HOST` on TCP port 65481 over TLS, checking the server's certificate against the CA stored in its modem. It installs only a strictly newer version whose `board=` matches its own build (for example `v3.4+kline`), and waits while the vehicle battery reads below 12.0V.
5. It raises the alert `fota: <old> -> <new> available, downloading`, stops GNSS (which shares the radio front end) and downloads the image into its second flash slot in 2KB pieces: up to three attempts per check, within a 20 minute budget.
6. It tells the server the image is staged, raises `fota: <old> -> <new>, rebooting` and restarts.
7. MCUboot swaps the new image in as a trial. The new firmware runs its whole start-up and then confirms itself. An image that hangs or crashes before that is swapped back out on the next boot.
8. The server sees the new version when the device next checks in and sends `<name>: fota: updated to <version>`. If MCUboot put the old image back instead, the server sends `<name>: fota: <version> failed to boot (running <old>) — updates withheld until retried` and stops offering that version to that device.

A failed download is retried after 10 minutes, doubling up to 80 minutes. Once a version has failed to boot on a device, the server stops offering it to that device, and the device refuses it itself after a second failure, until you [retry it](#when-an-update-fails). A newer version is always offered.

## What you need

- TCP port 65481 on the server reachable from the internet. The Docker installation sets up the TLS listener itself; see [telemetry port](/server/telemetry-port.html).
- Devices whose modems trust your CA: built with your server's CA from their very first boot, as in [minimal config and initial flashing](/board-setup/initial-flashing.html#copy-your-servers-ca-certificate).
- SSH access from your build machine to the server as the owner of `/srv/l0destar`, who can write to its firmware directory, `/srv/l0destar/fw`. See [server deployment](/server/deployment.html).
- `firmware/certs/ca.crt`, your server's CA certificate. `push_fw.sh` uses it to check the endpoint the way a device will.
- `firmware/mcuboot_priv.pem`, the signing key your first build created. Every build for every device must be signed with the same key for as long as the devices are in service.
- curl, ssh and scp. `push_fw.sh` runs on macOS and Linux.
- Each device's IMEI, and the key it was [enrolled](/board-setup/server-onboarding.html) with.

## Describe your devices in remote.conf

`firmware/remote.conf` lists the devices in the field, one section per IMEI. It is gitignored because it holds their keys - back it up along with the signing key. Start from the example:

```sh
# in firmware/
cp remote.conf.example remote.conf
```

Then replace the contents with your own devices:

```ini
[common]
CONFIG_APP_SERVER_HOST="tracker.example.com"
CONFIG_APP_APN="your.apn"
CONFIG_LTE_NETWORK_MODE_LTE_M_GPS=y

[350000000000000]
name    = Car
profile = makerdiary
CONFIG_APP_BOARD_L0DESTAR_V3_4=y
CONFIG_APP_OBD_MODE=2
CONFIG_APP_PSK_HEX="<64 hex characters>"
```

- `[common]` applies to every device. Each IMEI section is layered on top of it and wins.
- Lower-case `key = value` lines are for the script: `name` labels the device in its output, `profile` is `makerdiary` for the Connect Kit (or `dk` for Nordic's development kit), and `board` names a Zephyr board target explicitly, which is rarely needed.
- `CONFIG_` lines are copied verbatim into that device's configuration fragment, `.remote/<imei>.conf`.
- `local.conf` is never included, so nothing from your bench reaches a device in the field.
- Set the APN here: the example does not, and a production build would otherwise get the `prj.conf` default.
- If your server publishes its ports under other numbers, put `CONFIG_APP_SERVER_PORT` and `CONFIG_APP_FOTA_PORT` in `[common]` too, and set `VERIFY_PORT` to the update port when you run `push_fw.sh`.
- The options a vehicle build needs, such as K-wire telemetry and alert priorities, are covered in [deployment configuration](/deployment/configuration.html).

!!! warning "One key per device"
    The example puts `CONFIG_APP_PSK_HEX` in `[common]`, which would give every device the same key. Put each device's key - the one it was enrolled with - in its own section.

!!! warning "Never inhibit updates in remote.conf"
    A device built with `CONFIG_APP_FOTA_INHIBIT=y` never updates again, and `push_fw.sh` does not check for it. Keep that setting in `local.conf`.

## Publish a release

Tell the script where the server keeps its firmware, check what it is about to do, then publish:

```sh
# in firmware/
export FW_SERVER=you@tracker.example.com
export FW_DIR=/srv/l0destar/fw
./push_fw.sh --list
./push_fw.sh
```

`FW_SERVER` is the SSH destination and `FW_DIR` the firmware directory on the server. Always set both: the defaults are the author's own server. The script takes the hostname to verify against from `CONFIG_APP_SERVER_HOST` in `[common]`.

`--list` prints the version it would publish and each device's resolved configuration without building anything. Then, for each device, `push_fw.sh`:

1. Writes the device's configuration fragment and builds it into `build_remote_<imei>/`.
2. Refuses to publish a build whose version does not match the release, a version that is not newer than the one the device is already offered (unless `--force`), or a build with a bench override left on - `APP_DEBUG_IGNITION`, `APP_DEBUG_BATTERY_MV`, `APP_CAN_TEST`, `APP_KLINE_TEST` or `APP_ACCEL_TEST`.
3. Uploads the signed image, then replaces the manifest in one step, so a device fetching in the middle sees either the old release or the new one.
4. Checks the result the way the device will: the manifest through `?imei=` over TLS on port 65481, trusted by your CA, and a ranged request for the image answered with `206`.

It finishes like this:

```text
done: 1 device(s) will pull 0.4.13 on their next telemetry
      exchange or power-on
```

| Option | Effect |
|---|---|
| `--device <imei>` | Build and publish only that device. |
| `--list` | Show the next version and each device's configuration; publish nothing. |
| `--no-build` | Publish the images already in `build_remote_*/`. |
| `--force` | Allow a version that is not newer than the live one. |
| `--patch <n>` | Use this patch number instead of working it out. |

### Version numbers

`firmware/VERSION` holds only the major and minor version (`0.4`). The patch number is worked out from what your server has already published - it lists every version it holds at `/fw/published.txt` - plus one, so there is nothing to bump by hand and two machines cannot reuse a number. The first release of a minor version is patch 1: patch 0 is what every bench build gets, and a device only installs a version newer than the one it runs. If that list cannot be fetched, the script stops rather than guess.

Devices install only strictly newer versions, and each part of the version runs from 0 to 255. When the patch number would pass 255, raise the minor version in `VERSION` (for example to `0.5`) and patches start again from 1. To roll a device back, check out the old code and publish it: it goes out under the next number.

## Watching an update

A device learns about an update the next time it reads a reply from the server: within about 30 seconds when its ignition is on, and while driving. A parked device that makes no timed wakes learns at its next journey, and normally installs the update when the ignition is switched off. The [device settings reference](/reference/device-settings.html) lists when replies are read.

As it happens you get, in order:

- `<name>: fota: 0.4.12 -> 0.4.13 available, downloading` from the device.
- `<name>: fota: 0.4.12 -> 0.4.13, rebooting` from the device.
- `<name>: fota: updated to 0.4.13` from the server.

On the server, `tls.log` shows the manifest request and the first and last ranges of the download, and the transport logs record the device staging the version and then `... is running 0.4.13 — update confirmed`. The version each device runs is shown in the web interface's device list and stored with every record.

## When an update fails

| What you see | What happened | What to do |
|---|---|---|
| `fota: <old> -> <new> failed after 3 attempts (err ..., cause ...)` | The download did not complete. | Check TCP 65481 is reachable from outside and the certificate is valid. The device retries by itself; the `fota` command retries now. |
| `<name>: fota: <version> failed to boot (running <old>) — updates withheld until retried` | The image installed but did not start, and MCUboot reverted it. | Publish a fixed, newer version (offered automatically), or retry the same one with `command.py <imei> fota-retry`. |
| Console: `manifest targets board '...', this unit is '...' — refusing` | The device's section in `remote.conf` does not match its hardware. | Correct the section and publish again. |
| Console: `battery ... — deferring update` | The vehicle battery reads below 12.0V. | Nothing: it tries again at the next check. |
| Console: `manifest request failed` while telemetry works | The TCP path to port 65481 is broken - working UDP telemetry proves nothing about TCP - or the modem trusts a different CA. | Check port 65481 is forwarded and open; a failed connection often reports error 22 (EINVAL) rather than a timeout. If the board ever booted the tracker firmware with another CA, the recovery is in [firmware build options](/reference/firmware.html). |

To retry a version the server is withholding, run on the server:

```sh
sudo docker exec l0destar python tools/command.py 350000000000000 fota-retry
```

That clears the server's block and queues the `fota` command, which clears the device's own block and makes it check at the next reply it reads.

## Bench units

- Build bench units with `CONFIG_APP_FOTA_INHIBIT=y`, as in [minimal config and initial flashing](/board-setup/initial-flashing.html). The unit skips the check at boot and ignores update adverts and the `fota` command.
- Never use `CONFIG_APP_FOTA=n` instead. That also removes the call that confirms an image, so a build that did arrive over the air would be reverted on its next boot.
- Keep bench units out of `remote.conf`. A device with no manifest on the server is never offered anything.

A unit running an inhibited bench build never fetches an update, so to move it into the fleet add its section to `remote.conf`, publish it, then flash that same image over USB once:

```sh
# in firmware/
./push_fw.sh --device 350000000000000
pyocd load -t nrf91 --no-reset build_remote_350000000000000/merged.hex
pyocd reset -t nrf91 -m hw -O auto_unlock=false
```

## Security

!!! warning "Images contain the device's key"
    `CONFIG_APP_PSK_HEX` is compiled into each image, and the server's TLS port serves images to anyone who can reach it and knows the device's IMEI. Read [server security](/server/security.html) before leaving TCP 65481 open permanently.

The signing key is the other half: anyone holding `mcuboot_priv.pem` can build firmware every one of your devices will accept. Keep it offline and backed up.
