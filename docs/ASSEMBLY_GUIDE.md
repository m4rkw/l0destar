# l0destar assembly guide

**Before building, installing or relying on any of this, read the [project disclaimer](../DISCLAIMER.md).**

This document describes how to assemble a l0destar tracker PCB.

At some point I will probably make a video but this guide will have to do for
now.

## Disclaimer

This is a hobby project, not a certified product with a company behind it. You
are solely responsible for ensuring the safe and compliant operation of anything
you build with this project as a reference.

**It is absolutely essential to use external fusing, 2A recommended on both the
12V line and the ignition line**

24V vehicles are not supported.

This document assumes a l0destar v3.4 PCB. See the
[v3.4 README](../hardware/l0destar_v3.4/README.md) for the parts lists, jumper
table and test status, and [PROTECTION.md](../hardware/l0destar_v3.4/PROTECTION.md)
for details of the protection features. Earlier v3.x boards are close enough
that most of this still applies, but the test points and the reference
designators for the OVP stage differ.

## Required tools

- Hot air rework station, eg [Yihua 858D](https://link.amazon/B0bN4ESzl)
- Soldering iron, eg [Yihua 982](https://link.amazon/B0gqapZfV)
- Solder paste, I recommend type 4 leaded solder paste such as [Wonderway T4](https://link.amazon/B0hetTNxm)
- Desoldering wick, I recommend [MG #444](https://link.amazon/B0c8qzNTj)
- Fume extraction, I use an [Aoyue 486+](https://link.amazon/B0alCSAYb) and turn a desk fan on at the same time
  when using the iron to blow the fumes towards it
- Bench power supply, I use a [Riden
  RD6006](https://parallaxdigital.co.uk/shop/power_supplies/bench_power_supplies/rd6006/).
  If you don't want to spend the money a generic 12V adapter that has terminals
  like this [SoulBay 30W](https://link.amazon/B0gLo8L0l) will do
- DMM, I recommend [Fluke 117](https://link.amazon/B0g4okXV6) but any cheap meter will do
- Tweezers, I recommend [Erem E5SA](https://link.amazon/B0aGIsvtk)
- Magnification, I recommend [USOR 15x](https://link.amazon/B0isEdLNz),
  obviously a microscope like an Amscope would be way better but a lot more
  expensive
- [Isopropyl alcohol](https://link.amazon/B00urqQmb)
- [Foam swabs](https://link.amazon/B02K5TwZw)

## Suggestions

- Buy more components than you need. The smallest passives on v3.4 are 0402
  and it's very easy to lose them or have them snap out of tweezers
- Don't scrimp on ventilation / fume extraction, solder and flux fumes can be
  very toxic and cause permanent damage
- Open the PCB in Kicad before starting so you can reference each part as you go
- Decide up front whether the board is CAN, K-wire or neither. The OBD parts
  are optional and there's no point placing the ones you won't use

## Board layout

Orientation below assumes the Molex connector S1J1 is on the left edge and the
SMA connectors are on the right.

- **Top left**, next to the Molex: the four configuration pads S5R1-S5R4,
  marked `CAN` and `K` on the silkscreen. These select which OBD interface the
  two bus pins of the Molex are routed to
- **Bottom left**, beside the 20-pin header: the OVP bypass pad S5R5, the
  2-pin Connect Kit power header S1J4 (marked `+` and `-`) and test points
  S12TP1 (`GND`) and S12TP2 (`4.2V`, the protected rail that feeds the Connect
  Kit)
- **Top right**: test points S12TP3 (`3.3V`), S12TP4 (`3.3V GPS`) and S12TP5
  (`3.3V CAN`)
- **Right, between the SMA connectors**: test points S12TP6 (`3.3V K`) and
  S12TP7 (`12V K`)
- **Right edge**: the GPS (top) and LTE (bottom) u.FL and SMA connectors
- The two 20-pin Connect Kit headers run horizontally across the middle and
  bottom of the board

## General soldering process

There are multiple ways to solder SMD components but this what I've found to be
the easiest. Don't bother with flux, it doesn't help much and just makes the
board end up very sticky and difficult to clean.

1. Apply a very thin layer of solder paste to the pads. However much you think
you need, the real amount you probably need is less but it doesn't matter too
much if you get it wrong.

2. Place the components with tweezers. Don't worry about getting the alignment
perfect, it doesn't really matter at this point.

3. Apply heat, the components will self-align into place. If they don't line up
properly you can adjust them with tweezers.

4. For fine pitch components like the LT8609 and INA228 you might end up with
bridged pins. If this happens simply apply heat with a soldering iron tip
against some MG #444 wick very briefly - just a second or two - to remove the
excess.

## Build sequence

1. Place the buck converter components (the S6 designators) first. It's
important that this works and regulates its output correctly, a fault here can
cause the full input voltage to be output which would destroy the MCU board.
Flow these into place. Don't place the over-voltage protection stage (the S11
designators, including the LM66100 ideal diode S11U1) yet as this can also be
damaged by high voltage.

2. Solder temporary wires to a ground point (S12TP1 is convenient) and to the
PP12VP net, for example the non-ground pad of the buck input cap S6C3. Apply 12V
and measure the buck output on the PP4V2_UNPROTECTED net at the non-ground pad of
the output cap S6C5. It should be 4.2V. If
it's 0V or 12V, check for shorts or bad connections before continuing. The
protected rail test point S12TP2 will read 0V at this stage because the OVP
stage isn't populated yet.

   **Optional - disabling UVLO for lower sleep current.** The LT8609 enable
   divider (S6R4 from PP12VP to EN/UV, S6R5 to ground, S6R6 hysteresis from
   the output) sets the ~4.3V shut-off / ~5.6V restart thresholds added in
   v3.3 and draws around 9 µA continuously. If you don't need the clean
   under-voltage cut-off you can fit a 0R in place of S6R4 and omit S6R5 and
   S6R6 entirely, which ties EN/UV straight to the 12V rail and drops the
   sleep current to around ~31.5 µA. The buck then runs down to its own
   internal minimum input voltage instead of shutting off cleanly.

3. Apply paste to the remaining pads, place the rest of the components and
solder them. If you're not building the CAN or K-wire interface leave those
parts off entirely, see the parts lists in the README for which designators
belong to each.

4. Solder the configuration pads in the top left corner according to the
configuration of the board, this can be either CAN, K-wire (K-line) or no OBD
comms:

   | Interface | S5R1 | S5R2 | S5R3 | S5R4 |
   |-----------|------|------|------|------|
   | None      | OPEN | OPEN | OPEN | OPEN |
   | CAN bus   | CONNECT | OPEN | CONNECT | OPEN |
   | K-line    | OPEN | CONNECT | OPEN | CONNECT |

   Bridge them with solder or a 0R 0402. **Never connect the CAN and K pads at
   the same time.** Leave S5R5 (bottom left, next to S11U1) open - it bypasses
   the over-voltage protection and only exists for boards built without the
   S11 stage.

5. Once all the SMD components are placed, next do the Molex connector. General
rule of assembly for through-hole components is in order of vertical height.

6. Next is the 2-pin Connect Kit power header S1J4 and, for CAN builds, the
2-pin termination header S9J1. Only fit a shunt on S9J1 if the tracker is going
to be at the end of the CAN bus.

7. Then the 20-pin headers. I recommend using tape to hold them in position,
solder a single pin on each row to hold them, then remove the tape and solder
the rest.

8. Finally the SMA connectors. Easiest way I've found is to rest the board
upside down with the connectors in place, surround each pin with solder paste
and then apply hot air. Because the connectors are metal they have a lot of
thermal mass so turn heat and airflow up to maximum (500C in my case). As soon
as the solder melts again the pin remove the heat, if you let too much of it
drain down by the pins you can end up shorting the centre pin to ground.

Once an SMA connector has been through this once or twice it's generally got too
much solder on it to be easily reused so I've begun treating them as disposable.

## Testing

**Don't connect the Makerdiary Connect Kit until these checks have been
completed**

The test points make most of this easy: S12TP1 is ground, S12TP2 is the
protected 4.2V rail, S12TP3 is the 3.3V rail (which comes from the Connect Kit,
so it's unpowered during these tests), S12TP4-S12TP6 are the switched 3.3V GPS,
CAN and K rails and S12TP7 is the switched 12V K rail.

1. Check ohms from every test point to S12TP1, address any shorts before
  continuing
2. Check continuity from the 12V rails (Molex pin 4 and pin 5, and S12TP7) to
  the 4.2V and 3.3V test points, address any shorts before continuing
3. Check for shorts between all adjacent pads on the ASM330LHHXTR, eg between
  SDA/SCL, SDA/3.3V etc
4. Same test for the INA228, the LT8609 and any OBD chips if placed
5. Check continuity between the centre pin of the SMA connectors to the outer
  casing - if shorted the antennas won't work
6. **Without** the Connect Kit connected to the board, apply 12V to the 12V and
  ignition inputs. On the Molex, pin 2 is ground, pin 4 is permanent 12V and
  pin 5 is ignition (pin 3 and pin 6 are the OBD bus lines and pin 1 is
  unused). Having a bench PSU helps here, if you have one set its current limit
  to 50mA and watch the display as you turn it on. If the current immediately
  maxes out, turn it off quickly. This indicates a short and needs to be fixed
  before proceeding. If it jumps up and then settles to 0 that indicates normal
  operation (the buck going to sleep).
7. With the inputs live, check S12TP2 reads 4.2V. This confirms the buck, the
  OVP MOSFET S11Q1 and the ideal diode S11U1 are all passing the rail through
8. Check S12TP4-S12TP7 all read 0V. The load switches should be off with no
  enable signal from the Connect Kit
9. Turn the board over and probe voltage on every pin of the two 20-pin
  headers, make sure there's no 12V reading on any of them. Every pin should
  read 0V - nothing on the headers is powered until the Connect Kit supplies
  the 3.3V rail, and the 4.2V supply reaches the Connect Kit through S1J4
  rather than the headers

## Final assembly

1. Connect 35mm u.FL cables to the connectors on the board
2. Connect the power cable to the Makerdiary battery connector and the other
end to S1J4, observing the `+` and `-` markings
3. Plug the Makerdiary Connect Kit into the board and connect the u.FL cables to
its terminals
4. Connect the two SMA connectors to the external antenna

Apply power again from the PSU with current limited to 50mA - any sign of a
short and power off immediately. If the current doesn't pin at the max then
chances are the PCB is built correctly.

Note that shorts after the auxiliary power rail load switches may still be
present, but on v3.1 and later boards these can be detected by the firmware.
You can also confirm them by hand: once the firmware enables a rail the
matching test point (S12TP4 GPS, S12TP5 CAN, S12TP6 K, S12TP7 12V K) should
come up to 3.3V or 12V.

If you've made it this far you should now have a working tracker, the next step
is to configure the firmware. See: [QUICKSTART.md](../firmware/QUICKSTART.md)
