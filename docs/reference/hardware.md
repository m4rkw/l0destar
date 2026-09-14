# Hardware configuration reference

Everything that can be configured on the hardware side of a l0destar board - which firmware board
definition to build, the interface pads, the optional parts, the connectors and the antenna -
together with the board's key figures. It describes v3.4, with notes for earlier boards.

Measured figures are the author's unverified bench observations. v3.4 itself has not been built
yet.

## Board revisions and firmware selection

The firmware needs to know which carrier board it runs on, because every revision lands signals on
different Connect Kit pins and switches its power rails differently. Select the board in
`firmware/local.conf` (or in the device's section of `remote.conf` for published builds).

| Board | Status | Board selection | Interface setting | FOTA board id |
|---|---|---|---|---|
| v3.4 | Designed, not yet built or tested | `CONFIG_APP_BOARD_L0DESTAR_V3_4=y` | `CONFIG_APP_OBD_MODE=0`, `1` or `2` | `v3.4`, `v3.4+can`, `v3.4+kline` |
| v3.3 | Built and bench tested | `CONFIG_APP_BOARD_L0DESTAR_V3_3=y` | `CONFIG_APP_OBD_MODE=0`, `1` or `2` | `v3.3`, `v3.3+can`, `v3.3+kline` |

v3.4 has its own board definition, but it is the v3.3 one under a new name: the Connect Kit
headers and the power header carry identical nets on the two PCBs - the only differences are the
renamed bus nets and the accelerometer's NC pads - so the pin map and the rail topology are the same.
It exists so that update images stay tied to the revision: a v3.4 unit reports the FOTA board id
`v3.4` and installs only images built for it. Pick v3.4 in the interactive board test.

`CONFIG_APP_OBD_MODE` tells the firmware which interface is populated, and with it
which rails to switch and which drivers to start:

| Value | Interface | Pads bridged | Rails used |
|---|---|---|---|
| `0` | None (the default) | none | - |
| `1` | CAN | S5R1 and S5R3 | 3.3V CAN rail |
| `2` | K-wire | S5R2 and S5R4 | 3.3V K and 12V K rails |

## Interface selection pads

The four configuration pads in the top left corner route the two vehicle bus lines on the Molex
connector (pins 3 and 6) to either the CAN block or the K-wire block. Bridge them with solder or a
0402 0R resistor.

| Interface | S5R1 | S5R2 | S5R3 | S5R4 |
|-----------|------|------|------|------|
| None      | OPEN | OPEN | OPEN | OPEN |
| CAN bus   | CONNECT | OPEN | CONNECT | OPEN |
| K-wire    | OPEN | CONNECT | OPEN | CONNECT |

!!! danger "Only one interface at a time"
    If the CAN and K pads are both connected, unpredictable behaviour may occur. Make sure the pads
    that should be open are not connected.

The pads only route the bus lines. Each interface's power rails are switched by their own load
switches under firmware control, so an unused interface draws nothing. Both sets of interface parts
can be populated, but only one can be in use.

**S5R5** is a separate pad that bridges the unprotected 4.2V rail straight to the protected one,
bypassing the over-voltage protection MOSFET. Leave it **open** unless the S11 protection stage is
not populated, which is not recommended.

## Vehicle connector pinout

The board's vehicle connector S1J1 is a 6-way Molex Micro-Fit 3.0 right-angle header, 3.0mm pitch,
Molex 43045-0600. A harness mates with it through a Micro-Fit 3.0 dual-row 6-way receptacle
housing, Molex 43025-0600, fitted with female crimp terminals.

| Pin | Function |
|---|---|
| 1 | Not connected |
| 2 | Ground |
| 3 | CAN high, or the K-wire K line |
| 4 | 12V permanent live |
| 5 | 12V ignition |
| 6 | CAN low, or the K-wire L line |

Pins 3 and 6 are routed by the [interface selection pads](#interface-selection-pads). On a vehicle's
OBD socket, CAN high is pin 6 and CAN low pin 14; the K line is pin 7 and the L line pin 15.

- Fit an external 2A fuse on both 12V feeds, pins 4 and 5.
- The ignition input only feeds a sense divider; the board is powered from pin 4.
- Check the pin numbering against the Molex drawing for the housing you use, and check every wire
  with a meter before connecting the board.

## Other headers

| Header | Pins | Purpose |
|---|---|---|
| S1J2 | Connect Kit pins 1-20 | Lower 20-pin 2.54mm header. Pin 1, the Connect Kit's VSYS (the l0destar symbol names it VBUS), is the square pad at the Molex end. |
| S1J3 | Connect Kit pins 21-40 | Upper 20-pin 2.54mm header, pin 40 at the Molex end. |
| S1J4 | `+` protected 4.2V, `-` ground | Holes for the MX1.25 lead that feeds the Connect Kit's battery connector; the lead's wires are soldered in. |
| S9J1 | CAN termination | 2-pin 2.54mm header in series with the 120R resistor S9R3 across the bus. CAN builds only. |

**CAN termination.** Fit a shunt on S9J1 only if the tracker is at the end of the CAN bus. A tracker
connected to a vehicle's OBD socket normally is not - the vehicle's bus is already terminated - so
leave it off there. On a bench with a USB CAN adapter and nothing else on the bus, both ends need
terminating.

**Powering the Connect Kit.** The Connect Kit could be powered through VBUS, but then a powered USB
cable and VBUS could not be connected at the same time. Instead the 4.2V buck output feeds the
Connect Kit's battery connector through the over-voltage protection stage and an ideal diode, so
USB-C can be connected and disconnected at any time for programming without any power disruption.

## Optional parts

### Buck enable divider

S6R4, S6R5 and S6R6 form a divider on the LT8609A's EN/UV pin that gives the buck a defined
under-voltage lockout with hysteresis.

| Build | S6R4 | S6R5 | S6R6 | Behaviour |
|---|---|---|---|---|
| Default | 0402 0R | not fitted | not fitted | EN tied to the input; the buck runs whenever input voltage is present and relies on its own internal under-voltage lockout |
| With divider | 0402 1M 1%, anti-sulfur AEC-Q200 | 0402 243K 1%, anti-sulfur AEC-Q200 | 0402 3.92M 1% | Starts only once the supply reaches about 5.6V; shuts off if it falls to about 4.3V, then needs about 5.6V again (calculated; a v3.3 board measured 5.14V on and 4.47V off) |

With the EN threshold at 1.05V rising and 1.00V falling and an output of 4.24V:

- K = 1 + S6R4/S6R5 + S6R4/S6R6
- VIN(on) = K x 1.05 ≈ 5.6V
- VIN(off) = K x 1.00 - S6R4 x VOUT / S6R6 ≈ 4.3V

Across the EN/UV threshold's specified tolerance band the turn-off point stays within about 3.97V to
4.61V, above the roughly 3.4V at which the board browns out under peak LTE load. The divider exists
for the ISO 16750-2 tests that require clearly defined behaviour during low-voltage and drop-out
events. It draws around 9µA continuously from a 12V input, which is why it is not fitted by
default.

### CAN common-mode choke

S9FL1 (ACT1210-101-2P-TL00) sits in series with CAN high and CAN low between the transceiver and the
connector. It improves radiated emissions and common-mode noise rejection, but CAN works without it.

!!! warning "If S9FL1 is omitted, bridge its pads"
    Fit two 0402 0R resistors across the S9FL1 footprint, one in each line. Without them CAN high
    and CAN low are open circuit.

### Interface parts

The CAN parts and the K-wire parts are optional as a group; see
[component selection](/assembly/component-selection.html).

## Test points

| Test point | Net |
|---|---|
| S12TP1 | Ground |
| S12TP2 | PP4V2, the protected rail feeding the Connect Kit |
| S12TP3 | PP3V3, supplied by the Connect Kit |
| S12TP4 | PP3V3_GPS, the switched GPS antenna bias rail |
| S12TP5 | PP3V3_CAN, the switched CAN rail |
| S12TP6 | PP3V3_K, the switched K-wire 3.3V rail |
| S12TP7 | PP12V_K, the switched K-wire 12V rail |

Expected readings are on the [board test](/assembly/board-test.html) page.

## Status LEDs

Three red 0603 LEDs, S13D1-S13D3, each driven high by a Connect Kit GPIO through a 1K resistor. The
firmware turns them all off while the tracker sleeps, and by default does not flash them on
accelerometer wakes (`CONFIG_APP_LED_ACCEL_WAKE`), since a parked unit that blinks both wastes the
battery and advertises where it is.

## Antenna

The board has two antenna ports, each a 50 ohm SMA jack, with the GPS port at the top and the LTE
port at the bottom. The recommended antenna is the
[Taoglas MA310.A.LB.001](https://www.taoglas.com/product/ma310-a-lb-001-magnet-mount-gps-glonass-smam-4g-lte-cellular-smam-3m-rg-174/),
a combined GPS/GLONASS and LTE antenna that works well with l0destar. Any antenna meeting the
specification below should do.

**Both ports**

- 50 ohm, with a standard SMA plug on the cable - **not RP-SMA**.
- -40 to +85°C, and IP67 if the antenna is mounted outside the vehicle.

**LTE port - passive**

- Covers your operator's bands. In the UK that means B20 (800MHz), B8 (900MHz) and B3 (1800MHz); a
  698-2690MHz or "penta-band cellular" antenna covers everything you need.
- Vertical polarisation, omnidirectional.
- Keep the cable as short as practical: a 3m RG-174 lead costs roughly 2-3dB against a short one.

**GNSS port - active, 3.3V**

- GPS L1 (1575.42MHz), right-hand circular polarisation.
- The low-noise amplifier's supply range must include about 2.9V: the board feeds 3.3V through a
  15 ohm resistor, so the antenna sees a little less.
- LNA current no more than 30mA. The 3.3V supply behind the switched GPS, CAN and K-wire rails is
  shared.
- A typical LNA gain of about 25dB and noise figure of about 3dB are fine.
- The bias is on a switched rail that the firmware turns on only while GNSS is in use, so the
  antenna draws nothing while the tracker is parked.

!!! danger "Never fit a passive or DC-grounded antenna to the GNSS port"
    It shorts the bias supply through the feed resistor, and GNSS will not work.

## Electrical and mechanical figures

| Item | Value |
|---|---|
| Supply | 12V nominal automotive, two feeds: permanent live and ignition |
| Maximum input | 40V, bounded by the K-wire 12V load switch; the buck itself is rated to 42V |
| Transient protection | 33V TVS on each input, sized for ISO 7637-2:2011 pulse 2a at its maximum level; unsuppressed load dump (ISO 16750-2 test A) is out of scope |
| Fusing | 2A time-lag on each 12V input on the board, as a backstop; external 2A harness fuses on both feeds are required |
| Module rail | 4.2V (4.24V nominal) from an LT8609A synchronous buck at 2MHz |
| Over-voltage protection | Trips at about 4.95V and releases at about 4.80V; latches off while the unprotected rail stays above the release threshold, so clearing a trip needs the input power to drop far enough for the buck output to fall below about 4.8V |
| Sleep current | about 35.5µA at 12V expected for a default v3.4 build (measured on a v3.2 board with the accelerometer rework, which is the same circuit for sleep current); fitting the buck enable divider adds about 9µA |
| Sleep consumption | about 0.85mAh per day at 35.5µA |
| Reporting current | Nordic gives the nRF9151's average current in an LTE-M connection as 45mA at the lowest transmit power and 115-125mA at the maximum, 23dBm, at 3.7V: roughly 15-45mA from a 12V input through the buck, taking it as about 85% efficient, with short transmit bursts above that. The author's bench supply showed 15-25mA while the tracker reported |
| Battery thresholds | 11.9V low-battery warning, 13.0V engine running (firmware defaults, configurable) |
| Vehicle interface | Optional, one at a time: classic CAN and CAN-FD (MCP2518FD controller, TCAN3414DR transceiver specified for 2, 5 and 8Mbps) or K-wire (TJA1027T transceiver, K and L lines) |
| PCB | 66.65 x 37.55mm, 1.6mm, 4 layers |
| Mounting | 4 x M2 holes, 2.2mm |
| Smallest passive | 0402 |
| Module | Makerdiary nRF9151 Connect Kit on two 20-pin 2.54mm headers |
| Enclosure | 72.5 x 43.4 x 28.5mm, 3D printed; designed for v3.3, whose board outline v3.4 keeps |

## L-line driving on K-wire boards

`CONFIG_APP_L_SEND_ENABLED`, on by default for v3.3 and v3.4 builds, lets the firmware drive the
K-wire interface's L line. The L pull-down is current-limited to 90mA by an AL5809-90, and the
L_SENSE input (Connect Kit P0.14, an analog input) lets the firmware detect an L wire shorted to
battery before driving it. Not every vehicle needs the L line: the reference vehicle opens its
diagnostic session on the K wire alone.

L_SENSE classifies the line with a threshold, `CONFIG_APP_L_SENSE_LOW_MV` (default 2800mV): a line
pulled low reads about 2.0V, while a line that is high, open or shorted to battery reads at the
3.6V full scale.
