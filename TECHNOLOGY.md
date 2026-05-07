# Technology selection

## Platform: Nordic Semiconductor nRF9151

- Integrated LTE-M/NB-IoT and GPS in one package.
- Mature ecosystem with high quality SDK, extensive sample code and active support.
- Very power efficient.
- Widely available with no signs of going away any time soon.
- No requirement for an external MCU — all core functionality is contained within a single SiP, reducing cost and improving efficiency.
- Small form factor.
- Hardware-accelerated cryptography.

## Power pre-regulator: ADI LT8609S buck converter
- 2.5 µA Iq is ~5–10× better than alternatives — contributes essentially zero to the sleep budget, leaving all headroom for the rest of the system.
- Silent Switcher 2 EMI performance matters when you have an LTE-M modem 20 mm away.
- 42 V Vin gives clean margin over typical ISO 7637-2 suppressed load dump (~35 V).
- 3A output — modem TX peak + nPM1300 charging at 800 mA + system rails fits comfortably.
- Relatively low cost.

## Power regulator: Nordic Semiconductor nPM1300
- Designed for nRF91 + cellular — built to handle ~600 mA TX bursts from LTE-M/NB-IoT without rail collapse. The nRF9151 DK uses it as the reference power solution.
- Battery backup mostly solved: integrated Li-Ion/LiPo charger (up to 800 mA, JEITA-compliant) plus fuel gauge support.
- ~600 nA ship mode and dual high-efficiency bucks.
- First-class support in nRF Connect SDK / Zephyr — drivers, fuel gauge library, sample apps.

## Accelerometer: ST ASM330LHHX-Q1
- Automotive 6-axis IMU (3-axis accel + 3-axis gyro), AEC-Q100 grade 1.
- Same register family as LSM6DSO/X — mature lsm6dso-family Zephyr driver covers it.
- Built-in finite-state machine and machine-learning core for offloaded event classification (harsh-braking, towing, etc.) — keeps the nRF9151 asleep more.
- Tuned for automotive sensor fusion and dead-reckoning use cases.

## CAN interface: TI TCAN4550-Q1
- Integrated controller + transceiver + SPI in one 5×5 QFN — halves BOM line count and layout area for the CAN block.
- AEC-Q100 grade 1.
- Built-in ±58 V bus fault protection and ESD.
- Sleep mode with wake-on-CAN (any dominant pulse) and selective wake on configured frame IDs.
- Zephyr driver upstream (tcan4x5x).

## ISO-9141 K-Line: ST L9637D
- Dedicated K-Line transceiver IC for ISO-9141 / ISO 14230 (KWP2000) vehicle diagnostics interface.
