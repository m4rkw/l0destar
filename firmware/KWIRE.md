# K-wire (ISO 14230-1) Interface

**Before building, installing or relying on any of this, read the [project disclaimer](../DISCLAIMER.md).**

The K-wire is the single-wire bidirectional diagnostic line on OBD pin 7,
with the optional L wire on pin 15.  Its physical layer is ISO 14230-1 and
the protocol the tracker speaks over it is KWP2000 (ISO 14230).  Earlier
documentation called this interface "ISO-9141"; that was inaccurate — ISO
9141-2 is a different, older protocol that happens to share the wire and the
5-baud wake-up — and nothing here uses it.  Code and Kconfig symbols keep
the `kline` / `KLINE` names, which are the common shorthand for the same wire.

This document covers two things:

1. the **bench loopback test** (`CONFIG_APP_KLINE_TEST`), which verifies the
   transceiver and pins with no vehicle attached;
2. the **vehicle session init** (`CONFIG_APP_KLINE_INIT_AT_BOOT`), which opens
   a KWP2000 session with a real ECU at boot and reports how, and the
   configuration that does so on the reference vehicle.

## Hardware

| Board | Transceiver | Rails |
|-------|-------------|-------|
| v2.5K / v2.6K | L9637D + TXS0104E level shifter | AUX (shifter A side) + K_EN (L9637D 5 V / 12 V) |
| v3.0 | TJA1027T LIN transceiver | shared OBD domain, SLP_N (K_SLEEP) raised to wake |
| v3.1+ | TJA1027T | own K_EN domain: PP3V3_K + PP12V_K |

On v3.x the K wire has a 510 Ω pull-up to PP12V_K through a diode (S10D1 +
S10R3) and a 33 V TVS; the transceiver's bus pin sits directly on the wire.
L has the same pull-up and a 2N7002 pulldown gated by L_SEND.  Pins are
assigned in `Kconfig.boards` (`APP_PIN_K1_TX`, `APP_PIN_K1_RX`, `APP_PIN_K_SLEEP`,
`APP_PIN_L_SEND`).

The TJA1027T has no TXD dominant time-out, only an initial-TXD-low check on
leaving sleep, so it will hold the bus down for the 200 ms bits of a 5-baud
address (checked against the datasheet — a LIN transceiver with a time-out
could not do this).

## How the firmware drives the wire

- The **5-baud address** and the KWP2000 **fast-init wake-up pulse** are
  bit-banged on the K_TX GPIO, with the echo on K_RX checked every bit.
- Every **data byte** at 9600 / 10400 bps goes through **UARTE1**, driven
  through the nrf HAL (`src/hw_kline.c`, `kline_uart_open()`).  Zephyr's UART
  driver only accepts its fixed list of rates and 10400 is not on it, so the
  BAUDRATE register is written directly.  UARTE1 is otherwise unused and is
  non-secure in the TF-M build.  The pins are handed to the UARTE after the
  address has gone out and returned to GPIO afterwards.
- A bit-banged 10.4 k receiver was tried first and is **not good enough**
  against a real ECU: the busy-wait loop lands a few percent off the rate,
  which is exactly enough to misread the last bit of a key byte.  It is kept
  (calibrated at boot) only for the bench loopback and as a fallback.

## Part 1 — bench loopback test

Verifies the K and L circuits using the transceiver's own bus echo (TX
drives the wire, the same chip's RX reads it back).  On bench boards with
two transceivers it streams bytes K1->K2 and K2->K1 across the wire instead.
No external equipment needed beyond a 12 V supply for the K rail.

1. Enable in `local.conf`:
   ```
   CONFIG_APP_KLINE_TEST=y
   ```
2. Build and flash:
   ```
   ./build.sh && ./flash.sh
   ```
3. Reset the board.  Serial output:
   ```
   *** K-WIRE TEST ***
   K1 static: idle=1 TX=0->RX=0 TX=1->RX=1 OK
   K1 loopback: TX=P0.1 RX=P0.3
   K1 loop: 256/256 bytes OK
   L-line: SKIP (L_SEND disabled — the pulldown FET on this board dies into a short to battery)
   PASS: K-wire test
   *** K-WIRE TEST DONE ***
   K-line test complete - halting.
   ```
4. Comment out `CONFIG_APP_KLINE_TEST=y` when done.

What it proves: K_EN rails, the transceiver, and the TX/RX pins.  What it
cannot prove: anything beyond the transceiver's bus pin.  That pin idles high
on the chip's internal pull-up whether or not the wire goes anywhere, so a
passing loopback with an open, swapped or unconnected harness looks identical
to a healthy one.  Two days of the reference-vehicle bring-up went to K and L
being swapped in the harness; the loopback was clean throughout.

## Part 2 — vehicle session init

`CONFIG_APP_KLINE_INIT_AT_BOOT=y` makes the tracker try to open a KWP2000
session once at boot, report the result on the console and as an alert once
the modem is up (priority 1 if a session opened, 0 otherwise), and then start
as normal — including the power-on FOTA check, so a bench build with a lower
version than the fleet's swaps itself back to the production image straight
after reporting.  Nothing is sent over K beyond the init itself; the session
is left to time out on the ECU (P3max, 5 s) when the rails drop.

Stages, each only if the previous got no reply, each gated by Kconfig:

| Stage | Option | What it sends |
|-------|--------|---------------|
| 5-baud init on 0x33 | always | address 0x33 at 5 baud on K (+L if `APP_L_SEND_ENABLED`); expects 0x55, two key bytes, sends ~KB2, expects ~0x33 |
| fast init on 0x33 | `APP_KLINE_INIT_FAST` (default y) | 25 ms low / 25 ms high, then StartCommunication `C1 33 F1 81 66`, once with 5 ms between bytes and once back to back |
| known addresses | `APP_KLINE_INIT_ADDRS` (e.g. `"13,29,58,B4"`) | the 5-baud handshake on each; if one breaks down partway, a raw hex capture of what that address sends, with inter-byte gaps (`APP_KLINE_INIT_ACK` controls whether ~KB2 is sent during capture) |
| sweeps | `APP_KLINE_INIT_SWEEP` (default y) | fast init, then 5-baud init, on every address 0x01-0xFE — up to 15 min |

`APP_KLINE_BAUD` (default 10400) is the rate the ECU's reply is read at and
the tester's bytes sent at.  If the ECU answers but its sync byte does not
decode cleanly, the slow init sends the address again and reads at the other
of 9600 / 10400, so a wrong setting costs one extra attempt.

`APP_KLINE_INIT_DIAG` adds two operator checks before the init for the things
the echo cannot prove: L is held low for 5 s so pin 15 can be metered (expect
~0 V), then K is watched for 20 s while the operator grounds pin 7 to pin 4 by
hand — any edge counted proves a low on the wire reaches the receiver.

### Reference vehicle: 2006 Toyota Harrier 2.4 L (ACU30, JDM)

OBD socket populated on pins 4, 7, 9, 12, 13, 15, 16 — no CAN.  `local.conf`:

```
CONFIG_APP_BOARD_L0DESTAR_V3_1=y
CONFIG_APP_BOARD_HAS_CAN=n
CONFIG_APP_BOARD_HAS_KLINE=y

CONFIG_APP_KLINE_INIT_AT_BOOT=y
CONFIG_APP_KLINE_BAUD=9600
CONFIG_APP_KLINE_INIT_ADDRS="13,29,58,B4"
CONFIG_APP_KLINE_INIT_FAST=n
CONFIG_APP_KLINE_INIT_SWEEP=n
CONFIG_APP_KLINE_INIT_DIAG=n
# L not needed on this vehicle; leave APP_L_SEND_ENABLED at its default (off).
```

What the vehicle does:

- Four ECU addresses answer the 5-baud init — **0x13, 0x29, 0x58, 0xB4** — with
  the ISO 14230-4 key bytes **E9 8F**, each with its own W1 (about 90, 120,
  145 and 95 ms), so they are most likely four separate ECUs.  Which is the
  engine ECU is not yet established.
- Replies are at **9600 baud**, not the standard 10400.
- The OBD functional address **0x33 gets nothing**, and the fast init gets
  nothing on any address.  All four responding addresses have odd parity;
  0x33 does not.
- **K alone is sufficient.**  The L wire is not needed for the init.

Serial output with that configuration:

```
*** K-LINE INIT ***
slow init: address 0x33 on K only, reply at 9600 baud
  no sync byte within 400 ms
slow init: address 0x13 on K only, reply at 9600 baud
  0x13: sync 0x55 after 89 ms at 9600 baud
  key bytes: KB1=0xE9 KB2=0x8F — ISO 14230-4 KWP2000
  ack: 0xEC (expect 0xEC)
...
addresses: 4 of 4 replied, 4 handshakes completed
PASS: K-line communication established via 5-baud init, ECU 0x13 (ISO 14230-4 KWP2000)
*** K-LINE INIT DONE ***
```

The alert that follows reads
`K-line: session via 5-baud init at 9600 baud, ECU 0x13 KB E9 8F (ISO 14230-4 KWP2000) +3 more: 29 58 B4`.

### Bringing up a new vehicle

1. Start with the defaults: `APP_KLINE_INIT_AT_BOOT=y` and nothing else set.
   That tries 0x33 slow and fast, then sweeps every address at 10400 with a
   9600 retry.  Up to 15 minutes; ignition on.
2. Any address that replies is printed as it happens.  Put the responders in
   `APP_KLINE_INIT_ADDRS`, set `APP_KLINE_BAUD` to whatever the sync line
   reported, turn the sweeps and (if unanswered) the fast init off.  A run
   then takes about 20 s.
3. Only enable `APP_L_SEND_ENABLED` if K alone gets no reply *and* the L wire
   has been checked for a short to battery — see "The L line" below.

## Troubleshooting

**No reply on any address, echo clean** — the echo cannot see past the
transceiver.  Check the harness before touching the protocol: continuity from
the board's K terminal to socket pin 7 (and L to 15), and that they are not
swapped.  With the tracker booted and idle, pin 7 to pin 4 should read close
to battery voltage.  `APP_KLINE_INIT_DIAG` lets you ground pin 7 by hand and
see whether the receiver notices.

**Sync byte reads 0xB5 / 0xA5 / 0xC9 with a framing error, key bytes E9 0F or
C9 0F** — wrong data rate.  Those are exactly what a 9600-baud reply looks
like read at 10400.  Set `APP_KLINE_BAUD=9600`.

**Sync 0x55 and key bytes fine, no acknowledge** — the tester's ~KB2 was not
accepted.  Either it was built from a misread KB2 (check for the symptoms
above) or the bit-banged fallback is in use instead of the UARTE.

**"echo: bus stayed high for N ms of the low bits"** — the transceiver is not
driving K.  K_EN rails, SLP_N high, and on v3.x remember the TJA1027T's
initial-TXD-low check: TXD must be high when SLP_N rises, which
`kline_power_on()` arranges.

**"bus busy before init"** — something else is transmitting: a previous
address's ECU still talking, or another tester on the wire.

**Loopback K1 static FAIL (TX=0 -> RX=1)** — RX doesn't follow TX through the
transceiver: K domain not up ("K domain on" in the log), correct board
variant selected, transceiver populated and powered.

**Loopback byte mismatches** — bit-timing drift in the bit-banged path;
check that no high-priority interrupt is starving the loop.

## Protocol notes (ISO 14230-2, 5-baud init)

| Window | Meaning | Spec | Used here |
|--------|---------|------|-----------|
| W5 | bus idle before the address | ≥ 300 ms | 300 ms, verified high |
| — | address byte | 5 baud, 200 ms/bit, 8N1 | bit-banged, echo checked |
| W1 | end of address -> sync byte | 60-300 ms | wait up to 400 ms |
| W2 | sync -> KB1 | 5-20 ms | 50 ms timeout |
| W3 | KB1 -> KB2 | 0-20 ms | 50 ms timeout |
| W4 | KB2 -> ~KB2, and ~KB2 -> ~address | 25-50 ms | 30 ms, 60 ms timeout |
| P3max | idle before the ECU drops the session | 5 s | session left to expire |

Key bytes: KB2 = 0x8F identifies ISO 14230-4; KB1 (0xE9, 0x6B, 0x6D, 0xEF)
lists the header formats and timing the ECU supports.  08 08 or 94 94 would
mean an ISO 9141-2 ECU, which this firmware recognises but does not talk to.

Fast init (ISO 14230-2): Tinil 25 ms low, Twup 50 ms from the falling edge to
the first request bit, then StartCommunication with P4 5-20 ms between bytes
and the response within P2 25-50 ms.

## The L line

Driving the L line is gated by `CONFIG_APP_L_SEND_ENABLED`, and it is **off
by default on every board before v3.3**. Those boards switch the L pulldown
FET (2N7002) straight across the wire: an L wire shorted to battery looks
exactly like a healthy idle one (both sit at 12-16 V), and asserting L_SEND
then saturates the FET into the short, where it dissipates 1-12 W in a SOT-23
and fails inside the first address bit of a 5-baud init - sometimes gate-first,
putting battery voltage on the L_SEND GPIO and taking the nRF9151 with it.
`../hardware/l0destar_v3.2/README.md` has the full write-up. With the gate off
the pin is still parked low (FET off) and `kline_l_send()` returns `-EPERM`, so
nothing in firmware can assert it; the cost is the L half of a 5-baud init.

v3.3 fixed the hardware - an AL5809-90 in series limits the pulldown to 90 mA
with thermal shutdown - and added **L_SENSE**, a tap on the wire through a
1N4148 (cathode to L) and a 47K. The diode blocks the vehicle's 12 V from ever
reaching the pin, so the line can only be read by sourcing current into it:
firmware samples it on the SAADC with the internal pull-up resistor ladder
engaged (~400K to VDD).

| L wire | Reading |
|--------|---------|
| pulled low (our FET, or an ECU) | diode conducts, ~0.7-0.9 V |
| high, open, or shorted to battery | diode reverse biased, runs to the 3.6 V full scale |

`CONFIG_APP_L_SENSE_LOW_MV` (default 1500) splits the two. A plain GPIO input
cannot: the nRF's ~13K internal pull-up against the 47K leaves even a grounded
line at ~2.7 V, above VIH. Zephyr's ADC driver hard-codes the ladder to bypass,
so `src/hw_kline.c` drives the SAADC through nrfx.

`kline_l_line_probe()` pulses the pulldown for 5 ms and reports whether the
line actually went low - a line that stays high is being held up by something
low-impedance, i.e. shorted to battery, and the init must not be attempted.

**Pin requirement:** the SAADC can only sample AIN0-AIN7, which are P0.13 to
P0.20 on the nRF9151. v3.3 puts L_SENSE on P0.14 (AIN1) and moves the
PP3V3_GPS rail sense, which only ever needed a digital read, to P0.05.
`kline_l_sense_init()` rejects any `CONFIG_APP_PIN_L_SENSE` outside that
range ("L_SENSE on P0.x has no SAADC channel") and the sense reports
unavailable.

## Signal path (v2.6K)

```
nRF P0.0 (K1_TX) --> TXS0104E A2/B2 --> L9637D TxD --> K-line
                                                          |
nRF P0.1 (K1_RX) <-- TXS0104E A1/B1 <-- L9637D RxD <----+
```

Both AUX (TXS0104E VCCA) and K (L9637D Vs, TXS0104E VCCB) domains must
be powered for the signal path to work.
