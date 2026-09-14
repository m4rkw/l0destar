# Deployment prerequisites

Everything to have ready before you start work on the vehicle.

## A tracker that is ready to fit

- The board is assembled, has passed the [board test](/assembly/board-test.html) and is in its enclosure.
- The Connect Kit's [interface firmware has been updated](/board-setup/makerdiary-firmware.html). Without the update, unplugging a USB cable leaves the interface MCU drawing about 2mA, which matters if you plan to plug USB in while the tracker is in the vehicle.
- The tracker is [enrolled on your server](/board-setup/server-onboarding.html) and its [telemetry has been verified](/board-setup/verifying-telemetry.html) on the bench.
- It runs production firmware: the device has its own section in `remote.conf` and has already taken at least one update over the air from `push_fw.sh` (see [OTA updates](/board-setup/ota-updates.html)). That proves the update path through your firewall before the unit is hidden behind a panel. A production image must not contain `CONFIG_APP_FOTA_INHIBIT=y` or any bench overrides - see [Deployment configuration](/deployment/configuration.html).
- The SIM is active, supports LTE-M, and has LTE-M coverage where the vehicle is kept and driven. The firmware does not use NB-IoT.
- A notification backend is configured on the server and delivers alerts (see [Configure alerts](/deployment/alerts.html)).

## Antenna

A combined LTE and active GNSS antenna with two SMA plugs, as described in [Antenna](/reference/hardware.html#antenna). Check that its cable reaches from where you intend to put the antenna to where you intend to put the tracker.

## Wiring harness

The tracker connects to the vehicle through a 6-way Molex Micro-Fit 3.0 connector. The pin assignments are in [Vehicle connector pinout](/reference/hardware.html#vehicle-connector-pinout): pin 2 ground, pin 4 permanent 12V, pin 5 ignition 12V, and pins 3 and 6 for the optional bus lines.

| Item | Notes |
|---|---|
| Micro-Fit 3.0 receptacle housing, 2 x 3 (Molex 43025-0600) | Mates with the 43045-0600 header on the board |
| Micro-Fit 3.0 female crimp terminals (Molex 43030 series) | Choose the terminal for your wire size and buy spares |
| Crimp tool for Micro-Fit 3.0 terminals | Or buy pre-crimped Micro-Fit 3.0 leads and skip crimping |
| Automotive wire, about 0.5mm² (20 AWG) | Check it is within the terminal's wire range; use different colours for permanent, ignition and ground |
| Two inline fuse holders and two 2A fuses | One for the permanent feed, one for the ignition feed |
| Fuse taps (add-a-circuit), if taking power from a fuse box | They must match the type of fuse the vehicle uses |
| Ring terminal | For the ground connection, sized for the stud or bolt you use |
| Heat shrink, loom tape or braided sleeve, cable ties | Insulation, protection and strain relief |

The tracker's current is small - roughly 15-45mA from 12V while it reports, going by Nordic's figures for the modem, and 15-25mA on the author's bench supply - so the wire size is set by the terminals and by mechanical strength rather than by current. The 2A fuses protect the wiring.

## Diagnostic connection (optional)

Only needed for a board built with the K-wire interface, if you want engine data and fault codes (see [Deployment configuration](/deployment/configuration.html)). Firmware 0.4.x reads nothing over CAN.

- An OBD-II (SAE J1962) plug with flying leads, or a pass-through splitter so the vehicle's socket stays free for a scan tool.
- The standard socket pins are 4 chassis ground, 5 signal ground, 6 CAN high, 7 K line, 14 CAN low, 15 L line and 16 battery positive. Not every vehicle populates every pin: look inside the socket, and measure before relying on one.

## Tools

- A multimeter. Use it, rather than a test lamp, on circuits near control units.
- Plastic trim tools and whatever is needed for the vehicle's panels and fixings.
- Wire strippers, the crimp tool and a heat gun for the heat shrink.
- A laptop with the firmware toolchain (see [Board setup prerequisites](/board-setup/prerequisites.html)) and a USB-C data cable, to watch the console or reflash the tracker in the vehicle - for example for K-wire discovery.
- A torch.

## Optional extras

- A USB-C extension with a panel-mount socket, to bring the Connect Kit's USB port somewhere you can reach it.
- A normally closed push-to-break switch, to power-cycle the tracker without reaching it.
- Foam or felt tape, to stop the enclosure rattling.

The first two are covered in [Mounting recommendations](/deployment/mounting.html).

## Vehicle information

- The fuse box map from the owner's manual, and a wiring diagram if you can get one.
- Where the vehicle's airbags are.
- The manufacturer's procedure for disconnecting the battery, if you intend to disconnect it.
