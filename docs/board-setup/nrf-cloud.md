# nRF Cloud onboarding

This step is optional. It lets the tracker download assistance data (A-GNSS) from Nordic's nRF Cloud when it starts, which cuts the first GNSS fix from 2 to 5 minutes to about 20 seconds on the author's bench. Without it the tracker works the same, it just takes longer to find itself after a cold start.

The firmware authenticates to nRF Cloud with a key and certificate stored in the modem, and nRF Cloud only accepts them once the device has been onboarded to your account. Until then the download fails - the console shows `agnss_data_get` reporting `HTTP 401`, then `A-GNSS fetch failed — first fix will take longer` - and GNSS carries on unassisted. Onboarding is done once per device. The credentials live in the modem's own storage, so they survive every reflash. Check nRF Cloud's current terms for its location services before you rely on them.

The procedure uses a provisioning build of the tracker firmware, which turns the console into a bridge to the modem for Nordic's tools.

## Before you start

You need:

- **A board set up as in [minimal config and initial flashing](/board-setup/initial-flashing.html).** The provisioning build uses the same `local.conf`, because the provisioning firmware still sets up the carrier board's pins.
- **An nRF Cloud account and its API key.** Sign in at [nrfcloud.com](https://nrfcloud.com) and find the key on the User Account page. If the site sends you to Memfault, that is the wrong interface: follow the link back to nRF Cloud to find the key.
- **nrfcloud-utils**, Nordic's provisioning and onboarding tools.

Install nrfcloud-utils with uv:

```sh
uv tool install --python 3.12 nrfcloud-utils
```

Keep the API key in your shell for the commands below:

```sh
export NRF_CLOUD_API_KEY=<your API key>
```

## 1. Create a CA for your devices

Once, for all your devices:

```sh
# in firmware/
mkdir -p onboarding
create_ca_cert -c GB -o l0destar -p onboarding -f l0destar
```

Replace `GB` with your two-letter country code. This writes three files named like `onboarding/l0destar0x<serial>_ca.pem`, `_prv.pem` and `_pub.pem`. `onboarding/` is gitignored. The `_prv.pem` file is this CA's private key - anyone holding it can mint device certificates under your CA - so keep it private and back it up with your other keys.

This CA only vouches for your devices to nRF Cloud. It is separate from the CA your l0destar server's certificate is issued from.

## 2. Build and flash the provisioning firmware

```sh
# in firmware/
PROV=1 BUILD_SUBDIR=build_prov ./build.sh
pyocd load -t nrf91 --no-reset build_prov/merged.hex
pyocd reset -t nrf91 -m hw -O auto_unlock=false
```

`PROV=1` layers `prov.conf` over your configuration: logging is switched off so nothing interleaves with the exchange, the modem is brought up, and the firmware then does nothing except pass commands between the console and the modem. It builds into its own directory, so your normal build is left alone. Use `BUILD_SUBDIR` rather than a relative `BUILD_DIR`, which would resolve inside the SDK directory. The two `pyocd` commands are what `flash.sh` does, pointed at this build - see [why the scripts reset the way they do](/board-setup/initial-flashing.html#why-the-scripts-reset-the-way-they-do).

The repository's own notes build this with `pristine` (which also deletes your normal `build/` directory) and flash it with a `flash-md.sh` that no longer exists; the commands above replace both.

On the console the board prints:

```text
*** PROVISIONING MODE — AT host ready; run nrfcloud-utils ***
```

## 3. Install the device credentials

Close anything that has the serial port open - `screen`, a logger, another terminal. A second program reading the port corrupts the exchange. Then run the installer on the console port - the first `/dev/cu.usbmodem*` port on macOS, or `/dev/serial/by-id/usb-Makerdiary_IFMCU_CMSIS-DAP_<serial>-if00` on Linux:

```sh
# in firmware/
device_credentials_installer \
  --port /dev/cu.usbmodemXXXX --cmd-type at \
  --ca onboarding/*_ca.pem \
  --ca-key onboarding/*_prv.pem \
  --id-imei --id-str nrf- \
  -S 16842753 -d \
  --csv onboarding/onboard.csv --verify
```

The modem generates a private key that never leaves it, the tool signs a certificate for it with your CA and writes the certificate back into the modem, and a row describing the device is written to `onboarding/onboard.csv`.

- `--id-imei --id-str nrf-` names the device `nrf-<IMEI>`, and `-S 16842753` stores the credentials at the security tag the firmware uses. Both must be exactly as shown, or nRF Cloud rejects the firmware's requests.
- `-d` deletes anything already stored at that tag first, so running it again is safe.
- `onboard.csv` is overwritten on each run, so it only ever describes the device you just provisioned.

## 4. Onboard the device to your account

```sh
# in firmware/
nrf_cloud_onboard --api-key "$NRF_CLOUD_API_KEY" --csv onboarding/onboard.csv
```

Check that nRF Cloud now lists `nrf-<IMEI>`:

```sh
curl -s "https://api.nrfcloud.com/v1/devices" -H "Authorization: Bearer $NRF_CLOUD_API_KEY"
```

For each further device, repeat steps 2 to 4 with the same CA.

## 5. Back to the tracker firmware

The provisioning firmware does not track anything. Flash the tracker firmware back: the provisioning build went into its own directory, so `build/` still holds it.

```sh
# in firmware/
./flash.sh
```

From then on, when the tracker registers on the network at start-up, it fetches assistance data before starting GNSS:

```text
<inf> agnss: requesting A-GNSS data from nRF Cloud...
<inf> agnss: received <n> bytes, processing
<inf> agnss: A-GNSS data injected
```

| Message | Meaning |
|---|---|
| `JWT needs modem time; waiting for NTP fallback...` | Normal just after start-up, until the network has provided the time. |
| `agnss_data_get: ... (HTTP 401)` | nRF Cloud refused the device's token ("Auth token is malformed"): the device is not onboarded to your account, or its ID or security tag differs from the commands above. |
| `no network yet — skipping A-GNSS, GNSS will ask later` | The modem had not registered in time; the firmware requests assistance again once GNSS needs it. |
