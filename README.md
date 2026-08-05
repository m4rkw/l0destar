# l0destar

A fully open-source vehicle tracker — open hardware and open software.

Inspired by the [Geolink Opentracker](https://github.com/geolink/opentracker/) and the [Fortebit Polaris](https://www.fortebit.tech/polaris/), the goal is to build something that will last: a reference design for the best possible vehicle tracker that anyone can fab, modify, and host. Hardware and software are completely open and maintained as a community project.

## Status

Prototyping / design phase. The v2.1 board layout worked on a bench, testing of newer versions is pending and should begin soon.

## What it is

- **Mission** — the world's best fully open-source vehicle tracker, no profit motive. See [`MISSION.md`](MISSION.md).
- **Goals** — KiCad reference design, open enclosure CAD, open firmware and server, weeks of standby on a typical car battery, 1 Hz GNSS, LTE-M/NB-IoT, CAN + K-Line diagnostics, accelerometer-driven wake. Full list in [`GOALS.md`](GOALS.md).
- **Technology choices** — Nordic nRF9151 SiP (LTE-M/NB-IoT + GNSS), nPM1300 PMIC, LT8609S pre-regulator, ASM330LHHX-Q1 IMU, TCAN4550-Q1 CAN, L9637D K-Line. Rationale and trade-offs in [`TECHNOLOGY.md`](TECHNOLOGY.md).

## Repository layout

To be populated as artifacts land. Planned top-level directories:

- `hardware/` — KiCad schematics, PCB layouts, BOM, fabrication outputs
- `enclosure/` — 3D-printable / fabbable case designs
- `firmware/` — embedded code for the nRF9151
- `server/` — backend, ingestion pipeline, web UI
- `docs/` — extended documentation beyond the top-level `*.md` files

## License

l0destar is multi-licensed by artifact type — permissive across the board:

| Artifact | License |
|---|---|
| Hardware design files | `CERN-OHL-P-2.0` |
| Firmware | `Apache-2.0` |
| Server-side software | `Apache-2.0` |
| Documentation, enclosure CAD, media | `CC-BY-4.0` |

Full text and rationale in [`LICENSE.md`](LICENSE.md); license bodies in [`LICENSES/`](LICENSES/).

## Contributing

Contributions are welcome. l0destar uses the [Developer Certificate of Origin](https://developercertificate.org/) rather than a CLA — every commit must carry a `Signed-off-by:` trailer (`git commit -s`). See [`CONTRIBUTING.md`](CONTRIBUTING.md) for PR expectations.

## Contact

- Bug reports and feature ideas: [GitHub Issues](https://github.com/m4rkw/l0destar/issues)
- Open-ended questions and design discussion: [GitHub Discussions](https://github.com/m4rkw/l0destar/discussions)
- Direct contact: [contact me](https://l0destar.com/contact.html)
