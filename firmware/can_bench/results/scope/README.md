# Oscilloscope captures, 2 September 2026

Rigol DHO814 (100 MHz, 625 MSa/s per channel with two channels on), 10x
passive probes on flying leads soldered to the MAX33041E CANH (CH1, yellow),
CANL (CH2, blue) and GND pins; Math1 (purple) = CH1 - CH2.  Traffic from
`can_bench/scope/traffic.py`; captures and statistics from
`can_bench/scope/rawcap.py` (whole 1 Mpt record read over SCPI, bits found in
software) and `fdcap.py` (screen windows); screenshots via `scope.py`.

| file | what |
|---|---|
| `scope_classic_*.png` | 500 kbps, board transmitting: single edge (100 ns/div), 1 us/div and 2 us/div single shots, whole frames, persistence eye at 500 ns/div |
| `scope_classic_host_*.png` | 500 kbps, adapter transmitting: whole frame and persistence eye |
| `scope_fd2m_*.png` | FD 500 k / 2 M, board transmitting: whole frame, single shot at 200 ns/div, eye at 250 ns/div |
| `scope_fd2m_host_*.png` | FD 2 M, adapter transmitting: frame and eye |
| `scope_fd5m_*.png` | FD 500 k / 5 M, board transmitting: wide, frame, bits, eye at 100 ns/div |
| `scope_fd5m_host_*.png` | FD 5 M, adapter transmitting: frame and eye |

## Differential (CANH - CANL) statistics over the data phase

Single-bit runs found in one 1 Mpt record; "at 75 %" is the value at the
receiver sample point of a 75-80 % configuration.

| case | bits | dominant at 75 % min / mean / max | recessive at 75 % | overshoot peak | undershoot trough | bit width min / mean / max | rise 10-90 % |
|---|---|---|---|---|---|---|---|
| 500 kbps, board TX | 130 | 1.84 / 1.90 / 2.15 V | -0.01 V | 2.11 V | -0.65 V | 1987 / 2001 / 2013 ns | ~10 ns (scope-limited) |
| 500 kbps, adapter TX | 79 | 1.91 / 2.15 / 2.22 V | -0.02 V | 2.22 V | -0.08 V | 1990 / 1997 / 2000 ns | 68 ns |
| FD 2 Mbps, board TX | 575 | 1.84 / 1.89 / 1.95 V | -0.01 V | 2.13 V | -0.64 V | 488 / 500 / 512 ns | 8 ns (scope-limited) |
| FD 2 Mbps, adapter TX | 467 | 2.08 / 2.15 / 2.22 V | 0.00 V | 2.20 V | -0.06 V | 403 / 499 / 502 ns | 69 ns |
| FD 5 Mbps, board TX | 503 | 1.81 / 1.87 / 1.91 V | -0.01 V | 2.12 V | -0.66 V | 189 / 200 / 211 ns | 10 ns (scope-limited) |
| FD 5 Mbps, adapter TX | 419 | 2.01 / 2.08 / 2.18 V | 0.01 V | 2.21 V | -0.04 V | 181 / 200 / 203 ns | 65 ns |

Single-ended levels, board transmitting: recessive CANH = CANL = 2.16 V;
dominant CANH 2.98 V, CANL 1.08 V.  After each dominant bit both lines drop
together to about 1.6 V and drift back to the bias over several microseconds
with zero differential (common-mode recovery through the receiver bias
resistors; invisible to receivers).

## Transceiver loop delay (CH3 = TXD pin 1, CH4 = RXD pin 4, 500 kbps, board transmitting)

223 edges each direction from one record at 312.5 MSa/s; thresholds 1.65 V on
TXD/RXD, 0.9 V on the differential.

| edge | TXD -> bus | bus -> RXD | TXD -> RXD loop |
|---|---|---|---|
| recessive -> dominant (TXD falls) | 26 / 29 / 32 ns | 42 / 43 / 45 ns | 70 / 72.5 / 74 ns |
| dominant -> recessive (TXD rises) | 16 / 16 / 19 ns | 42 / 43 / 45 ns | 58 / 60 / 61 ns |

(min / mean / max.)  Loop-delay asymmetry 13 ns: a dominant bit on the bus is
about 13 ns longer than the TXD pulse that produced it.  Screenshots
`scope_loopdelay_fall.png` / `_rise.png`.
