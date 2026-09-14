# Mounting recommendations

The aim is a tracker that is hard to find, cannot come loose, is powered safely and still has an antenna with a view of the sky. What follows is general advice; the vehicle decides what is possible. Read [Deployment: read this first](/deployment/read-this-first.html) before you start.

## Where to put the tracker

Bury the tracker inside the dashboard or behind a trim panel - somewhere a thief would need tools and time to reach, not just somewhere out of sight.

Avoid:

- airbag modules, their wiring and the areas airbags deploy into;
- heater ducts and other hot spots;
- footwells, the bottoms of pillars and anywhere else water can collect or run down;
- the steering column, the pedal box, seat runners, the glovebox mechanism and any other moving parts;
- places where the enclosure can knock against a panel.

Fix it so it cannot move: cable ties around the enclosure to an existing loom or bracket, or foam mounting tape on a clean surface. Wrap it in felt or foam tape if it could rattle. Do not leave it hanging on its cables, and support the harness close to the connector so the connector does not carry the weight of the wires.

Choose whatever orientation is convenient, then leave it alone. While parked, the tracker raises a tilt alert when its attitude changes from the one it had when it went to sleep, and the track-mode dashboard learns which way the vehicle's forward axis points for each device. Both assume the unit stays where it was fitted.

The enclosure is a 3D-printed prototype and a parked car gets hot. Print it in a material that tolerates the temperatures inside the vehicle; behind the dashboard is cooler than on top of it.

## Antenna

The antenna should be out of sight but near glass. GNSS needs a view of the sky, and LTE works best without metal around it.

- Under the top of the dashboard close to the base of the windscreen, or near the rear window, often works. Test a position before you settle on it.
- Keep metal from coming between the antenna and the sky - the roof, the dashboard frame and other brackets.
- Some windscreens have a heat-reflective coating or heating elements that weaken signals. Some vehicles leave an uncoated area of glass for toll tags and similar devices; the vehicle's manual may say where it is.
- The recommended Taoglas MA310 is a magnetic-mount antenna designed for a metal roof. Inside a vehicle, fix it with a cable tie or an adhesive pad so it cannot slide about.
- Route the cable where trim clips, door seals and seat mechanisms cannot pinch or crush it. Avoid tight bends, and coil any slack loosely and tie it down.
- Connect the GNSS lead to the GNSS connector. A passive or DC-grounded antenna on the GNSS port shorts the antenna bias supply, and GNSS will not work.

Check the reception from the final position before closing anything up - see [Before you close it up](#before-you-close-it-up).

## Power

The tracker needs a permanent 12V feed (connector pin 4), an ignition-switched 12V feed (pin 5) and ground (pin 2). Each 12V feed gets its own 2A inline fuse, placed as close to its source as practical.

### Permanent feed

A permanent source is live with the ignition off and the key removed, and stays live after the vehicle has been locked and left long enough for its control units to go to sleep - some circuits are switched off by the vehicle a while after it is locked. Measure it at the end of that wait.

Common choices:

- a fuse tap on a permanent circuit in a fuse box;
- pin 16 of the OBD socket, which is meant to be permanent battery positive - measure it;
- the battery positive terminal, with the inline fuse as close to the battery as practical.

### Ignition feed

The ignition source should be live only while the ignition is on, and should drop as soon as the ignition goes off. The tracker reads this input as "ignition on", so:

- a circuit that is also live in the accessory position makes accessory mode look like ignition on, which starts journeys and ignition alarms early;
- a circuit that stays live for a while after the ignition goes off delays the ignition-off record, the end of the journey and the tracker's return to sleep.

Watch the voltage with a multimeter while you turn the ignition on and off to confirm the circuit behaves.

### Fuse taps

- Use the fuse box map to choose circuits, and never take power from a circuit that feeds a safety system.
- With the original fuse removed, find which contact in the slot is the supply side, then fit the tap the way its instructions describe for that side.
- Put the original fuse back in its position in the tap and a 2A fuse in the position for the new circuit. Never fit a larger fuse than the circuit originally had.

### Ground

Use a body earth point or earth stud: bare, clean metal, a ring terminal and a secure fixing - not a painted panel, a plastic bracket or a trim screw. Many modern vehicles measure battery current with a sensor on the battery's negative lead, so take the ground from the body rather than from the battery negative terminal.

### Check the harness before connecting the tracker

With the harness finished but the tracker unplugged, measure at the Micro-Fit housing with a multimeter. Check the pin numbers against the Molex drawing for the housing rather than guessing from its face.

| Pins | Ignition off | Ignition on |
|---|---|---|
| 4 to 2 | Battery voltage | Battery voltage |
| 5 to 2 | 0V | Battery voltage |
| 1 | Nothing connected | Nothing connected |
| 3 and 6 | Nothing connected, unless you have wired the bus lines | As ignition off |

Only plug the tracker in when these are right.

## Diagnostic wiring (optional)

Connect bus lines only to a board whose [interface selection pads](/reference/hardware.html#interface-selection-pads) are set for that interface, and only if you want what they provide.

For a K-wire build:

- OBD pin 7 (K) goes to connector pin 3.
- OBD pin 15 (L) goes to connector pin 6, but only if K-wire discovery shows that the vehicle needs the L line.
- Ground is connector pin 2, shared with the rest of the harness.

For a CAN build:

- OBD pin 6 (CAN high) goes to connector pin 3 and OBD pin 14 (CAN low) to connector pin 6, as a twisted pair, with the branch to the socket kept short.
- Leave the CAN termination header S9J1 without a shunt: the vehicle's bus is already terminated at both ends.
- Firmware 0.4.x reads no data over CAN, so there is nothing to gain from connecting it yet.

A splitter keeps the diagnostic socket usable. A scan tool and the tracker talking on the K line at the same time can corrupt each other's traffic, so unplug the tracker's diagnostic lead before a garage connects a scan tool.

## USB-C extension to the glovebox (optional)

The Connect Kit's USB-C port is how you watch the console and reflash the tracker. A USB-C extension with a panel-mount socket, routed to the glovebox or another place you can reach, keeps that possible without taking the dashboard apart.

- USB can be connected at any time. The board feeds the Connect Kit through its battery connector, so USB simply takes over the supply while it is plugged in.
- Update the [Makerdiary interface firmware](/board-setup/makerdiary-firmware.html) first. Without it, unplugging USB leaves the interface MCU drawing about 2mA.
- Use a short, good-quality cable that carries data; a charge-only cable will not work.

!!! warning "An accessible USB port is a way in"
    The USB-C port gives full debug access to the tracker. Anyone who finds the socket can erase or reflash the tracker, or read its firmware, which contains the device's PSK. Hide the socket as carefully as the tracker, and [rekey the device](/board-setup/server-onboarding.html) if you think someone has used it.

## Push-to-break reset button (optional)

A normally closed momentary switch - push to break - wired in series with the permanent 12V feed, after its fuse, lets you power-cycle the tracker without reaching it. Use it if the tracker stops responding, or to clear a latched over-voltage trip, which only releases once the supply to the protection stage has fallen.

- The ignition input only feeds a sense circuit, so breaking the permanent feed removes all of the tracker's power.
- Hold the button down for a few seconds so the input capacitors discharge. The tracker restarts when you let go.
- With a USB cable connected, the Connect Kit runs from USB and the button does not restart it.
- Use a switch rated for 12V automotive use, and mount it discreetly: holding it down, or finding and cutting its wires, turns the tracker off.

## Before you close it up

Before refitting any trim:

1. Check the tracker is fixed, its connector is latched and the harness is supported, with nothing near moving parts or airbags.
2. Check the tracker reports from its final position with the antenna where it will stay. The latest records should show a GNSS fix (the position on the map page, or `satellites` and `hdop` in the `log` table) and `rat` should read `CATM1`. See [Verifying telemetry](/board-setup/verifying-telemetry.html).
3. Turn the ignition on and then off, and check that both records arrive.

Do the [test drive](/deployment/test-drive.html) before relying on the installation.
