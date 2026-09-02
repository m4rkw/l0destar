# l0destar v3.1 CAN interface bench test report

Date: 2 September 2026. Board: l0destar v3.1 (nRF9151 Connect Kit profile),
MCP2518FD CAN FD controller on a 40 MHz crystal, MAX33041EASA+ transceiver,
NUP2105L bus protector, 220 R series resistors on CAN_SDI / CAN_SCK, 1 k on
CAN_CS. Bench conditions: clean USB-derived supply, two-node bus, short
cable, 120 R termination at both ends (60 R measured across CANH/CANL).

Everything here is repeatable with the code left in `can_bench/` and
`src/can_bench.c`; see `can_bench/README.md` for the procedure. The raw
measurements are in `can_bench/results/<timestamp>.json` / `.md`.

## 1. Summary

The CAN hardware on the v3.1 board is sound. Across roughly 60 000 frames in
classic and FD modes, at every bit rate from 125 kbps to 1 Mbps nominal and
1 to 8 Mbps FD data phase, the controller, transceiver, crystal and SPI
wiring produced no data corruption and no bus errors that were not
deliberately provoked. The bit-rate tolerance window is symmetric around
500 kbps, so the crystal is on frequency. The transceiver standby / wake
path works: bus activity wakes the sleeping controller within the first
frame and asserts CAN_INT. Rail switching and re-initialisation work
repeatably.

The deficiencies found are all in software, and all matter for a vehicle:

1. **The bit-banged SPI driver is the bottleneck.** With the production
   driver's timing (Zephyr GPIO API with 1 µs waits) the SPI link runs at
   about 138 kbit/s, so the board can absorb roughly 500 classic frames/s
   and transmit about 440 frames/s. A busy 500 kbps vehicle bus carries
   2000 to 4000 frames/s, so the board would overflow its 32-frame RX FIFO
   in well under 100 ms unless it filters. Driving the same pins directly
   (no Zephyr GPIO calls, no waits) is 6 to 7 times faster and keeps up
   with about 3300 frames/s, close to the bus maximum, with no loss of
   SPI data integrity.
2. **Acceptance filtering is essential** for the same reason: with a
   filter set for one ID the controller passed only the matching traffic.
   The production driver must configure filters before entering Normal
   mode on a vehicle bus.
3. **A classic-only configuration is destructive on a CAN FD bus.** In
   Normal CAN 2.0 mode the controller error-flags every FD frame (each one
   raised the receive error counter and the host saw a storm of error
   frames). If the production driver is not certain the vehicle bus is
   classic-only, it should use Normal FD mode (which accepts both) or
   Listen-only mode.
4. **One-shot transmit needs explicit FIFO recovery.** After a failed
   single attempt the controller sets TXATIF and leaves the message in the
   FIFO; everything queued behind it is stuck until the FIFO is reset.
5. **The existing `hw_can.c` test code checks the wrong status bit** for
   TX abort (bit 2, the FIFO-empty flag, instead of bit 6). Harmless in the
   PING test but worth fixing before it is reused.

Two agent bugs found during the work (FD data timing off by one time
quantum at 2 and 4 Mbps; FD RX FIFO sized 64 bytes past the end of message
RAM) were fixed before the final run; they are documented in section 6
because they are exactly the class of mistake a production driver can make
and the symptoms are instructive.

## 2. Test setup

```
  l0destar v3.1 (nRF9151)                     Mac (macOS, Python 3.12)
  +---------------------------+               +----------------------------+
  | src/can_bench.c agent     |   CANH/CANL   | can_test_host.py (root)    |
  | bit-banged SPI ---------> |===============| gs_usb driver, async USB   |
  | MCP2518FD  -> MAX33041E   |  120R  120R   | DSD TECH SH-C31A           |
  +------------+--------------+               | (CANable 2.0, STM32G431)   |
       serial console (115200)                | run_all.py test suite      |
       can_bench/console_log.py               +----------------------------+
```

* **Firmware.** `CONFIG_APP_CAN_BENCH=y` builds `src/can_bench.c`, an agent
  that owns the MCP2518FD and is driven from the host over the bus itself
  (8-byte control frames on 0x7E0, replies on 0x7E8). It counts and
  sequence-checks every other frame, can echo them on ID+1, transmit
  bursts, change mode/bit timing, run internal/external loopback, sleep,
  cycle the CAN rail, benchmark and integrity-check the SPI link, reboot,
  and report the controller's TEC/REC, error-diagnostic and interrupt
  state. The SPI link can be switched at run time between the production
  driver's method (Zephyr GPIO API + `k_busy_wait(1)`) and direct
  `nrf_gpio` register access with no waits.
* **Adapter.** The SH-C31A shipped with a candleLight build that does not
  advertise CAN FD (its feature bits: LISTEN_ONLY, LOOP_BACK, HW_TIMESTAMP,
  IDENTIFY, USER_ID, PAD_PKTS; vendor documentation says FD needs the
  slcan firmware). It was reflashed over DFU with the community gs_usb FD
  fork `tymmothy/candleLight_fw_canable_v2_fd` v1.0.1, which advertises
  FD, BT_CONST_EXT, ONE_SHOT, LISTEN_ONLY and LOOP_BACK on a 160 MHz CAN
  clock. All images and the DFU tools are in `can_bench/adapter_firmware/`.
  This fork has quirks that cost most of the debugging time (128-byte
  padded USB frames, retry-forever jamming, echo-on-queue, no hardware
  timestamps, coarse bit-timing limits); the host driver works around them
  and `can_bench/README.md` lists them.
* **Host software.** `can_bench/gs_usb_fd.py` is a pyusb gs_usb driver with
  FD and libusb asynchronous IN transfers (needed: one synchronous read per
  frame lost about a quarter of the frames at 500 frames/s). `server.py`
  runs as root and exposes the adapter over a localhost socket;
  `client.py` / `device.py` / `run_all.py` are unprivileged.

## 3. What was tested

| id | test | what it exercises |
|---|---|---|
| A1 | identification | mode, OSC, CAN_INT idle level, adapter capabilities |
| A2 | SPI link | 2 KB message RAM sweep with 5 patterns (10 240 bytes) plus 200 timed 16-byte write/read/compare transactions, slow and fast SPI |
| A3 | CAN_INT | interrupt line low exactly while the RX FIFO is non-empty |
| B1 | frame integrity | DLC 0-8 with 00/FF/55/AA/random data, standard IDs 0x000-0x7EE, extended IDs 0x0-0x1FFFFFFE, host -> device -> host echo |
| B1r | RTR | remote frames received and counted |
| B2 | latency | 200 echo round trips, slow and fast SPI |
| B3 | RX rate limit | paced host stream, gap stepped down until the device loses frames |
| B3b | RX FIFO | full-rate bursts of 16/32/60/100 frames against the 32-deep FIFO |
| B4 | TX rate | 500-frame device bursts, DLC 8 and 0, slow and fast SPI, host checks every sequence number |
| B5 | arbitration | host stream and device burst at the same time |
| B6 | counters | TEC/REC/diagnostics after the classic tests |
| C1 | bit rates | 125 k / 250 k / 500 k / 1 M, echo + burst at each |
| C2 | sample point | host sample point swept 60 % to 95 % against the device's 80 % |
| C3 | tolerance | host bit rate offset in ~0.4 % steps from -7 % to +7 % |
| D1 | FD integrity | lengths 0-64 (all DLC codes), BRS, extended IDs, classic frame in FD mode |
| D2 | FD data rates | 1 / 2 / 4 / 5 / 8 Mbps data phase with 500 kbps arbitration |
| D3 | FD throughput | 200 x 64-byte frames each way at 2 and 5 Mbps |
| D4 | FD RX rate limit | as B3 with 64-byte frames |
| D5 | misconfiguration | classic-mode device on a bus carrying FD frames |
| D6 | transceiver FD path | device external loopback through the MAX33041E at 1-8 Mbps with the host silent; internal loopback for comparison |
| D7 | TDC | 5 Mbps with transmitter delay compensation off |
| E1 | no partner | one-shot abort, retry-until-partner-returns, error counter decay |
| E2 | bus-off attempt | same ID with different data from both ends at once |
| E3 | listen-only | device must not ACK |
| E4 | filter | acceptance filter on one standard ID |
| E5 | overflow | 4-deep FIFO overflowed on purpose, flag and recovery |
| E6 | sleep / wake | controller Sleep with transceiver standby (production configuration) and with the transceiver awake, bus traffic during sleep |
| E7 | rail cycle | PP3V3_CAN switched off/on five times, rail sense timing, re-init |
| E8 | reboot | cold reboot, agent comes back |
| E9 | soak | 2 minutes of paced traffic with echo, every frame checked |

## 4. Results

Final run `can_bench/results/20260902-090105` (E1 from `20260902-090159`,
re-run after the agent's one-shot recovery was fixed). "slow" is the
production driver's SPI method, "fast" direct GPIO. PASS/FAIL tests have a
built-in criterion; INFO tests are measurements.

| id | result | key numbers |
|---|---|---|
| A1 | PASS | Normal CAN 2.0, OSC 0x460 (crystal, no PLL), CAN_INT idle high, TEC/REC 0 |
| A2 | PASS | 10 240-byte RAM sweep and 200 write/read/compare: 0 errors, slow and fast. 16-byte transaction: slow 1.0-1.1 ms (127-138 kbit/s on the wire), fast 0.15-0.2 ms (760-900 kbit/s) |
| A3 | PASS | CAN_INT low on every one of 30 frames while the FIFO held data, high otherwise |
| B1 | PASS | 53/53 frames echoed bit-exact (DLC 0-8, five data patterns, 4 standard and 4 extended IDs) |
| B1r | PASS | 5/5 RTR frames received and flagged as remote |
| B2 | PASS | round trip host->device->host, 200 frames: slow 6.3 / 6.8 / 8.1 / 9.7 ms (min / median / p95 / max), fast 2.5 / 3.0 / 3.3 / 5.0 ms, 0 lost |
| B3 | INFO | max lossless RX rate, 8-byte frames: slow 500 frames/s (loss starts at 535/s), fast 2500 frames/s (loss at 3100/s; an earlier run reached 3325/s) |
| B3b | PASS | full-rate bursts of 16 and 32 frames absorbed without loss; 60 and 100 overflow (FIFO is 32 deep) with the overflow flag raised |
| B4 | PASS | 500-frame bursts, every sequence number received: slow 350-440 frames/s (DLC 8), 420-550 (DLC 0); fast 2000-3300 (DLC 8), 2400-4300 (DLC 0); TEC 0 |
| B5 | PASS | host 200 frames at 3 ms pace while device sent 200 back-to-back: 200/200 each way, no overflow, no errors |
| B6 | PASS | TEC 0, REC 0, no warning flags, no error diagnostics |
| C1 | PASS | 125 k, 250 k, 500 k, 1 M: 20/20 echoes and 100/100 burst frames at each, counters clean |
| C2 | PASS | host sample point 60, 70, 75, 80, 85, 90, 95 %: 20/20 pings at every setting |
| C3 | INFO | host bit rate offset: 10/10 from -2.44 % to +2.56 %, 0/10 at -3.03 % and +3.23 % and beyond |
| D1 | PASS | 36/36 FD frames echoed bit-exact: every DLC code 0-64 bytes, BRS, two extended IDs, one classic frame in FD mode |
| D2 | PASS | 64-byte BRS echoes, 20/20 at 1, 2, 4, 5 and 8 Mbps. 8 Mbps: 2 data-phase bit errors on the device's own transmissions (retried, all delivered), TEC peaked at 3-5 |
| D3 | PASS | 200/200 64-byte frames each way at 2 and 5 Mbps: device->host slow 156 frames/s, fast 875 frames/s; host->device at 4 ms pace 100/100 |
| D4 | INFO | max lossless RX rate, 64-byte FD frames: slow 500 frames/s (loss at 615/s), fast 2951 frames/s = bus-limited (no loss at zero gap) |
| D5 | INFO | classic-mode device with 10 FD frames on the bus: received 0, REC rose to 25, stuff-error diagnostic set, adapter reported >2000 error frames |
| D6 | PASS | device external loopback through the MAX33041E: 16/16 64-byte frames, 0 mismatches, TEC 0 at 1, 2, 4, 5 and 8 Mbps; internal loopback identical |
| D7 | INFO | 5 Mbps with TDC off: 50/50 delivered, no data-phase errors (short bus) |
| E1 | PASS | one-shot with no partner: 6 attempts aborted, TEC 56, ACK-error diagnostic; auto-retry: frame delivered the moment the partner returned, TEC had reached 126 (TXWARN), fell to 65 after 100 good frames |
| E2 | PASS | 400 same-ID frames from each end at once: 0 collisions in the data field, no bus-off reached (see section 7); device kept responding |
| E3 | PASS | listen-only device: adapter reported ACK errors, device sent nothing; 3 frames counted only after the adapter went error-passive (passive error flags) |
| E4 | PASS | filter 0x123: 20/20 matching frames received, 40 non-matching rejected; accept-all restored 20/20 |
| E5 | PASS | 4-deep FIFO, 50-frame burst: 9 received, 5 overflow events flagged, normal operation afterwards 20/20 |
| E6 | INFO | 2 s Sleep with transceiver standby (production config): WAKIF set and CAN_INT asserted 9 ms after the first bus frame (sent at 500 ms), oscillator ready 396 µs after wake, controller in Configuration mode after wake. Identical with the transceiver awake |
| E7 | PASS | 5 rail cycles (300 ms and 1.5 s off): rail sense low 17-18 ms after switch-off, controller re-initialised 24-25 ms after switch-on, echo OK after each |
| E8 | PASS | cold reboot: agent back on the bus in 2 s |
| E9 | PASS | 120 s at 186 frames/s with echo: 22 300 sent, 22 300 received in sequence, 22 300 echoed and verified, 0 overflow, TEC/REC 0, 0 adapter error frames |

## 5. Findings in detail

### 5.1 Hardware: no faults found

* **SPI wiring through the series resistors.** 10 240 bytes of message RAM
  written and read back with five patterns, plus 200 timed write/read
  pairs, with zero errors at both the production timing (about 130 kbit/s)
  and the direct-GPIO timing (about 800 kbit/s, SCK edges a few hundred
  nanoseconds apart). The 220 R / 1 k series resistors do not limit the
  link at these speeds. (Whether hardware SPIM at 8-10 MHz would work
  through them was not tested; see section 7.)
* **Controller and crystal.** Every bit rate the OBD/J1939 world uses
  (125 k to 1 M) works with clean counters. The host adapter's bit rate
  could be pulled 2.44 % low or 2.56 % high before the link broke, and the
  break is symmetric within the 0.4 % step size, so the 40 MHz crystal
  with its 27 pF loads is centred to well within half a percent. The host
  sample point could sit anywhere from 60 % to 95 % against the device's
  80 %.
* **Transceiver.** The MAX33041E handled the data phase at 1, 2, 4, 5 and
  8 Mbps in both directions on the bench bus, including the device's own
  external loopback with the adapter silent. At 8 Mbps the device logged
  two data-phase bit errors on its own transmissions in one 20-frame run
  (retried, all delivered); 8 Mbps is above the transceiver's 5 Mbps
  rating and outside anything a vehicle uses, so this is a curiosity, not
  a finding.
* **CAN_INT.** Wired and working: low exactly while the RX FIFO holds a
  frame, and asserted on wake-up.
* **Rail switching.** PP3V3_CAN drops below the sense threshold 17-18 ms
  after CAN_EN goes low (light load on the rail) and the MCP2518FD is back
  in Configuration mode with its oscillator running 24 ms after CAN_EN
  goes high, five times out of five, with the bus working immediately
  afterwards. The rail-sense pin reads the real rail state.
* **Sleep and wake-on-bus.** With the controller in Sleep and the
  MAX33041E in standby through XSTBY (the production configuration),
  the first frame on the bus set WAKIF and pulled CAN_INT low within
  about 9 ms of the frame; the oscillator was ready 396 µs after wake.
  Behaviour was identical with the transceiver awake, so the standby
  receiver path works.
* **Error handling.** ACK errors, retry-until-partner-returns, error
  warning, one-shot abort, RX overflow, acceptance filtering and
  listen-only all behaved as the datasheet says, and the counters decayed
  back to zero with good traffic.

### 5.2 The SPI driver is the throughput limit

| direction, frame | production SPI (Zephyr GPIO + 1 µs waits) | direct GPIO |
|---|---|---|
| receive, 8-byte classic | 500 frames/s lossless, overflow from ~535/s | 2500-3300 frames/s |
| transmit, 8-byte classic | 350-440 frames/s | 2000-3300 frames/s |
| receive, 64-byte FD | 500 frames/s | 2950 frames/s (bus-limited) |
| transmit, 64-byte FD | 156 frames/s | 875 frames/s |
| echo round trip | 6.8 ms median | 3.0 ms median |

A saturated 500 kbps bus carries about 3600 eight-byte frames per second;
typical passenger-car powertrain buses run 40-80 % load. With the
production driver the 32-frame RX FIFO fills in about 10 ms of bus-rate
traffic and frames are then lost (the overflow flag is raised reliably,
so the firmware would at least know). Direct GPIO access with no waits
gets within a factor of about 1.3 of the bus rate with identical data
integrity. The nRF9151's hardware SPIM would remove the CPU cost as well;
it was not tried because the production pin-parking scheme is built
around GPIO control of these pins.

Whatever the driver, the firmware should not rely on receiving everything:
hardware acceptance filters (E4) reduce the load to the IDs of interest,
and the RX FIFO should be configured as deep as the message RAM allows
(32 objects) rather than the 4 used by the boot-time test.

### 5.3 Mode choice on a vehicle bus

* **FD tolerance.** D5 shows that a controller in Normal CAN 2.0 mode
  cannot coexist with FD traffic: it error-flags every FD frame, which
  destroys the frame for every node on the bus, and its own receive
  error counter climbs. Modern vehicles increasingly carry FD frames even
  on the diagnostic connector. Normal FD mode received classic frames
  without issue in every FD test, so it is the safer default; Listen-only
  is the safest if the tracker only needs to observe.
* **Listen-only on a live bus** is genuinely passive (E3): the adapter saw
  no ACK from the device. Note the corollary observed on the two-node
  bench: a lone transmitter's frames are only valid to a passive listener
  once the transmitter has gone error-passive, because active error flags
  destroy them. On a real bus other nodes ACK, so this does not apply.
* **One-shot transmit** (needed if the tracker must never retry into a
  bus it does not understand) works, but after an aborted attempt the
  message stays in the FIFO with TXREQ clear and blocks everything queued
  behind it; the driver must reset the FIFO (FRESET) when it sees that
  state. TXATIF was not observed to be set in this condition.

### 5.4 Existing firmware

* `hw_can.c` `hw_can_test()` and `mcp_send_std()` test bit 2 of
  C1FIFOSTA for "TX aborted"; bit 2 is TFERFFIF (FIFO empty) and TXABT is
  bit 6. The check never fires falsely in the PING test but reports
  nothing useful either.
* The boot-time init sequence, XSTBY configuration, rail request/release
  and sleep entry in `hw_can.c` were exercised by every rail cycle and
  reboot in this suite and are sound.

## 6. Agent bugs found on the way (instructive)

* **FD data bit timing.** The agent's table had TSEG2 one quantum too long
  at 2 and 4 Mbps (21 and 11 time quanta per bit instead of 20 and 10, so
  the device's data phase ran 5-10 % slow). Symptoms: the device's own
  loopback passed (it agreed with itself), but every frame exchanged with
  the adapter at those two rates failed with data-phase stuff errors while
  1, 5 and 8 Mbps were perfect. Rule: check `1 + (TSEG1+1) + (TSEG2+1)`
  against the intended quantum count for every entry.
* **Message RAM overrun.** With receive timestamps enabled an FD RX object
  is 76 bytes, not 72; 4 TX + 24 RX objects came to 2112 bytes in a 2048
  byte RAM. The controller silently wrapped: 2.7 % of FD frames came back
  with the first four data bytes correct and the rest zero, with no error
  flag anywhere. Rule: budget the RAM from PLSIZE, FSIZE and RXTSEN, and
  test payloads longer than four bytes at every FIFO slot.
* **One-shot TX.** Described above; the agent now resets the FIFO on
  TXATIF.

## 7. Oscilloscope measurements (added after the first run)

Rigol DHO814, 10x probes on flying leads soldered to the MAX33041E's CANH,
CANL and GND pins, difference computed on the scope and re-computed from
the raw samples on the host. Whole 1 Mpt records were read over SCPI and
every single-bit-wide run in the data phase was measured; screenshots and
the full table are in `can_bench/results/scope/`, scripts in
`can_bench/scope/`.

| case | bits measured | dominant at the 75 % sample point (min / mean) | recessive | overshoot / undershoot | bit width spread | 10-90 % edge |
|---|---|---|---|---|---|---|
| 500 kbps, board TX | 130 | 1.84 / 1.90 V | 0 V | +2.11 / -0.65 V | ±13 ns | <10 ns (scope-limited) |
| 500 kbps, adapter TX | 79 | 1.91 / 2.15 V | 0 V | +2.22 / -0.08 V | ±5 ns | 68 ns |
| FD 2 Mbps, board TX | 575 | 1.84 / 1.89 V | 0 V | +2.13 / -0.64 V | ±12 ns | <10 ns |
| FD 2 Mbps, adapter TX | 467 | 2.08 / 2.15 V | 0 V | +2.20 / -0.06 V | ±3 ns | 69 ns |
| FD 5 Mbps, board TX | 503 | 1.81 / 1.87 V | 0 V | +2.12 / -0.66 V | ±11 ns | <10 ns |
| FD 5 Mbps, adapter TX | 419 | 2.01 / 2.08 V | 0 V | +2.21 / -0.04 V | ±3 ns | 65 ns |

What this says:

* **The board's driver is clean and fast.** Dominant differential 1.87 to
  1.90 V (CANH 2.98 V, CANL 1.08 V, recessive both 2.16 V), flat from
  about 20 ns after the edge to the end of the bit at every rate, and the
  recessive level is 0 V within 50 mV. At 5 Mbps the worst single bit in
  500 still read 1.81 V at the sample point, against a 0.9 V receiver
  threshold. The eye is fully open at 2 and 5 Mbps.
* **Edges are faster than the 100 MHz scope can resolve**, and the
  dominant-to-recessive transition has a brief undershoot to about
  -0.65 V (a third of the swing, gone within tens of nanoseconds) and a
  ringing of roughly 40 MHz for about 150 ns that appears equally on both
  lines and cancels in the difference. Neither affects decoding, but the
  MAX33041E has no slew-rate control and these edges are what the bus
  cable will radiate: an EMC test on a real harness is the open question,
  not signal margin.
* **What the board receives** from the adapter's transceiver is a slower,
  larger signal (65 ns edges, 2.1 V), which at 5 Mbps leaves about 130 ns of
  settled level before the sample point. The board decoded 100 % of it in
  every FD test.
* **Bit timing:** the board's bit widths scatter ±11 to 13 ns around
  nominal at every rate (the adapter's ±3 ns), which at 5 Mbps is ±5.5 %
  of a bit. This is edge jitter of the MCP2518FD/transceiver pair, well
  inside the resynchronisation range the receiver has, and consistent with
  the ±2.5 % bit-rate tolerance window measured electrically.
* **Common-mode recovery:** after every dominant bit both lines drop
  together to about 1.6 V and drift back to 2.16 V over several
  microseconds with zero differential. This is the receiver bias network
  recharging and is normal; it confused the first persistence view and is
  worth knowing when reading single-ended traces.

**Transceiver loop delay** (probes on the MAX33041E TXD and RXD pins, 223
edges each way at 500 kbps): TXD to RXD is 72.5 ns for the recessive-to-
dominant edge (29 ns TXD to bus, 43 ns bus to RXD) and 60 ns for the
dominant-to-recessive edge (16 ns plus 43 ns), spread under ±2 ns. The
13 ns asymmetry means a dominant bit on the bus is 13 ns longer than the
TXD pulse, 6.5 % of a 5 Mbps bit, within what CAN FD transceivers are
allowed (the datasheet limit is 120 ns loop delay). These are the numbers
to feed a manual TDC offset if the MCP2518FD's automatic measurement is
ever turned off; with TDC in auto mode, as configured here, the controller
measures this delay itself on every frame.

Not measured with the scope: the SPI edges through the series resistors.

## 8. Not testable on this bench

* Signal integrity on a real harness: the measurements above are on a
  short two-node bus; stub reflections, ground offset between ECUs and
  emissions from the fast edges need the vehicle wiring or an EMC bench.
* Crystal accuracy and drift. The ±2.5 % symmetric tolerance window bounds
  the error to well under 0.5 %, but a frequency counter or a long-run
  timestamp comparison is needed for ppm figures, and temperature was not
  varied.
* Propagation delay of the transceiver (for TDC settings on long buses);
  the short bench bus worked even with TDC disabled at 5 Mbps.
* Fault protection of the MAX33041E (±40 V on CANH/CANL, ground offset,
  shorts to battery/ground) and the NUP2105L clamping: needs a controlled
  fault rig.
* Power: sleep and standby currents of the MCP2518FD and transceiver, and
  the current drawn from a real vehicle harness. Needs a meter in the rail.
* EMC, vibration, temperature.
* Multi-node and real-vehicle behaviour: high bus load with other nodes'
  traffic, wake-on-CAN with vehicle-specific frames, and the ISO 15765 /
  OBD request-response protocol, which the firmware does not yet
  implement (`hw_can.c` is explicitly "not a bus driver yet").
* Bus-off: could not be provoked on a two-node bus with this adapter (its
  transmit scheduling never collided with the device's frames in the data
  field), so bus-off recovery of the MCP2518FD was not observed.
* Listen-only on a live bus: on a two-node bench the passive device sees
  no valid frames until the lone transmitter goes error-passive, so
  listen-only reception was only demonstrated indirectly.
