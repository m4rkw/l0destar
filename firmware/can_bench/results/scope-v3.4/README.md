# Oscilloscope captures, 19 September 2026 — v3.4 board, TCAN3414DR

Rigol DHO814 (100 MHz, 625 MSa/s per channel with two channels on, 312.5
MSa/s with four), 10x passive probes on flying leads soldered to the
TCAN3414DR CANH (CH1, yellow), CANL (CH2, blue), TXD (CH3, magenta), RXD
(CH4, blue) and GND pins.  CH1/CH2 at 500 mV/div, both offset −2.0 V.
Traffic from `can_bench/scope/traffic.py`; setup from `scope/setup.py`;
captures and statistics from `scope/rawcap.py` (whole 1 Mpt record read over
SCPI, bits found in software, differential recomputed from the raw samples),
single shots from `scope/shots.py`, loop delay from `scope/loopdelay.py`;
the per-case script output is in `*_stats.txt`.

| file | what |
|---|---|
| `scope_classic_*.png` | 500 kbps, board transmitting: single edge (100 ns/div), 1 µs/div and 2 µs/div single shots, one frame (50 µs/div), a burst of frames (500 µs/div), persistence eye at 1 µs/div |
| `scope_classic_host_*.png` | 500 kbps, adapter transmitting: whole frame and persistence eye |
| `scope_fd2m_*.png` | FD 500 k / 2 M, board transmitting: whole frame, single shot at 200 ns/div, eye at 250 ns/div |
| `scope_fd2m_host_*.png` | FD 2 M, adapter transmitting: frame and eye |
| `scope_fd5m_*.png` | FD 500 k / 5 M, board transmitting: wide (200 µs/div), frame, bits (200 ns/div), eye at 100 ns/div |
| `scope_fd5m_host_*.png` | FD 5 M, adapter transmitting: frame and eye |
| `scope_loopdelay_*.png` | TXD (CH3) and RXD (CH4) with the bus, 50 ns/div, falling and rising TXD edge |

In the board-transmit captures the adapter's ACK bit is also present (one
dominant bit per frame from the far end, CANH 3.5 V / CANL 1.4 V against the
board's 3.0 / 1.0 V), so the persistence eyes show both drivers.

## Differential (CANH − CANL) statistics over the data phase

Single-bit runs found in one 1 Mpt record; "at 75 %" is the value 75 % into
the bit, the earliest sample point in use on either side (board 80 %
nominal and at 2 Mbps, 75 % at 5 Mbps; adapter 80 % nominal, 75 % data).

| case | bits | dominant at 75 % min / mean / max | recessive at 75 % | overshoot peak | undershoot trough | bit width min / mean / max | rise 10-90 % |
|---|---|---|---|---|---|---|---|
| 500 kbps, board TX | 140 | 1.96 / 1.99 / 2.12 V | 0.01 V | 2.18 V | −0.36 V | 1987 / 1998 / 2013 ns | 26 / 30 / 80 ns (80 = adapter ACK edges) |
| 500 kbps, adapter TX | 334 | 1.96 / 2.11 / 2.13 V | 0.01 V | 2.17 V | −0.41 V (one bit; mean −0.02) | 1994 / 1998 / 2104 ns | 29 / 78 / 83 ns |
| FD 2 Mbps, board TX | 590 | 1.96 / 1.98 / 2.00 V | 0.00 V | 2.17 V | −0.36 V | 486 / 500 / 514 ns | 26 / 27 / 29 ns |
| FD 2 Mbps, adapter TX | 2411 | 2.08 / 2.11 / 2.13 V | 0.01 V | 2.17 V | −0.03 V | 494 / 499 / 502 ns | 72 / 78 / 83 ns |
| FD 5 Mbps, board TX | 495 | 1.96 / 1.99 / 2.01 V | −0.02 V | 2.17 V | −0.36 V | 187 / 200 / 213 ns | 26 / 27 / 30 ns |
| FD 5 Mbps, adapter TX | 3484 | 1.96 / 2.00 / 2.03 V | 0.05 V | 2.17 V | −0.03 V | 195 / 200 / 203 ns | 67 / 71 / 75 ns |

Single-ended levels, board transmitting: recessive CANH = CANL = 2.2 V;
dominant CANH 2.9–3.0 V, CANL 1.0 V.  After each dominant bit both lines
drop together to about 1.4–1.6 V and drift back to the bias over several
microseconds with zero differential (common-mode recovery through the
receiver bias resistors; invisible to receivers).

## Transceiver loop delay (CH3 = TXD pin 1, CH4 = RXD pin 4, 500 kbps, board transmitting)

232 edges each direction from one record at 312.5 MSa/s; thresholds 1.65 V on
TXD/RXD, 0.9 V on the differential.

| edge | TXD → bus | bus → RXD | TXD → RXD loop |
|---|---|---|---|
| recessive → dominant (TXD falls) | 38 / 40 / 42 ns | 45 / 46 / 48 ns | 83 / 86 / 86 ns |
| dominant → recessive (TXD rises) | 51 / 53 / 54 ns | 51 / 54 / 54 ns | 106 / 107 / 109 ns |

(min / mean / max.)  Loop-delay asymmetry 21 ns: the dominant bit starts
40 ns late on the bus and ends 53 ns late, so it is about 13 ns longer than
the TXD pulse that produced it.  Datasheet: tPROP(LOOP1) 95 ns typ / 180 max,
tPROP(LOOP2) 120 ns typ / 180 max, pulse skew 18 ns typ / 28 max.
Screenshots `scope_loopdelay_fall.png` / `_rise.png`.
