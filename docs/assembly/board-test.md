# Board test

Test a newly assembled board in four stages, stopping to fix anything unexpected before moving
on:

1. unpowered checks, without the Connect Kit;
2. first power-up, still without the Connect Kit;
3. power-up with the Connect Kit fitted;
4. the interactive firmware board test, which exercises every fitted subsystem.

Stages 1 and 2 need only a multimeter and a current-limited supply. Stage 4 also needs the
firmware toolchain.

## Test points

v3.4 has seven test points. Readings below are with 12V applied to the board.

| Test point | Label | Net | Expected |
|---|---|---|---|
| S12TP1 | `GND` | GND | Ground reference for every reading |
| S12TP2 | `4.2V protected` | PP4V2 | 4.2V whenever the input is powered: the buck, the OVP MOSFET S11Q1 and the ideal diode S11U1 are all passing the rail |
| S12TP3 | `3.3V` | PP3V3 | 3.3V only with the Connect Kit fitted - it comes from the Connect Kit |
| S12TP4 | `3.3V GPS` | PP3V3_GPS | 0V until the firmware switches the GPS rail on |
| S12TP5 | `3.3V CAN` | PP3V3_CAN | 0V until the firmware switches the CAN rail on |
| S12TP6 | `3.3V K` | PP3V3_K | 0V until the firmware switches the K-wire rails on |
| S12TP7 | `12V K` | PP12V_K | 0V until the firmware switches the K-wire rails on |

The unprotected buck output has no test point on v3.4; measure it at the non-ground pad of S6C5.

## Stage 1: unpowered checks

With no power applied and the Connect Kit **not** fitted:

1. Measure resistance from every test point to S12TP1. Find and fix any short before continuing.
2. Check for continuity from the 12V rails - Molex pin 4, pin 5 and S12TP7 - to the 4.2V and 3.3V
   test points. There must be none.
3. Check for shorts between every pair of adjacent pads on the ASM330LHHXTR accelerometer, for
   example between SDA and SCL, or SDA and 3.3V.
4. Do the same for the INA228, the LT8609A and any interface chips you placed.
5. Check that the centre pin of each SMA connector is not shorted to its outer casing. If it is,
   that antenna will not work.

## Stage 2: first power-up without the Connect Kit

The board is fed through the Molex connector: pin 2 is ground, pin 4 the permanent 12V and pin 5
the ignition input. Pins 3 and 6 are the interface bus lines and pin 1 is unused. See the
[vehicle connector pinout](/reference/hardware.html#vehicle-connector-pinout).

1. Set the bench supply's current limit to 50mA.
2. Apply 12V to both the permanent (pin 4) and ignition (pin 5) inputs and watch the current as you
   switch on. If it immediately pins at the limit, switch off quickly: there is a short to find.
   If it jumps up and then settles close to 0, that is normal - the buck converter going to sleep.
3. Check that S12TP2 reads 4.2V.
4. Check that S12TP4, S12TP5, S12TP6 and S12TP7 all read 0V. The load switches should be off, since
   nothing is driving their enable signals.
5. Turn the board over and measure the voltage on every pin of both 20-pin headers. Every pin
   should read below 3.3V, and none may read 12V. Nothing on the headers is powered until the Connect
   Kit supplies the 3.3V rail; the 4.2V supply reaches the Connect Kit through the power lead in S1J4,
   not through the headers.

!!! danger "Any 12V on a header pin means stop"
    Do not fit the Connect Kit to a board that shows battery voltage on any header pin. It would be
    destroyed.

## Stage 3: power-up with the Connect Kit

Carry out the [final assembly](/assembly/assembly-guide.html#final-assembly) steps, then:

1. Apply 12V again with the current limited to 50mA. At any sign of a short, switch off
   immediately. If the current does not pin at the limit, chances are the board is built
   correctly.
2. Once the firmware is running, it switches rails on as it needs them, and the matching test
   point should then come up: S12TP4 (GPS) to 3.3V, S12TP5 (CAN) to 3.3V, S12TP6 (K) to 3.3V and
   S12TP7 (K) to 12V.

A short after one of the auxiliary rail load switches will not show up until the firmware enables
that rail. The board senses each rail, so the firmware detects a rail that fails to come
up, and the firmware board test below checks every one.

## Stage 4: the firmware board test

`firmware/board_test.sh` builds and flashes a test firmware that walks you through every fitted
subsystem over the serial console: rail switching, ignition sensing and wake, battery voltage, the
accelerometer, GPS, the modem and the interface loopbacks.

### What you need

- The software and the clone of the repository from
  [Board setup prerequisites](/board-setup/prerequisites.html). Updating the Connect Kit's
  interface firmware ([Updating the Makerdiary firmware](/board-setup/makerdiary-firmware.html))
  is not needed for the test, but is needed before the board goes into use, so you may as well do
  it first.
- A USB-C data cable to the Connect Kit. The script flashes over it and uses its serial console.
- A bench lead with a **switch in the ignition feed** (pin 5), so you can turn the ignition on and
  off while the permanent 12V stays connected.
- An **adjustable supply**: one test asks you to raise the voltage by at least 1V and then lower
  it by at least 1V.
- The supply's **current limit raised to around 300mA**. The 50mA limit of the first power-up
  catches shorts, but a modem registering and transmitting draws more than that from the 12V input,
  and a supply sitting in current limit makes the board brown out and reset.
- A SIM card in the Connect Kit and an antenna on both SMA connectors, with the GPS antenna able
  to see the sky. The GPS test searches for an unassisted cold fix, which can take several minutes.
- `CONFIG_APP_APN` set to your SIM's APN in `firmware/local.conf`.

!!! note "Set your APN before running the test"
    The script uses the first APN it finds in `local.conf`, then the Makerdiary profile overlay,
    then `prj.conf`. `prj.conf` always sets one (`sensor.net`), so without an APN in `local.conf`
    the modem test silently uses that value rather than asking - and it is unlikely to be the APN
    for your SIM.

A one-line `firmware/local.conf` is enough for the test:

```text
CONFIG_APP_APN="your.apn"
```

### Running the test

```sh
# in firmware/
./board_test.sh
```

The script asks three things:

1. **Board version.** There is no v3.4 entry: **choose v3.3 for a v3.4 board.** The two boards
   connect the Connect Kit's pins identically; the only differences between their PCBs are the
   accelerometer pads and net names.
2. **OBD interface**: none, CAN or K-wire (ISO 14230), matching the parts and pads
   you fitted.
3. **APN.** It shows the APN it found and where it found it. It only asks if it finds none, and
   leaving the answer empty skips the modem test.

It then writes the answers to `board_test.conf` (gitignored), builds the firmware with
`CONFIG_APP_BOARD_TEST=y`, flashes it with `./flash.sh`, opens a `screen` session called
`l0destar-test` on the console port at 115200 baud - the first `/dev/cu.usbmodem*` port on macOS,
or the Connect Kit's `-if00` port under `/dev/serial/by-id/` on Linux - resets the board and
attaches you to the console. Press **Enter** to start the tests.

`local.conf` is not layered into the test build, but four values are read from it:
`CONFIG_APP_APN`, `CONFIG_APP_DEMO_MODE` (masks the GPS coordinates in the output),
`CONFIG_APP_BOARD_TEST_HIDE_COORDS` (drops them altogether) and `CONFIG_APP_CRASH_THRESHOLD_MG`
(the impact threshold, 1200mg by default for the test - a firm bang on the desk is 1.5-3g). The
test build also limits network registration to 180 seconds.

Useful `screen` keys: `Ctrl-A [` scrolls back, `Ctrl-A d` detaches (reattach with
`screen -r l0destar-test`) and `Ctrl-A k` kills the session. The console output is logged to
`firmware/screenlog.0`.

### The tests

| # | Test | What you do | Passes when |
|---|---|---|---|
| 1 | GPS rail switching | Nothing | The GPS rail's sense line follows it off and back on. The rail is left on. |
| 2 | OBD rail switching | Nothing | CAN builds: the CAN 3.3V rail cycles. K-wire builds: the K-wire 3.3V and 12V rails cycle. Skipped with no interface. |
| 3 | Ignition sense | Switch the ignition on and off three times within 180 seconds | All six edges are seen. |
| 4 | Ignition wake from sleep | Switch the ignition off when asked (within 60 seconds), wait for `SLEEPING`, then switch it on within 180 seconds | The board wakes on the ignition edge and reads the ignition as on. |
| 5 | Battery voltage | Raise the supply by at least 1V, then lower it by at least 1V, within 300 seconds | The INA228 reading follows both steps. |
| 6 | Accelerometer | Put the board flat and still; tilt and rotate it and watch the roll and pitch follow; then bang the desk at least once within 300 seconds | An impact is captured, with its peak g, rotation rate and duration. |
| 7 | Movement wake from sleep | Leave the board still, wait for `SLEEPING`, then move or tap it within 180 seconds | The board wakes on movement. |
| 8 | Raw GPS fix | Nothing, but the GPS antenna needs a view of the sky | A fix with no assistance data, within the cold-start timeout (300 seconds by default). |
| 9 | Modem and DNS | Nothing | The SIM is detected, the modem registers with your APN, and `www.google.com` resolves. Skipped when there is no SIM. |
| 10 | K-line loopback | Nothing | The K-wire transceiver echoes what is sent through it, and the L-line FET works where it is fitted. Skipped without a K-wire interface or if its rails failed. |
| 11 | CAN loopback | Nothing | The MCP2518FD passes its internal and external (through the transceiver) loopback modes. No other device on the bus is needed. Skipped without a CAN interface or if its rail failed. |

Keep the voltage step in test 5 modest - from 12V up to about 13V and back is plenty.

A failed test is reported and the run continues. Tests that depend on hardware that has already
failed are skipped rather than run: there is no GPS search on a dead GPS rail and no loopback on a
rail that never came up. The run ends with a summary:

```text
==============================================
  BOARD TEST SUMMARY
==============================================
   1. GPS rail switching           PASS
   ...
  11. CAN loopback                 SKIP
----------------------------------------------
  10 pass, 0 fail, 1 skip -- BOARD OK
==============================================
```

`BOARD OK` means nothing failed; skipped tests do not count against it.

### After the test

The test firmware runs the tests and then parks - it never starts the tracker. Flash the normal
firmware when you are done, as described in
[Minimal config and initial flashing](/board-setup/initial-flashing.html).

### Troubleshooting

| Symptom | What to try |
|---|---|
| `No Connect Kit serial port found` | Check the USB-C cable carries data and the Connect Kit enumerates (`pyocd list` should show its probe). If the Connect Kit LED isn't lit it may not be getting power or the 3.3V rail might be shorted to ground. On Linux, check the access set up in [Board setup prerequisites](/board-setup/prerequisites.html#linux-access-to-the-connect-kit). |
| The console stays silent | The Connect Kit exposes two serial ports and the script uses the first. Run again with `SERIAL=/dev/cu.usbmodemXXXX ./board_test.sh`, naming the other port. |
| `screen: command not found` | The script needs GNU screen: `sudo apt install -y screen` on Linux, or Homebrew or MacPorts on macOS. |
| `screen could not open` the port | The port can be busy or re-enumerating for a few seconds after flashing; the script retries for 10 seconds. If it still fails, the script prints what holds the port; close that and re-run. |
| The reset fails and pyocd reports APPROTECT | The chip booted with debug access locked. `./flash.sh` is the recovery path: its load step is allowed to unlock the part and reprogram it. |
| The boot output is too quiet to diagnose a problem | Run with `VERBOSE=1` to keep the module logs; warnings and errors always print. |
| Test 6 prints `interrupt but no FIFO data -- hit harder?` | Bang the desk harder. |
| Test 8 times out | Check the antenna on the GPS port is an active antenna, that the u.FL cables go GPS to GPS, and that the antenna can see the sky. |
| Test 9 reports no SIM, or registration fails | Check the SIM is seated, that your network has LTE-M coverage where you are, and the APN. |

The script tries a `power on` command when no debug probe is visible. That is the author's helper
for a switched USB hub; if you have no such command it is skipped.
