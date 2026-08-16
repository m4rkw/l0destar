# l0destar assembly guide

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

This document assumes a l0destar v3.1 PCB, see: [PROTECTION.md](https://github.com/m4rkw/l0destar/blob/master/hardware/l0destar_v3.1/PROTECTION.md) for details of
the protection features.

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

## Suggestions

- Buy more components than you need. There's only a single 0201 component but
  even at 0402 it's very easy to lose them or have them snap out of tweezers
- Don't scrimp on ventilation / fume extraction, solder and flux fumes can be
  very toxic and cause permanent damage
- Open the PCB in Kicad before starting so you can reference each part as you go

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

4. For fine pitch components like the LT8609 you might end up with bridged pins.
If this happens simply apply heat with a soldering iron tip against some MG #444
wick very briefly - just a second or two - to remove the excess.

## Build sequence

1. Place the buck converter components first. It's important that this works and
regulates its output correctly, a fault here can cause the full input voltage to
be output which would destroy the MCU board. Flow these into place. Don't place
S6U2 yet as this can also be damaged by high voltage.

2. Solder temporary wires to a ground point and to the PP12VP net. Apply 12V and
measure the voltage at pin 1 of the S6U2 footprint, it should be 4.2V. If it's
0V or 12V, check for shorts or bad connections. before continuing.

3. Apply paste to the remaining pads, place the rest of the components and
solder them.

4. Solder the configuration pads according to the configuration of the board,
this can be either CAN, K-line or no OBD comms.

5. Once all the SMD components are placed, next do the Molex collector. General
rule of assembly for through-hole components is in order of vertical height.

6. Next is the 20-pin headers. I recommend using tape to hold them in position,
solder a single pin on each row to hold them, then remove the tape and solder
the rest.

7. Finally the SMA connectors. Easiest way I've found is to rest the board
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

1. Check ohms from all power rails to ground, address any shorts before
  continuing
2. Check continuity from all 12V rails to 3.3V rails, address any shorts before
  continuing
3. Check for shorts between all adjacent pads on the ASM330LHHXTR, eg between
  SDA/SCL, SDA/3.3V etc
4. Same test for the INA228 and any OBD chips if placed
5. Check continuity between the centre pin of the SMA connectors to the outer
  casing - if shorted the antennas won't work
6. **Without** the Connect Kit connected to the board, apply 12V to the 12V and
  ignition inputs. Having a bench PSU helps here, if you have one set its
  current limit to 50mA and watch the display as you turn it on. If the current
  immediately maxes out, turn it off quickly. This indicates a short and needs
  to be fixed before proceeding. If it jumps up and then settles to 0 that
  indicates normal operation (the buck going to sleep).
7. With the 12V inputs live turn the board over and probe voltage on every pin
  of the two 20-pin headers, make sure there's no 12V reading on any of them

## Final assembly

1. Connect 3cm u.FL cables to the connectors on the board
2. Connect the power cable to the Makerdiary battery connector
3. Plug the Makerdiary Connect Kit into the board and connect the u.FL cables to
its terminals
4. Connect the two SMA connectors to the external antenna

Apply power again from the PSU with current limited to 50mA - any sign of a
short and power off immediately. If the current doesn't pin at the max then
changes are the PCB is built correctly.

Note that shorts after the auxillary power rail load switches may still be
present, but with the v3.1 board these can now be detected by the firmware.

If you've made it this far you should now have a working tracker, the next step
is to configure the firmware. See: [QUICKSTART.md](https://github.com/m4rkw/l0destar/blob/master/firmware/QUICKSTART.md)
