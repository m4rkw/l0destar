# Decision Record

A log of key decisions made during the project and the reasoning behind them.

## 21/08/2026 - Separate the CAN and K-line aux switching to separate GPIOs

- Simpler PCB design, fewer jumpers needed
- 99% of builds will only ever use one OBD technology so component count is largely unchanged

## 04/08/2026 - Switched to load switches to gate the aux power rails

- Overall reduced PCB footprint, simpler design
- A single load switch replaces two MOSFETs and multiple ideal diodes
- Aux power rails for OBD are diverted to the right circuits with jumper pads

## 04/08/2026 - Switched to MAX33041 for the CAN transceiver

- Higher peak voltage tolerance than the TJA1051T-3, more robust in the face of automotive transients

## 04/08/2026 - Switched to TJA1027T/20 for the K-wire (ISO 14230-1 K-line) interface

- Uses 3.3v logic - massively simplifies the PCB, no longer need a second 5V buck converter or the level shifter

## 04/08/2026 - Re-integrated the CAN and K-line circuits into a single configurable PCB

- With v3.0 the board is now configurable, the external PINs can be configured for either CAN or K-line and only the components necessary for the desired OBD interface need to be placed.
- This simplifies everything - one PCB layout that can support one or the other (or both), and the active one is set by bridging jumper pads with solder or 0R resistors

## 02/08/2026 - Decided to abandon the bare chip future board

- Started working on a bare chip version of the board without the Makerdiary Connect Kit as a baseline and quickly realised it was a pointless and needlessly complex endeavour. Using the Connect Kit allows a whole section of stuff to be placed underneath it which consolidates the board into a smaller footprint than anything I can make myself without it.

## 31/07/2026 - Split the CAN and K-line versions out into separate boards

- Cars only have one or the other so a board with both doesn't make sense
- Cheaper to build for the one system you actually need to support
- Smaller PCB footprint

## 25/07/2026 - Switched to a 4-layer stackup

- This is generally indicated for boards with RF functionality and makes routing a lot easier and cleaner

## 23/07/2026 - Dropped the general-purpose external AIO pins

- These featured on the Polaris tracker but their intended use is industrial control environments
- For an enthusiast vehicle tracker I can't see this being much use so decided to remove them

## 23/07/2026 - Switched the component baseline from 0805 to 0402

- Got more proficient at microsoldering, 0402 seems like a better baseline
- Allows a smaller overall PCB footprint while still being easily assembled by hand

## 22/07/2026 - Switched the CAN transceiver from TJA1051T-3 to TCAN334G

- Done for availability reasons

## 29/06/2026 - Switched to the MCP2518FD CAN controller

- Purely because it's more easily available

## 18/06/2026 - Switched to SMD

- I had originally thought through-hole would be easier but after getting a cheap rework station and giving it a go it's actually much easier
- This also massively expands the available range of parts and dramatically reduces the overall size of the board
- To maintain ease of assembly I'm intending to stick with 0805 or bigger components.

## 18/05/2026 - Build initial prototypes as through-hole PCBs

- Keeps the design simple
- Easy for anyone to assemble by hand
- End goal is to create a fully custom SMD PCB design that can be fabbed by a 3rd party such as JLCPCB

## 13/05/2026 - Use 3.3V logic on the MCU

- Makes the architecture much simpler.
- Cuts component count and cost.
- Power efficiency gains from a lower VDD are likely insignificant.

## 04/05/2026 - Use the Nordic Semiconductor nRF9151 as the platform

- Integrated LTE-M/NB-IoT and GPS in one package.
- Mature ecosystem with high quality SDK, extensive sample code and active support.
- Very power efficient.
- Widely available with no signs of going away any time soon.
- No requirement for an external MCU - all core functionality is contained within a single SiP, reducing cost and improving efficiency.
- Small form factor.
- Hardware-accelerated cryptography.
