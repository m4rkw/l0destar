# l0destar

A fully open-source vehicle tracker - open hardware and open software.

Inspired by the [Geolink Opentracker](https://github.com/geolink/opentracker/) and the [Fortebit Polaris](https://www.fortebit.tech/polaris/), the goal is to build something that will last: a reference design for the best possible vehicle tracker that anyone can fab, modify, and host. Hardware and software are completely open and maintained as a community project.

## Status

Prototyping / design phase. The v2.1 layout worked on a bench, and v2.6 (CAN,
K-line and micro), v3.0 and v3.1 have since been built and tested working,
with every recorded bench test passing - see the test status table in each
board's README. v3.2 is designed but untested, pending arrival of the first
batch of boards and v3.3 is an initial draft and may change slightly before
the boards are ordered.

<strong>All boards before v3.3 have a serious defect in the ISO-9141 circuit
if the L wire is connected. If any of these prototypes are built the L wire
should not be connected to the vehicle as if it was ever externally shorted
to the 12V rail the l0destar device would be damaged.</strong> Luckily very
few people are going to care at all about this ancient protocol and even fewer
will ever need the L wire in normal use but it still bears mentioning as an
indication that these are very much prototypes and there may be other
undiscovered defects.

## Disclaimer

This is a prototype, not a product. Nothing here is validated, qualified or certified, the parts lists are examples rather than a verified BOM, and any test results are my own unverified bench observations. All responsibility for anything built from it rests with the person building and installing it.

**Before building, installing or relying on any of this, read the [full disclaimer](DISCLAIMER.md).**

## What it is

- **Mission** - the world's best fully open-source vehicle tracker, no profit motive. See [`MISSION.md`](MISSION.md).
- **Goals** - KiCad reference design, open enclosure CAD, open firmware and server, weeks of standby on a typical car battery, 1 Hz GNSS, LTE-M/NB-IoT, CAN + K-Line diagnostics, accelerometer-driven wake. Full list in [`GOALS.md`](GOALS.md).
- **Technology choices** - Nordic nRF9151 SiP (LTE-M/NB-IoT + GNSS), LT8609A buck, ASM330LHHX IMU, MCP2518FD + MAX33041 CAN, TJA1027T K-Line. Rationale and trade-offs in [`TECHNOLOGY.md`](TECHNOLOGY.md).

## License

l0destar is multi-licensed by artifact type - permissive across the board:

| Artifact | License |
|---|---|
| Hardware design files | `CERN-OHL-P-2.0` |
| Firmware | `Apache-2.0` |
| Server-side software | `Apache-2.0` |
| Documentation, enclosure CAD, media | `CC-BY-4.0` |

Full text and rationale in [`LICENSE.md`](LICENSE.md); license bodies in [`LICENSES/`](LICENSES/).

## Contributing

Contributions are welcome. l0destar uses the [Developer Certificate of Origin](https://developercertificate.org/) rather than a CLA - every commit must carry a `Signed-off-by:` trailer (`git commit -s`). See [`CONTRIBUTING.md`](CONTRIBUTING.md) for PR expectations.

## Contact

- Bug reports and feature ideas: [GitHub Issues](https://github.com/m4rkw/l0destar/issues)
- Open-ended questions and design discussion: [GitHub Discussions](https://github.com/m4rkw/l0destar/discussions)
- Direct contact: [contact me](https://l0destar.com/contact.html)
