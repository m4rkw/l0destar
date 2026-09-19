# Deployment: read this first

This section covers fitting a working l0destar tracker to a vehicle: wiring it into the vehicle's electrics, hiding it, checking that it reports properly and setting up alerts. Read this page before you start, together with [Assembly: read this first](../assembly/read-this-first.md) and the project's [full disclaimer](https://github.com/m4rkw/l0destar/blob/master/DISCLAIMER.md).

## You are responsible for the installation

l0destar is a prototype, not a product. Nothing in the hardware, the firmware, the enclosure or these instructions has been validated or approved for installation in a vehicle, and nobody offers any warranty or accepts any liability for what happens when you install one. v3.4, the board these pages describe, has not been built or tested yet; v3.3 has been bench tested by the author.

The advice here is general. Every vehicle is wired differently, and nothing on these pages can tell you what a particular circuit, connector or fuse in your vehicle does. Use the vehicle's own documentation, measure before you connect anything, and if you are not confident working on vehicle electrics, have an auto electrician do the installation.

## Fire and electrical safety

!!! danger "Fuse both 12V feeds"
    Fit a 2A inline fuse in the permanent 12V feed and another in the ignition feed, each as close as practical to the point where you take the power. The fuses on the board are a backstop against faults on the board side and cannot protect the wire running to it: an unfused wire that chafes through to the bodywork can start a fire.

- The board is designed for 12V vehicle electrical systems. 24V vehicles are not supported.
- Plan the route of every wire before you cut anything. Keep wires away from sharp edges, hot parts and anything that moves - the steering column, pedals, seat runners, glovebox and door hinges - and use grommets wherever a wire passes through metal.
- Make every joint properly: crimped or soldered, insulated with heat shrink, and supported so vibration cannot work it loose. Avoid insulation-displacement splice connectors, which can cut strands and loosen over time.
- Measure a circuit with a multimeter before you connect to it, and check the finished harness before plugging the tracker in.
- Depending on the vehicle, disconnecting the battery can reset other systems or require them to be set up again. Check the vehicle's manual before you disconnect it and follow its procedure.

!!! danger "Airbags"
    Never tap into, probe or run wires along airbag (SRS) wiring or its connectors. They are commonly yellow, but do not rely on the colour. Keep the tracker, the antenna and all wiring out of the areas airbags deploy into: the steering wheel, the top and lower edge of the dashboard, the pillars and roof lining, and the sides of the seats.

## Vehicle diagnostic connections

Connecting the tracker's CAN or K-wire lines to the vehicle's diagnostic socket joins it to a bus that the vehicle's control units use. A wiring mistake or a firmware fault can interfere with those units, including while the vehicle is moving, or damage them.

- The tracker works without any diagnostic connection. Leave pins 3 and 6 of its connector unconnected unless you need what they provide.
- Firmware 0.4.x reads engine data and stored fault codes over the K wire only. It reads nothing over CAN, so there is currently no reason to connect a CAN build to a vehicle.
- The runtime K-wire code only sends read-only OBD-II requests. Nothing in the firmware clears fault codes.
- A firmware fault can hold the K line low until the watchdog restarts the tracker, which can take about half a minute. While the line is held, no scan tool can talk to the vehicle over it.
- Never run the K-wire discovery on a moving vehicle: its address sweeps can occupy the bus for up to a quarter of an hour, or about half an hour when it tries the L line.
- Connect the L wire to an installed tracker only if the K-wire discovery summary says `L wire needed`.
- Do not fit the CAN termination shunt. The vehicle's bus is already terminated.

## Mounting

- Secure the tracker and the antenna so they cannot come loose, rattle, or end up anywhere near the pedals or the steering.
- Keep both away from heat sources and from anywhere water can collect.
- The enclosure is an unvalidated 3D print. Its strength, heat resistance and flammability depend on the material you print it in.

## Driving

Do not look at the web interface while you are driving. Track mode is for closed circuits, private land and the bench; on a public road, obey the law and the conditions and keep your eyes on the road. The full warning is at the top of [TRACK_MODE.md](https://github.com/m4rkw/l0destar/blob/master/firmware/TRACK_MODE.md).

## Not a safety or theft-recovery system

The tracker can fail silently: no coverage, a flat battery, a disconnected harness, a firmware fault. Anyone who finds it can unplug it. It must not be anyone's only theft-recovery, safety or emergency measure.

## Location data and the law

The tracker records where the vehicle goes and when. If you operate one, you are the data controller for that data - including when someone else drives the vehicle, such as a family member, an employee or a borrower. Tracking a vehicle you do not own, or tracking its drivers without their knowledge, may be unlawful where you live. Find out what the law requires, get consent or give notice where it does, and keep the data secure (see [Server security](../server/security.md)).

## Radio, vehicle approval and insurance

- Nothing in this project is CE, UKCA or FCC marked, and no conformity assessment has been carried out. Running an LTE radio may require type approval where you live.
- The tracker has not been assessed against the rules that apply to electronic equipment fitted to vehicles, such as UNECE Regulation 10.
- Changing a vehicle's wiring can affect its warranty and its insurance. Check with your insurer. The tracker is not an insurer-approved tracking or security device.
