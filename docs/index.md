# l0destar documentation

l0destar is a fully open-source vehicle tracker: a hand-assemblable carrier board for the
Makerdiary nRF9151 Connect Kit (LTE-M and GNSS), firmware built on Zephyr, and a tracking server
you host yourself. These pages take you from a bare PCB to a tracker that reports from a vehicle
to your own server and alerts you when something happens.

!!! danger "A prototype, not a product"
    Nothing here is validated, certified or finished, and anything you build, install and
    operate from it is your responsibility. Start with
    [Assembly: read this first](/assembly/read-this-first.html) and read the full
    [project disclaimer](https://github.com/m4rkw/l0destar/blob/master/DISCLAIMER.md) before you
    order parts.

## How the docs are organised

Work through the sections in order. The server comes before board setup because a board needs
somewhere to report to while you bring it up on the bench.

| Section | What it covers |
|---|---|
| [Assembly](/assembly/read-this-first.html) | Choosing parts, building the PCB and testing it before and after the Connect Kit goes on |
| [Server](/server/installation.html) | Installing the tracking server, configuring it, running it as a service and exposing only what has to be exposed |
| [Board setup](/board-setup/prerequisites.html) | The toolchain, firmware, enrolling the board with your server and checking its telemetry on the bench |
| [Deployment](/deployment/read-this-first.html) | Installing the tracker in a vehicle, its production configuration, a test drive and alerts |
| [Configuration reference](/reference/hardware.html) | Every hardware option, firmware build setting, device command and server setting |

## What you end up with

- A tracker expected to sleep at about 35.5µA from the vehicle battery that wakes on the ignition,
  movement, an impact or a slow tilt such as a tow or a jack.
- Position, speed, battery voltage and ignition state every few seconds while driving (3-5
  seconds on LTE-M in the author's vehicle), and on a timer while parked if you want it.
- An optional vehicle diagnostic interface, CAN-FD or K-wire. Over the K wire the firmware reads
  engine data and fault codes; the CAN-FD interface has been bench tested from 125kbps up to 8Mbps,
  but the firmware does not read vehicle data over CAN yet.
- Firmware that updates itself over the air, with a separate signed image for every device.
- A server that stores the history, splits it into journeys, shows a live map and journey
  replays, sends alerts through Pushover or a webhook, and handles several vehicles.

## How the pieces fit together

- The tracker sends its records over LTE-M to UDP port 65480 on your server. Every datagram is
  encrypted and authenticated with a key unique to that device.
- The server stores the records in MySQL or MariaDB, relays alerts, and answers each datagram
  with the device's settings, any commands you have queued and news of a firmware update.
- A device fetches a published update over TLS on TCP port 65481, checks it was built for its own
  board and installs it; the bootloader rolls back an image that does not start properly.
- You use the map and history in a browser, ideally reachable only over your Tailscale network,
  and automate through an HTTP API authenticated with tokens.

## Project status

- **Board:** v3.4 is the current design and has not been built or tested yet. v3.3, which differs
  only in details covered in the assembly section, is built and bench tested. The guides assume
  v3.4.
- **Firmware:** 0.4.x, built with nRF Connect SDK v3.3.0. OBD-II data and fault codes are read over
  the K wire. The CAN-FD interface is supported as hardware and bench tested in classic CAN and
  CAN-FD modes, but the firmware does not read vehicle data over CAN yet.
- **Server:** a public reference implementation derived from the author's private deployment. It
  has tests but has not been run end to end against real hardware in this form, so treat it as
  something to review and adapt.

These pages describe the [l0destar repository](https://github.com/m4rkw/l0destar) as of September
2026. Where they disagree with the repository, the repository is right - please report it.

## Getting help

- Bugs and feature ideas: [GitHub issues](https://github.com/m4rkw/l0destar/issues)
- Questions and design discussion: [GitHub discussions](https://github.com/m4rkw/l0destar/discussions)
  or the [forum](https://forum.l0destar.com)
- Background, videos and the build blog: [l0destar.com](https://l0destar.com/)

## Licences

l0destar is licensed by artifact type: the hardware design files under CERN-OHL-P-2.0, the firmware
and server under Apache-2.0, and the documentation, enclosure CAD and media under CC-BY-4.0. See
[LICENSE.md](https://github.com/m4rkw/l0destar/blob/master/LICENSE.md).
