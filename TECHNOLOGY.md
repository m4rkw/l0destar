# Technology selection

## Platform: Nordic Semiconductor nRF9151

- Integrated LTE-M/NB-IoT and GPS in one package.
- Mature ecosystem with high quality SDK, extensive sample code and active support.
- Very power efficient.
- Widely available with no signs of going away any time soon.
- No requirement for an external MCU — all core functionality is contained within a single SiP, reducing cost and improving efficiency.
- Small form factor.
- Hardware-accelerated cryptography.

## Buck converter: ADI LT8609A

The boards fit the LT8609A (`LT8609AIMSE`); the LT8609S was the original
selection and the rationale below is what carried the LT8609 family.

- Very low Iq — contributes essentially zero to the sleep budget, leaving all headroom for the rest of the system.
- 42 V Vin gives clean margin over typical ISO 7637-2 suppressed load dump (~35 V).
- Output current comfortably covers the modem TX peak plus the system rails.
- Relatively low cost.

## Power regulator: not fitted

The nPM1300 was the original selection but is not used on any current board.
The Makerdiary nRF9151 Connect Kit carries its own regulation, so the boards
feed its battery connector from the 4.2 V buck through an LM66100 ideal diode.
That keeps USB-C connectable at any time for firmware updates without
disconnecting vehicle power, which powering VBUS directly would not allow.

## Accelerometer: ST ASM330LHHX-Q1
- Automotive 6-axis IMU (3-axis accel + 3-axis gyro), AEC-Q100 grade 1.
- Same register family as LSM6DSO/X — mature lsm6dso-family Zephyr driver covers it.
- Built-in finite-state machine and machine-learning core for offloaded event classification (harsh-braking, towing, etc.) — keeps the nRF9151 asleep more.
- Tuned for automotive sensor fusion and dead-reckoning use cases.

## CAN interface: Microchip MCP2518FD + transceiver

The TCAN4550-Q1 was the original selection. The boards instead use a separate
MCP2518FD controller over SPI with a discrete transceiver — a TCAN334GDR on
v2.5C/v2.6C, changed to a MAX33041EASA+ from v3.0 for more robust transient
protection, with a pulldown on STBY so the transceiver defaults to normal mode
instead of floating.

- CAN FD controller with a mature Zephyr driver (mcp251xfd).
- Controller sleep drives the transceiver standby pin via `IOCON.XSTBYEN`, leaving the block at roughly 10 µA.
- Splitting controller and transceiver allows the transceiver to be chosen for bus robustness independently of the controller.

## ISO-9141 K-Line: NXP TJA1027T

The L9637D is a dedicated K-Line transceiver for ISO-9141 / ISO 14230
(KWP2000) diagnostics and is what v2.5K and v2.6K fit. From v3.0 it was
replaced by a TJA1027T LIN transceiver, which dramatically simplifies the
circuit — the separate 5 V buck converter and level shifter are no longer
needed.
