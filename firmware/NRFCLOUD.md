# nRF Cloud

**Before building, installing or relying on any of this, read the [project disclaimer](../DISCLAIMER.md).**

## How to set up GPS assist

The full walkthrough is [nRF Cloud onboarding](../docs/board-setup/nrf-cloud.md) in the documentation. Run these in `firmware/`.

1. Build the firmware in provisioning mode, into its own directory so the normal build is left alone

```
PROV=1 BUILD_SUBDIR=build_prov ./build.sh
```

2. Flash it to the l0destar board - what `flash.sh` does, pointed at that build

```
pyocd load -t nrf91 --no-reset build_prov/merged.hex
pyocd reset -t nrf91 -m hw -O auto_unlock=false
```

3. Run the credentials installer on the console port, the first of the Connect Kit's two serial ports, with nothing else holding it open. The first time, install nrfcloud-utils and create your device CA

```
uv tool install --python 3.12 nrfcloud-utils
mkdir -p onboarding
create_ca_cert -c GB -o l0destar -p onboarding -f l0destar

device_credentials_installer --cmd-type at     --ca onboarding/*_ca.pem --ca-key onboarding/*_prv.pem     --id-imei --id-str nrf- -d     --csv onboarding/onboard.csv     --port /dev/cu.usbmodemXXXX
```

This will generate a new device key inside the modem, sign a cert with your CA key, store both in modem NVM at the nRF Cloud sec tag, and write a new row into onboard.csv with the Makerdiary board's IMEI.

4. Log into nRFCloud and find your API key - https://app.nrfcloud.com/#/account

Note: if you get redirected to memfault, this is the wrong interface. Click the link to get back to the legacy interface and get your API key from the User Account section.

5. Onboard the device to nRFCloud:

```
nrf_cloud_onboard --api-key <key> --csv onboarding/onboard.csv
```

6. Re-flash the l0destar firmware, which `build/` still holds:

```
./flash.sh
```
