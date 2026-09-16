# nRF Cloud onboarding

nRF Cloud is free for up to 10 devices and allows the use of A-GNSS (Assisted
GPS) which lets the tracker get GPS fixes much faster than it otherwise would.

## Initial setup

1. Register for nRFCloud and get your API key from this page: https://app.nrfcloud.com/#/account

Note: you may be redirected to memfault.com, this is Nordic's new interface. For
reasons I don't yet understand the new interface gives a different API key that
doesn't work with this onboarding process (open ticket:
https://devzone.nordicsemi.com/support/362337). If you get redirected go back
to https://app.nrfcloud.com, click the lines in the top right of the page, then
User Account. On this page your API key will be available.

```
export NRF_CLOUD_API_KEY=<your API key>
```

2. Install the nrfcloud-utils

```
uv tool install --python 3.12 nrfcloud-utils
```

3. Create a CA for your devices

This should only be done once, the CA is used for all of your devices. Replace
'GB' with your country code. This CA is only used to vouch for your devices to
nRFCloud, it is completely separate from any CAs that might be configured on
your instance of the server component.

```
git clone https://github.com/m4rkw/l0destar
cd firmware
mkdir -p onboarding
create_ca_cert -c GB -o l0destar -p onboarding -f l0destar
```

4. Build and flash the provisioning firmware

```sh
# in firmware/
PROV=1 BUILD_SUBDIR=build_prov ./build.sh
pyocd load -t nrf91 --no-reset build_prov/merged.hex
pyocd reset -t nrf91 -m hw -O auto_unlock=false
```

5. Install the device credentials

```sh
# in firmware/
device_credentials_installer \
  --port /dev/cu.usbmodem*1 --cmd-type at \
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

6. Onboard the device to your account

```sh
# in firmware/
nrf_cloud_onboard --api-key "$NRF_CLOUD_API_KEY" --csv onboarding/onboard.csv
```

Check that nRF Cloud now lists `nrf-<IMEI>`:

```sh
curl -s "https://api.nrfcloud.com/v1/devices" -H "Authorization: Bearer $NRF_CLOUD_API_KEY"
```

For each further device, repeat steps 2 to 4 with the same CA.

7. Back to the tracker firmware

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
