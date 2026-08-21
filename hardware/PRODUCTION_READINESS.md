# l0destar — road to a sellable consumer product

Gap analysis against the current design (v3.1, the latest complete board). This
is the list of things standing between "well-documented prototype that has never
been powered on" and "product you can sell to a member of the public."

Scope note: v3.1's own test matrix is **entirely NOT TESTED** and the README
still carries "USE AT YOUR OWN RISK." Everything below assumes the goal is a
boxed product a non-expert buys and has fitted to their car — not an eval board
for hobbyists. Items are grouped and roughly ordered within each group by how
hard they gate a sale.

---

## 0. Already known (tracked here for completeness)

- [ ] **Uprate the blocking FET.** PROTECTION.md already documents this: `S2Q2`
  (SQ2361ES) is at or slightly past its pulsed rating during ISO 7637-2 pulse 2a
  at the +112 V max level (~45 A peak vs 11 A rated, Tj near the 175 °C limit).
  Documented fix is Vishay **SQJ457EP** (PowerPAK SO-8L, −60 V, 100 A pulsed).
  It's a footprint change → board revision. `S2Q1` carries no bulk-charging
  current and does not need it.
  - **Smaller alternatives to the SO-8L (all datasheet-verified, AEC-Q101):**
    - **Diodes DMP6023LFGQ** — PowerDI3333-8, 3.3×3.3 mm (~⅓ the SO-8L
      footprint). −60 V, Rds(on) 25 mΩ max / 11 mΩ typ, IDM −55 A (10 µs/1 %),
      EAS 62.9 mJ, Tj 150 °C. Lowest Rds → *least* I²R heating in the 45 A
      pulse; cheap (~£0.38). Best size-vs-heat pick. Confirm the graphical
      single-pulse SOA at 45 A/50 µs at BOM sign-off (IDM is spec'd at 10 µs).
    - **Vishay SQS181ELNW** — PowerPAK 1212-8, 3.3×3.3 mm (~⅓ footprint).
      **−80 V**, 31 mΩ max, IDM −85 A (1.9× margin), EAS 46 mJ, Tj 175 °C.
      More current/temperature headroom; pricier (~$2.9). The −80 V is margin,
      not a requirement (bulk caps clamp the rail to ~42 V; the FET is ON,
      Vds≈0.5 V, during pulse 2a).
- [ ] **On-board fuse.** Note this *reverses* a documented decision:
  PROTECTION.md deliberately relies on two external 2 A harness fuses and
  rejects on-board fusing (a blown on-board fuse = board swap, not fuse swap).
  That reasoning holds for a device *you* install. It breaks for a consumer
  product, because you cannot assume the buyer (or their installer) fits inline
  fuses correctly. Options, in order of preference:
  1. Ship a **fused harness / fused lead in the box** so the fuse is not
     optional and is field-replaceable → keeps the documented board-side
     reasoning intact.
  2. Add an on-board resettable + one-shot combination (PTC for nuisance,
     fuse for the reverse-battery-into-TVS fault the PROTECTION.md calls out).
  3. Decide explicitly and write it down, because "settled: external fusing"
     is currently a supported-configuration assumption, not a product spec.

---

## 1. Regulatory & compliance (the true gate — no legal sale without it)

This is a **radio device that plugs into a vehicle's electrical system and
handles location data.** That triggers several independent regimes.

- [ ] **Radio approval (UKCA / CE-RED).** The nRF9151 SiP is pre-certified, but
  module certification is granted against a **reference antenna**. This design
  brings the RF out to **SMA/u.FL for external cellular + GPS antennas**, which
  changes radiated performance and generally **voids the "modular approval" free
  pass**. Budget for RED (EU) / Radio Equipment Regs (UK) assessment of the
  *finished product with its shipped antenna*.
- [ ] **Cellular network certification.** GCF and/or PTCRB, plus any specific
  **MNO/carrier approval** for the network you ship on. Confirm what the Connect
  Kit already carries and what re-testing the external-antenna integration
  forces. (Relevant to the existing 1NCE/DTLS deployment work.)
- [ ] **RF exposure (MPE/SAR).** Required for RED; needs the final antenna and
  mounting/separation distance defined.
- [ ] **Automotive EMC — UNECE R10 ("E-mark").** An aftermarket electronic
  sub-assembly hardwired to the vehicle 12 V system typically needs R10 EMC
  approval for road use in UK/EU. Separate from RED.
- [ ] **EMC directive testing.** Emissions + immunity. You have designed *to*
  ISO 7637-2:2011 pulse levels but **never bench-tested them** — see §3.
  Formal EMC lab time is a line item, not a formality.
- [ ] **RoHS / REACH / WEEE.** Requires a controlled BOM with compliant,
  documented parts — incompatible with the current AliExpress-sourced parts of
  unknown provenance (see §4). WEEE registration + marking. Battery Directive if
  any cell ships in the box.
- [ ] **Declaration of Conformity + technical file.** Consolidated evidence
  pack; legally required to affix UKCA/CE. Someone has to own this document.
- [ ] **Product labelling.** Model ID, IMEI, regulatory marks, WEEE crossed-bin,
  importer/manufacturer details. No provision for this exists yet (no enclosure,
  no label artwork).

## 2. Productise away the dev-kit dependency

- [ ] **The board is built around the Makerdiary nRF9151 Connect Kit** — a
  hobbyist dev kit with its own USB-C, buttons, regulators, and **2.54 mm pin
  headers**. This is fine for a prototype and unacceptable for a shipped product
  for three reasons: (a) **supply** — single hobby-scale source, no guaranteed
  lead time or lifecycle; (b) **cost** — you're paying for a whole dev kit; (c)
  **certification & robustness** — a socketed module on 0.1" headers will not
  survive automotive vibration and complicates the radio approval above.
  - **Decision needed:** either (i) get a written supply/lifecycle commitment
    from Makerdiary and validate the header stack under vibration, or (ii)
    **bring the nRF9151 SiP + PMIC + SIM + antenna matching onto your own PCB.**
    Option (ii) is a large effort (RF layout, matching, own cellular/GPS
    certification) but is the normal path to a real product and removes most of
    §1's "external antenna voids modular approval" pain by letting you design a
    proper front end.
- [ ] **Provisioning story.** How does firmware, DTLS credentials, and network
  identity get onto each unit at manufacture? The dev kit's USB-C is a
  bench convenience, not a production programming interface.

## 3. Design validation (nothing here has been powered on)

- [ ] **Build and bring up first articles.** Every row of the v3.1 test matrix
  is `NOT TESTED`. Power-on, rail-by-rail bring-up, then work the matrix:
  input reverse polarity (both inputs), INA228 read, ignition sense, LT8609
  4.2 V, all three switched aux rails, accelerometer + wake-on-motion, GPS bias
  tee, K-line (K + L), CAN + XSTBY standby current.
- [ ] **Measure quiescent current — this is a product-gating number, not a
  nicety.** Claim is "~120 µA estimated," **unverified.** A hardwired tracker
  that flattens a parked car's battery is a returns-and-liability event. Need
  measured worst-case sleep current including the module's own sleep, across
  temperature, and a **battery-protection undervoltage cutoff** so the device
  stops drawing before it kills the car battery. I don't see a UVLO/low-battery
  disconnect in the design — for a consumer tracker that's arguably mandatory.
- [ ] **Bench-test the transients you designed for.** PROTECTION.md is a strong
  paper analysis (load dump 35 V/400 ms ride-through, pulse 2a soak-up to
  ~37–42 V). None of it is measured. At minimum inject pulse 2a and a suppressed
  load dump before trusting the FET/cap sizing — especially given the FET is
  admittedly at-the-limit (§0).
- [ ] **Full automotive temperature range.** −40 to +85 °C. Several parts are
  commercial/industrial grade: X7T bulk caps, some 5 % commercial resistors, the
  **ITS4060 OBD switch is industrial (not AEC-Q) and already sits above its
  functional range during load dump**, and the AliExpress parts are ungraded.
  Validate the assembled board over the full range, not just the parts on paper.
- [ ] **Vibration + mechanical shock + thermal cycling** to an automotive
  profile. The SMA connectors, the socketed module, and the solder-jumper
  interface selection are all vibration risks.
- [ ] **HALT / burn-in** on a small batch before committing tooling.

## 4. BOM: sourcing, grading, traceability

- [ ] **Replace AliExpress-sourced silicon** (INA228, LM66100, PESD5V0X1BL,
  and the module itself) with **authorized-distributor parts.** Counterfeit and
  provenance risk is disqualifying for a product and for RoHS/REACH evidence.
- [ ] **Upgrade grade where the environment demands it.** The docs already note
  the AEC-Q200 CKG57N (JJ suffix) and flag the ITS4060 as non-AEC — decide,
  per part, whether automotive grading matters and lock it.
- [ ] **Second-source the single-source and hobby parts** (module above; any
  part with one link). Lifecycle/EOL check on everything.
- [ ] **Freeze a controlled, versioned production BOM** with MPNs, approved
  manufacturers, and no "example / I generally buy tighter" ambiguity. The
  current README explicitly ships loose tolerances ("indicated spec is the
  minimum") — fine for a hand build, not for a repeatable product.

## 5. Manufacturing / DFM / test

- [ ] **DFM review with a real assembly house.** Current board is
  "hand-solderable, hot air required," with **deliberately dogleg-routed vias to
  cut PCB cost** and 0402 passives. Validate against a production fab/assembler's
  actual capability; add fiducials, panelization, stencil/reflow profile.
- [ ] **Design a functional test fixture + firmware test jig** for end-of-line
  test (rails, current draw, RF, sensors, OBD interfaces). At volume you cannot
  hand-verify the v3.1 matrix per unit.
- [ ] **Production programming & calibration** — firmware flash, INA228
  calibration if you want accurate voltage/current, serialization, and
  identity/credential provisioning, all at end of line.
- [ ] **Serialization & traceability** (lot/date, board serial ↔ IMEI ↔ SIM).
- [ ] **Lock the CAN-vs-K-line configuration.** The solder-jumper scheme
  (`S5R1..S5R7`, "critical that only one is enabled") is a **field-error hazard**
  in consumer hands. It must be factory-set per SKU, not left as user-solderable
  pads, or replaced with a design that can't be mis-strapped.

## 6. Mechanical / enclosure / install

- [ ] **There is no enclosure.** A consumer product needs one: IP-rated for its
  install location (under-dash vs harsher), thermal path, mounting, strain
  relief on the harness, and defined antenna placement/mounting.
- [ ] **Ship a defined, certified antenna** (cellular + GPS) with cable and
  mounting instructions. The RF approval in §1 is against *that* antenna.
- [ ] **Harness/cable assembly** designed and validated (the Micro-fit / MX1.25
  leads), including the fused-lead option from §0.
- [ ] **Connector retention & vibration** for the module headers and SMA — or
  eliminate them by going to an integrated design (§2).

## 7. Firmware, security, data (gates a *connected consumer* product)

- [ ] **Secure boot + signed firmware + secure FOTA.** There is a `fota.c` in
  the firmware tree already — good — but confirm image signing, rollback
  protection, and secure credential storage are in place before shipping.
- [ ] **Privacy / lawful-use.** This is a location tracker handling personal
  location data → GDPR/UK-GDPR obligations, plus clear anti-stalking / lawful-use
  positioning. This is a commercial/legal gate on selling to the public, even
  though it's not a PCB change.

## 8. Commercial / support wrap

- [ ] **Installation instructions** for the intended installer (professional-fit
  only vs DIY changes your liability and your fusing decision in §0).
- [ ] **Warnings, warranty, support process, RMA path.**
- [ ] **Remove the "USE AT YOUR OWN RISK / not a finished product" framing** from
  shipped docs — which is only honest once the items above are actually closed.

---

### Shortlist: what actually blocks a first sale

1. **Regulatory approval** (§1) — UKCA/CE-RED, R10, EMC, network cert. Long lead,
   external dependency, and the external-antenna choice likely voids the module's
   free pass. Start scoping this first; it drives §2.
2. **Get the board powered and the transient/quiescent claims measured** (§3) —
   everything downstream assumes the design works, and it has never run.
3. **Decide the module-vs-integrated question** (§2) — it's the biggest fork in
   the road and it changes cost, cert, and robustness for everything else.
4. **Battery-drain cutoff** (§3) — a tracker that flattens the customer's car
   battery is a product-killer; there's no UVLO today.
5. Then the known two: **FET uprate + fusing decision** (§0).
