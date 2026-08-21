# License

l0destar is a multi-artifact open-source project. Each artifact type is licensed under a widely-used permissive open-source license:

| Artifact | License | SPDX identifier |
|---|---|---|
| Hardware design files (KiCad schematics, PCB layouts, BOM, gerbers, fabrication outputs) | CERN Open Hardware Licence v2 — Permissive | `CERN-OHL-P-2.0` |
| Firmware (embedded code running on the tracker) | Apache License 2.0 | `Apache-2.0` |
| Server-side software (backend, ingestion pipeline, web UI) | Apache License 2.0 | `Apache-2.0` |
| Documentation, enclosure CAD, images, media | Creative Commons Attribution 4.0 International | `CC-BY-4.0` |

Full license texts live in the [`LICENSES/`](LICENSES/) directory, named by SPDX identifier per the [REUSE specification](https://reuse.software/).

## Why these licenses?

l0destar is a hobby project and a labour of love, not a commercial operation. The goal is for the design to spread as widely as possible — to be built, modified, embedded, manufactured, hosted, and sold by anyone who finds it useful, with as little legal friction as possible.

All four licenses are **permissive**: you may use, modify, fabricate, host, embed, and sell derivatives — commercial or otherwise — and you are not obligated to publish your changes. The only baseline requirements are attribution and preservation of the licence notice.

- `Apache-2.0` (firmware and server) includes an explicit patent grant, which is especially valuable for hardware-adjacent software where patent ambiguity is common. Strongly preferred over MIT for any code that touches a physical device.
- `CERN-OHL-P-2.0` is the permissive variant of the CERN Open Hardware Licence — purpose-built for hardware design files. It handles manufacturing rights, documentation, and notice requirements in a way generic software licences do not.
- `CC-BY-4.0` keeps documentation and enclosure CAD attribution-only, dropping share-alike obligations so the material can be freely incorporated into derivative works and other documentation.

## Warranty and liability

All four licences provide the material "as is", disclaim warranties and limit liability. The operative text is in the licence files:

| Licence | Disclaimer of warranty | Limitation of liability |
|---|---|---|
| `Apache-2.0` | Section 7 | Section 8 |
| `CERN-OHL-P-2.0` | Section 5.1 | Section 5.2 |
| `CC-BY-4.0` | Section 5(a) | Section 5(b) |

In summary: there is no warranty of any kind, express or implied, including as to merchantability, satisfactory quality, fitness for a particular purpose or non-infringement, and neither the licensor nor any contributor is liable for any damages arising from use of the material. The entire risk as to quality and performance rests with you.

Nothing in these licences, or in the [project disclaimer](DISCLAIMER.md), excludes or limits liability that cannot lawfully be excluded or limited, including liability for death or personal injury caused by negligence.

This project is hardware intended to be wired into vehicles, and radio equipment that must be authorised before it is placed on any market. The permissive licences above grant you the right to manufacture and sell derivatives; they do not discharge the regulatory obligations that come with doing so. See the [project disclaimer](DISCLAIMER.md).

## SPDX headers

Every source file should declare its license using an SPDX header. Examples:

    // SPDX-License-Identifier: Apache-2.0
    // SPDX-FileCopyrightText: 2026 Mark Wadham

    # SPDX-License-Identifier: Apache-2.0
    # SPDX-FileCopyrightText: 2026 Mark Wadham

    <!-- SPDX-License-Identifier: CC-BY-4.0 -->
    <!-- SPDX-FileCopyrightText: 2026 Mark Wadham -->

## Copyright

Copyright (C) 2026 Mark Wadham and l0destar contributors.

## Contributing

l0destar does not require a Contributor License Agreement. Contributions are accepted under the [Developer Certificate of Origin](https://developercertificate.org/) — every commit must carry a `Signed-off-by:` trailer asserting that you have the right to submit the work under the project's licences. See [`CONTRIBUTING.md`](CONTRIBUTING.md) for the full process.
