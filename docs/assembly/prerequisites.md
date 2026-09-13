# Assembly prerequisites

What you need before you start building a board: the skills, the tools, the design files, a PCB
and the parts. Which parts to buy - and which ones not to substitute - is covered on the
[component selection](/assembly/component-selection.html) page.

## Skills

The board is designed to be hand-assembled at home with relatively inexpensive tools, but it is
surface mount throughout and needs hot air:

- the smallest passives are 0402;
- the finest-pitch parts are the LT8609A buck converter (MSOP-10 with an exposed pad) and the
  INA228 (VSSOP-10), where bridged pins are likely and need removing with wick;
- some parts have no exposed leads at all: the ASM330LHHX accelerometer is a 14-pad LGA and the
  antenna ESD diodes are X1SON-2 packages.

You also need to be comfortable finding a designator on the KiCad PCB and taking resistance,
continuity and voltage readings with a multimeter. If you have not soldered 0402 parts with hot
air before, practise on a scrap board first.

## Tools

These are the tools the author uses; the links are examples, not endorsements.

| Tool | Notes |
|---|---|
| Hot air rework station | e.g. [Yihua 858D](https://link.amazon/B0bN4ESzl) |
| Soldering iron | e.g. [Yihua 982](https://link.amazon/B0gqapZfV) |
| Solder paste | type 4 leaded paste is recommended, e.g. [Wonderway T4](https://link.amazon/B0hetTNxm) |
| Desoldering wick | [MG #444](https://link.amazon/B0c8qzNTj) recommended |
| Fume extraction | the author uses an [Aoyue 486+](https://link.amazon/B0alCSAYb), with a desk fan blowing the fumes towards it when using the iron |
| Bench power supply | the author uses a [Riden RD6006](https://parallaxdigital.co.uk/shop/power_supplies/bench_power_supplies/rd6006/); see below |
| Multimeter | [Fluke 117](https://link.amazon/B0g4okXV6) recommended, but any cheap meter will do |
| Tweezers | [Erem E5SA](https://link.amazon/B0aGIsvtk) recommended |
| Magnification | e.g. [USOR 15x](https://link.amazon/B0isEdLNz); a microscope is much better but costs a lot more |
| Cleaning | [isopropyl alcohol](https://link.amazon/B00urqQmb) and [foam swabs](https://link.amazon/B02K5TwZw) |

A generic 12V adapter with screw terminals, such as a
[SoulBay 30W](https://link.amazon/B0gLo8L0l), will power the board, but an adjustable bench
supply with a current limit is strongly recommended: the first power-up is done with the current
limited to 50mA so a short shows up before it does damage, and one step of the firmware board
test needs the supply voltage raised and lowered by 1V.

You will also want some tape to hold the pin headers in place while soldering them, and if you
print the enclosure, a soldering iron tip for heat-set inserts (see
[component selection](/assembly/component-selection.html#enclosure-hardware)).

Before you start:

- **Buy more components than you need.** The smallest passives are 0402 and it is very easy to
  lose them or have them snap out of the tweezers.
- **Do not skimp on ventilation and fume extraction.** Solder and flux fumes can be very toxic
  and cause permanent damage.
- **Open the PCB in KiCad** so you can look up each part as you go.
- **Decide up front whether the board is CAN, K-wire or neither.** The interface parts are
  optional and there is no point placing the ones you will not use.

## Design files

Clone the repository:

```sh
git clone https://github.com/m4rkw/l0destar.git
```

The v3.4 board lives in
[`hardware/l0destar_v3.4/`](https://github.com/m4rkw/l0destar/tree/master/hardware/l0destar_v3.4):

- `l0destar.kicad_pro`, `l0destar.kicad_pcb` and one schematic sheet per subsystem;
- `libs/`, the project-local symbols, footprints and 3D models the design needs;
- `README.md`, with the bill of materials, the interface pad table, design notes and the test
  table;
- `PROTECTION.md`, the reasoning behind the 12V input protection.

The v3.4 files were saved with KiCad 10.0, so open them with KiCad 10 or later.

The enclosure is in
[`hardware/enclosure/v3.3/`](https://github.com/m4rkw/l0destar/tree/master/hardware/enclosure/v3.3):
FreeCAD sources plus STL and STEP exports of the top and bottom, with and without the l0destar
logo.

## Ordering the PCB

| Property | Value |
|---|---|
| Layers | 4 |
| Thickness | 1.6mm |
| Outline | 66.65 x 37.55mm |
| Mounting holes | 4 x M2 (2.2mm) |

- No fabrication outputs are committed for v3.4. Generate the Gerber and drill files from the
  KiCad project in the format your board house asks for.
- The RF trace widths are calculated for JLCPCB's **JLC04161H-7628** 4-layer stack-up. A
  different stack-up or fabrication process needs the widths recalculated, or the antenna feeds
  will not be the intended 50 ohms.
- Component vias are dogleg-routed deliberately, so the board needs no special via processing
  and stays cheap to make.
- Order a few spare boards. Mistakes happen, and the extra boards cost very little.

## Parts

- One [Makerdiary nRF9151 Connect Kit](https://makerdiary.com/products/nrf9151-connectkit) per
  board. It provides the modem, GNSS receiver and processor. l0destar is not affiliated with
  Makerdiary and does not sell the Connect Kit.
- The parts in the
  [v3.4 bill of materials](https://github.com/m4rkw/l0destar/blob/master/hardware/l0destar_v3.4/README.md)
  for the build you chose - see [component selection](/assembly/component-selection.html).
- Two 35mm u.FL to u.FL cables and one ultra-thin MX1.25 two-pin power lead, both in the bill of
  materials. The cables link the board's u.FL connectors to the Connect Kit's antenna
  connectors; the lead takes the 4.2V rail to the Connect Kit's battery connector.
- If you are printing the enclosure: M2 heat-set inserts and M2 x 10mm screws.

An antenna, a SIM card and the firmware tooling are only needed once the board is built; they are
listed under [Board setup prerequisites](/board-setup/prerequisites.html).

## For the board test

- The current-limited bench supply and multimeter from above.
- A way to feed the board through its 6-pin Molex Micro-Fit 3.0 connector: a lead made up with
  the mating 43025-0600 housing, or temporary wires. Pin 2 is ground, pin 4 is the permanent 12V
  and pin 5 is the ignition input - see the
  [vehicle connector pinout](/reference/hardware.html#vehicle-connector-pinout). A switch in the
  ignition wire is needed for the firmware test.
- For the interactive firmware test only: a USB-C data cable, a SIM card and antenna, and the
  toolchain described in [Board setup prerequisites](/board-setup/prerequisites.html).
