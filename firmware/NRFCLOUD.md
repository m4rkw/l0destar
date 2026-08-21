# nRF Cloud

**Before building, installing or relying on any of this, read the [project disclaimer](../DISCLAIMER.md).**

## How to set up GPS assist

1. Build the firmware in provisioning mode

```
PROV=1 ./build.sh pristine
```

2. Flash it to the l0destar board

```
./flash-md.sh
```

3. Run the credentials installer

```
pip install nrfcloud-utils
device_credentials_installer --cmd-type at     --ca onboarding/*_ca.pem --ca-key onboarding/*_prv.pem     --id-imei --id-str nrf- -d     --csv onboarding/onboard.csv     --port /dev/cu.usbmodem11301
```

This will generate a new device key inside the modem, sign a cert with your CA key, store both in modem NVM at the nRF Cloud sec tag, and write a new row into onboard.csv with the Makerdiary board's IMEI.

4. Log into nRFCloud and find your API key - https://app.nrfcloud.com/#/account

Note: if you get redirected to memfault, this is the wrong interface. Click the link to get back to the legacy interface and get your API key from the User Account section.

5. Onboard the device to nRFCloud:

```
nrf_cloud_onboard --api-key <key>
--csv onboarding/onboard.csv
```

6. Rebuild and re-flash the l0destar firmware:

```
./build.sh pristine
./flash-md.sh
```
