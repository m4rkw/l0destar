# Disclaimer

This applies to everything in this repository — every board, the firmware, the
documentation and the analysis. It is referenced from the individual READMEs
and guides rather than repeated in each of them.

## This is a prototype, not a product

l0destar is a personal project in the prototyping and design phase. Treat every
board here, and the firmware, as unvalidated and unsafe by default.

Nothing in this repository has been through automotive qualification, EMC
testing, or formal ISO 7637-2 or ISO 16750 compliance testing. The analysis
documents are desk work backed by datasheets and manufacturer characterisation
data — they are estimates and reasoning, not test reports, and the reasoning may
be wrong.

## Test results are mine, and unverified

Some of this has been tested on my own bench and some has not. The per-board
READMEs under `hardware/` record what I have checked.

Those results are my own observations, on my own boards, under bench
conditions, with my own instruments. They are not independently verified and
should not be assumed correct. Anyone relying on this work should repeat the
testing themselves rather than taking my results on trust.

## Parts lists are examples, not a verified BOM

The bills of materials and parts lists throughout this repository are
suggestions and examples only. I have built working boards using many of these
parts, but not necessarily from the same vendor, series or batch, and
distributor links go stale.

Check every part against its current datasheet, satisfy yourself about
footprint, tolerance and voltage rating, and buy from a distributor you trust.
Distributor and manufacturer links are for identification only — they are not
endorsements and imply no relationship with those vendors. Where this
repository interprets a manufacturer's datasheet or characterisation data, that
interpretation is mine and may be wrong.

## If you build one, you are responsible for it

That means your own due diligence on part selection, assembly, fusing, wiring
and installation.

A device wired to a vehicle battery can start a fire if it is built or fused
wrong, and anything connected to OBD or CAN can interfere with systems you want
working while the vehicle is moving. The bench boards in this repository are
test equipment rather than trackers, but they still take a 12V supply and
connect to vehicle diagnostic lines, and the same care applies.

This design is not validated for safety-critical or security-critical use. It
may fail silently, and it must not be anyone's only theft-recovery, safety or
emergency measure.

## Firmware and over-the-air updates

Over-the-air updates mean that whoever controls the configured endpoint can
execute code on the device. Point it only at infrastructure you control. No
server software is provided with this project.

## Location data

If you operate one, you are the data controller for the location data it
collects — including where the tracked vehicle is driven by an employee, a
family member or anyone else. Those obligations are yours, as is any consent or
notification the law where you live requires.

## Regulatory approval

Nothing here is CE, UKCA or FCC marked, and no conformity assessment has been
carried out. Running an LTE radio may require type approval where you live.

The licences permit commercial manufacture and sale, but anyone placing a
product on the market becomes the manufacturer of radio equipment and takes on
the resulting obligations — UKCA or CE marking and a Declaration of Conformity
in the UK and EU, FCC equipment authorisation in the US, and UNECE Regulation
10 for vehicle installation. The nRF9151's modular approval does not transfer
to a finished product.

## No warranty, no liability

I make no claim that any of this is suitable for installation in a vehicle or
for any other purpose.

Neither I nor any contributor offers any warranty, express or implied, and to
the fullest extent permitted by law neither I nor any contributor is liable for
any loss or damage arising from its use — including damage to vehicles or other
property, personal injury, financial loss or regulatory penalty.

All responsibility for the safety, legality and fitness for purpose of anything
built from this project rests with the person building, operating and
installing it.

Nothing in this disclaimer excludes or limits liability that cannot lawfully be
excluded or limited, including liability for death or personal injury caused by
negligence.

The formal licence terms, including their warranty and liability provisions,
are in [`LICENSE.md`](LICENSE.md) and [`LICENSES/`](LICENSES/).
