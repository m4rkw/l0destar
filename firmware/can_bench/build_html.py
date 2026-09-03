import base64, os, json
R = "/Users/mark/code/l0destar/firmware"
IMG = f"{R}/can_bench/results/scope"
def img(name, alt, cap):
    with open(f"{IMG}/{name}", "rb") as f: b = base64.b64encode(f.read()).decode()
    return f'<figure><img src="data:image/png;base64,{b}" alt="{alt}" loading="lazy"><figcaption><span class="tag">{name.replace("scope_","").replace(".png","")}</span> {cap}</figcaption></figure>'

res = json.load(open(f"{R}/can_bench/results/20260902-090105.json"))["results"]
e1 = json.load(open(f"{R}/can_bench/results/20260902-090159.json"))["results"][0]
res = [e1 if r["id"] == "E1" else r for r in res]
KEY = {
 "A1": "Normal CAN 2.0, OSC 0x460, CAN_INT idle high, TEC/REC 0",
 "A2": "2 KB message RAM x 5 patterns (10 240 bytes) + 200 write/read/compare: 0 errors. 16-byte read (18 bytes on the wire) 1.13 ms slow (127 kbit/s), 0.19 ms fast (757 kbit/s)",
 "A3": "CAN_INT low on all 30 frames while the FIFO held data, high otherwise",
 "B1": "53/53 frames echoed bit-exact: DLC 0–8, five patterns, 4 std + 4 ext IDs",
 "B1r": "5/5 remote frames received and flagged",
 "B2": "RTT 200 frames: slow 6.3 / 6.8 / 8.1 / 9.7 ms, fast 2.5 / 3.0 / 3.3 / 5.0 ms (min/med/p95/max)",
 "B3": "max lossless RX: slow 500 f/s, fast 2500 f/s (3325 f/s in an earlier run)",
 "B3b": "16 and 32-frame bursts absorbed; 60 and 100 overflow with the flag raised (FIFO = 32)",
 "B4": "500-frame bursts, all sequence numbers received: slow 350–440 f/s, fast 2000–3300 f/s",
 "B5": "200 host frames at 3 ms pace while device sent 200: 200/200 each way, no errors",
 "B6": "TEC 0, REC 0, no flags, no diagnostics",
 "C1": "125 k / 250 k / 500 k / 1 M: 20/20 echoes and 100/100 burst at each",
 "C2": "host sample point 60–95 %: 20/20 pings at every setting",
 "C3": "lossless from −2.44 % to +2.56 %, fails at −3.03 % and +3.23 % (points ~0.6 % apart)",
 "D1": "36/36 FD frames echoed bit-exact, all DLC codes to 64 bytes, BRS, ext IDs",
 "D2": "20/20 64-byte BRS echoes at 1, 2, 4, 5 and 8 Mbps; 2 bit errors on own TX at 8 M, retried",
 "D3": "200/200 each way at 2 and 5 Mbps: dev→host slow 156 f/s, fast 875 f/s",
 "D4": "max lossless RX, 64-byte frames: slow 500 f/s, fast 2951 f/s (bus limited)",
 "D5": "classic-mode device vs 10 FD frames: received 0, REC 25, stuff-error diagnostic, adapter flooded with error frames",
 "D6": "external loopback through the MAX33041E 16/16 at 1, 2, 4, 5, 8 Mbps, 0 mismatches",
 "D7": "5 Mbps with TDC off: 50/50, no data-phase errors (short bus)",
 "E1": "one-shot: 6 data frames + the report frame aborted (7 × 8 = TEC 56); retry: TEC saturated at 128 (error-passive), delivered on partner return, read 126 after two deliveries and 65 after the next 100-frame burst",
 "E2": "400 same-ID frames from both ends: no data-field collision produced, bus-off not reached",
 "E3": "listen-only: adapter saw no ACK; 3 frames counted only after the adapter went error-passive",
 "E4": "filter 0x123: 20/20 matching, 40 rejected; accept-all restored 20/20",
 "E5": "4-deep FIFO + 50-frame burst: 9 received, 5 overflow events flagged, recovered 20/20",
 "E6": "Sleep + transceiver standby: WAKIF and CAN_INT 9 ms after the first frame, oscillator ready 396 µs after wake",
 "E7": "5 rail cycles: sense low 17–18 ms after off, controller back 24–25 ms after on, echo OK each time",
 "E8": "cold reboot: agent back on the bus in 2 s",
 "E9": "120 s at 186 f/s with echo: 22 300 sent, received, echoed and verified; 0 loss; TEC/REC 0",
}
CLS = {"PASS": "pass", "FAIL": "fail", "INFO": "info", "WARN": "warn", "ERROR": "fail"}
rows = "".join(f'<tr><td class="mono">{r["id"]}</td><td>{r["name"]}</td><td><span class="pill {CLS[r["status"]]}">{r["status"]}</span></td><td>{KEY.get(r["id"], "")}</td></tr>' for r in res)

html = f'''<title>l0destar CAN Bench</title>
<meta name="description" content="Bench and oscilloscope test report for the l0destar v3.1 CAN interface">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@400;700&display=swap">
<style>
:root{{color-scheme:light}}
body{{font-family:'JetBrains Mono',Menlo,monospace;font-size:10pt;color:#333;background:#fff;margin:0}}
#content{{margin:0 auto;max-width:62em;padding:0 2em 5em}}
h1{{font-size:12pt;color:#4a7fa5;margin:1.5em 0 .8em}}
h2{{font-size:11pt;color:#4a7fa5;margin:2.2em 0 .6em}}
h3{{font-size:10pt;color:#4a7fa5;margin:1.6em 0 .3em}}
p{{margin:.8em 0;line-height:1.5}}
a{{color:#4a7fa5;text-decoration:underline}} a:hover{{color:#2d5f80}}
ul,ol{{padding-left:1.5em;margin:.8em 0;line-height:1.5}} ul{{list-style-type:disc}} li{{margin-bottom:.3em}}
code{{background:#f5f5f5;padding:.1em .3em;font-family:'JetBrains Mono',monospace;font-size:.9em}}
table{{border-collapse:collapse;margin:1em 0}} th,td{{border:1px solid #ddd;padding:.4em .8em;text-align:left;vertical-align:top}} th{{background:#f5f5f5}}
.tablewrap{{overflow-x:auto}}
td.mono,.num{{white-space:nowrap}}
.pill.pass{{color:#2d7a4f}} .pill.fail{{color:#b0352a}} .pill.info{{color:#666}} .pill.warn{{color:#a5690e}}
figure{{margin:1.5em 0}} figure img{{max-width:min(100%,900px);height:auto;display:block;margin:0 0 .4em}}
figcaption{{color:#666;line-height:1.5}} .tag{{color:#4a7fa5}}
.key{{color:#666;margin:.5em 0}} .key i{{display:inline-block;width:1.6em;height:3px;vertical-align:middle;margin:0 .4em 0 1em}}
.small{{font-size:9pt;color:#666}}
nav ul{{list-style:none;padding:0;margin:.8em 0 1.5em}} nav li{{display:inline;margin:0 1em 0 0}}
hr{{border:0;border-top:1px solid #ddd;margin:2em 0}}
</style>
<div id="content">
<h1>CAN interface bench test — l0destar v3.1</h1>
<p class="small">MCP2518FD + MAX33041E · 2 September 2026 · <a href="https://github.com/m4rkw/l0destar">source, scripts and raw results in the firmware repository</a></p>
<p>The CAN hardware on the v3.1 board is sound: about 60 000 frames in classic and FD modes, 125 kbps to 8 Mbps, with no corruption and, within the transceiver's 5 Mbps rating, no unprovoked errors (two data-phase bit errors on the board's own 8 Mbps transmissions were the only ones), and a clean eye on the oscilloscope at 5 Mbps. Every deficiency found is in software, and the biggest is the bit-banged SPI link.</p>
<nav><ul><li><a href="#setup">setup</a></li><li><a href="#results">results</a></li><li><a href="#findings">findings</a></li><li><a href="#scope">oscilloscope</a></li><li><a href="#bugs">agent bugs</a></li><li><a href="#limits">not testable</a></li><li><a href="#repeat">repeat it</a></li></ul></nav>
<h2>At a glance</h2>
<table><tbody>
<tr><th>Hardware</th><td>No faults. Controller, crystal, transceiver, SPI wiring, CAN_INT, rail switching and wake-on-bus all behave as the datasheets say.</td></tr>
<tr><th>Bit-rate tolerance</th><td>−2.4 % to +2.6 %, symmetric around 500 kbps to within the ±0.3 % resolution of the test: no gross crystal error (the crystal's ppm spec is far below what this can see).</td></tr>
<tr><th>Throughput, production SPI</th><td>500 frames/s. A 500 kbps bus at 40–80 % load carries 1600 to 3500, saturated about 4400. Direct GPIO reaches 2500 to 3300 with identical integrity.</td></tr>
<tr><th>5 Mbps eye, worst bit of 503</th><td>1.81 V dominant differential at the sample point, against a 0.9 V threshold. Recessive 0 V ± 50 mV.</td></tr>
</tbody></table>

<section id="setup">
<h2>Setup</h2>
<p>Two-node bus on the bench: the board's MAX33041E at one end, a DSD TECH SH-C31A (CANable 2.0, STM32G431) at the other, 120 Ω at both ends (60 Ω measured across CANH–CANL), short cable, USB-derived supply. The board runs a test agent (<code>src/can_bench.c</code>, <code>CONFIG_APP_CAN_BENCH=y</code>) that is driven from the host over the bus itself, so every command and reply is also a CAN exchange.</p>
<ul>
<li><b>Firmware agent</b> — owns the MCP2518FD. Counts and sequence-checks frames, echoes on ID+1, bursts, changes mode and timing, loopbacks, sleeps, cycles the rail, benchmarks the SPI link, reports TEC/REC and diagnostics.</li>
<li><b>Bit-banged SPI, two speeds</b> — production method (Zephyr GPIO API + 1 µs waits, ~130 kbit/s) or direct nrf_gpio with no waits (~800 kbit/s), switchable at run time.</li>
<li><b>Adapter</b> — reflashed with the community gs_usb CAN FD fork; the shipped candleLight build had no FD. Its quirks (128-byte padded USB frames, retry-forever jams, echo-on-queue, 160 MHz clock) are worked around in the host driver.</li>
<li><b>Host software</b> — pyusb gs_usb driver with FD and async USB, a root-side server on a local socket, and the unprivileged test suite <code>can_bench/run_all.py</code>.</li>
</ul>
</section>

<section id="results">
<h2>Results</h2>
<p>Final run <code>can_bench/results/20260902-090105</code>; E1 from the re-run after the agent's one-shot recovery was fixed. PASS and FAIL rows have a built-in criterion; INFO rows are measurements. "slow" is the production driver's SPI method, "fast" direct GPIO.</p>
<div class="tablewrap"><table><thead><tr><th>id</th><th>test</th><th>result</th><th>key numbers</th></tr></thead><tbody>{rows}</tbody></table></div>
</section>

<section id="findings">
<h2>Findings</h2>
<h3>Hardware: nothing to fix</h3>
<ul>
<li><strong>SPI through the series resistors.</strong> The 2 KB message RAM written and read back with five patterns (10 240 bytes) plus 200 timed transactions, zero errors, at both the production timing (127 kbit/s on the wire) and the direct-GPIO timing (757 kbit/s, edges a few hundred nanoseconds apart). The 220 Ω and 1 kΩ resistors do not limit the link at these speeds.</li>
<li><strong>Controller and crystal.</strong> Every bit rate from 125 k to 1 M works with clean counters. The adapter could be pulled 2.44 % low or 2.56 % high before the link broke, symmetric to within the ±0.3 % resolution of the test points, which rules out a gross crystal error but says nothing at the ppm level. Host sample points from 60 % to 95 % all worked against the board's 80 %.</li>
<li><strong>Transceiver.</strong> FD data phase at 1, 2, 4, 5 and 8 Mbps in both directions, including the board's own external loopback with the adapter silent. Two data-phase bit errors on its own 8 Mbps transmissions in one run, above the part's 5 Mbps rating and outside anything a vehicle uses.</li>
<li><strong>Rail switching.</strong> PP3V3_CAN drops below the sense threshold 17 to 18 ms after CAN_EN goes low and the controller is back in Configuration mode with its oscillator running 24 ms after it goes high, five times out of five.</li>
<li><strong>Sleep and wake-on-bus.</strong> With the controller asleep and the MAX33041E in standby through XSTBY, the first bus frame set WAKIF and pulled CAN_INT low within 9 ms; the oscillator was ready 396 µs after wake. Identical with the transceiver awake.</li>
</ul>
<h3>The SPI driver is the throughput limit</h3>
<div class="tablewrap"><table><thead><tr><th>direction, frame</th><th>production SPI</th><th>direct GPIO</th></tr></thead><tbody>
<tr><td>receive, 8-byte classic</td><td>500 frames/s lossless, overflow from ~535/s</td><td>2500 to 3300 frames/s</td></tr>
<tr><td>transmit, 8-byte classic</td><td>350 to 440 frames/s</td><td>2000 to 3300 frames/s</td></tr>
<tr><td>receive, 64-byte FD</td><td>500 frames/s</td><td>2950 frames/s (bus limited)</td></tr>
<tr><td>transmit, 64-byte FD</td><td>156 frames/s</td><td>875 frames/s</td></tr>
<tr><td>echo round trip</td><td>6.8 ms median</td><td>3.0 ms median</td></tr>
</tbody></table></div>
<p>A saturated 500 kbps bus carries about 4000 to 4400 eight-byte standard-ID frames a second (111 bits plus stuffing); passenger-car powertrain buses run at 40 to 80 % load, 1600 to 3500 frames/s. With the production driver the 32-frame RX FIFO fills in 8 to 9 ms of bus-rate traffic and frames are lost, with the overflow flag raised reliably. Direct GPIO with no waits reaches 57 to 75 % of the bus maximum with identical data integrity. Whatever the driver, hardware acceptance filters (E4) should reduce the load to the IDs of interest and the RX FIFO should be the full 32 objects, not the 4 used by the boot-time test.</p>
<h3>Mode choice on a vehicle bus</h3>
<ul>
<li><strong>FD tolerance.</strong> A controller in Normal CAN 2.0 mode error-flags every FD frame (D5), destroying it for every node on the bus while its own receive error counter climbs. Normal FD mode accepted classic frames in every test, so it is the safer default; Listen-only is safest if the tracker only observes.</li>
<li><strong>Listen-only</strong> is genuinely passive (E3). On the two-node bench a lone transmitter's frames only become valid to the passive listener once that transmitter is error-passive; on a real bus other nodes ACK.</li>
<li><strong>One-shot transmit</strong> works, but an aborted message stays in the FIFO with TXREQ clear and blocks everything behind it until the FIFO is reset (E1). TXATIF was not observed to be set in that state.</li>
<li><strong>Error counter decay (E1).</strong> With the partner absent TEC climbed in steps of 8 from 56 and saturated at 128, error-passive; the first read after the partner returned showed 126, two successful frames later, and 65 after the following 100-frame burst. Minus one per successful frame predicts 26, so that reading came before the burst had drained or the burst lost frames; the test shows entry into and recovery from error-passive but does not check the decrement rate.</li>
<li><strong>Existing test code.</strong> <code>hw_can.c</code> checks bit 2 of C1FIFOSTA for "TX aborted"; bit 2 is the FIFO-empty flag, TXABT is bit 6.</li>
</ul>
</section>

<section id="scope">
<h2>Oscilloscope</h2>
<p>Rigol DHO814, 10× probes on flying leads soldered to the transceiver's CANH, CANL and GND pins. Whole 1 Mpt records were read over SCPI and every single-bit-wide run in the data phase was measured; the differential was recomputed from the raw samples. Board-transmit shows the MAX33041E's driver; adapter-transmit shows what the board has to decode. Levels are read 75 % into the bit, the earliest sample point in use on either side (board 80 % nominal and at 2 Mbps, 75 % at 5 Mbps; adapter 80 % nominal, 75 % data phase).</p>
<div class="key"><span><i style="background:var(--accent)"></i>CH1 CANH</span><span><i style="background:var(--canl)"></i>CH2 CANL</span><span><i style="background:var(--diff)"></i>Math CANH − CANL</span></div>
<div class="tablewrap"><table><thead><tr><th>case</th><th>bits</th><th>dominant at 75 % sample point, min / mean</th><th>recessive</th><th>overshoot / undershoot</th><th>bit width spread</th><th>10–90 % edge</th></tr></thead><tbody>
<tr><td>500 kbps, board TX</td><td class="mono">130</td><td class="mono">1.84 / 1.90 V</td><td class="mono">0 V</td><td class="mono">+2.11 / −0.65 V</td><td class="mono">±13 ns</td><td class="mono">&lt;10 ns</td></tr>
<tr><td>500 kbps, adapter TX</td><td class="mono">79</td><td class="mono">1.91 / 2.15 V</td><td class="mono">0 V</td><td class="mono">+2.22 / −0.08 V</td><td class="mono">±5 ns</td><td class="mono">68 ns</td></tr>
<tr><td>FD 2 Mbps, board TX</td><td class="mono">575</td><td class="mono">1.84 / 1.89 V</td><td class="mono">0 V</td><td class="mono">+2.13 / −0.64 V</td><td class="mono">±12 ns</td><td class="mono">&lt;10 ns</td></tr>
<tr><td>FD 2 Mbps, adapter TX</td><td class="mono">467</td><td class="mono">2.08 / 2.15 V</td><td class="mono">0 V</td><td class="mono">+2.20 / −0.06 V</td><td class="mono">±3 ns</td><td class="mono">69 ns</td></tr>
<tr><td>FD 5 Mbps, board TX</td><td class="mono">503</td><td class="mono">1.81 / 1.87 V</td><td class="mono">0 V</td><td class="mono">+2.12 / −0.66 V</td><td class="mono">±11 ns</td><td class="mono">&lt;10 ns</td></tr>
<tr><td>FD 5 Mbps, adapter TX</td><td class="mono">419</td><td class="mono">2.01 / 2.08 V</td><td class="mono">0 V</td><td class="mono">+2.21 / −0.04 V</td><td class="mono">±3 ns</td><td class="mono">65 ns</td></tr>
</tbody></table></div>
<p>Single-ended, board transmitting: recessive CANH = CANL = 2.16 V; dominant CANH 2.98 V, CANL 1.08 V. Edges are faster than the 100 MHz scope resolves. Each dominant-to-recessive transition has a brief undershoot to about −0.65 V and a ~40 MHz ring for about 150 ns that appears equally on both lines and cancels in the difference; it cannot upset a receiver, but this is what a harness will radiate. The MAX33041E has a slew-rate-limited mode selected by a 39.2 kΩ resistor from its STBY pin to ground (rising-edge slew 15 V/µs instead of 120 V/µs, about 100 ns edges). The datasheet states no data-rate restriction for it, but 100 ns edges in a 200 ns bit would leave little margin at 5 Mbps, so expect it to cost the highest FD rates; if EMC on a real loom is a problem that is the first thing to try, and the board's STBY drive from the controller's GPIO0 would need to accommodate it. After every dominant bit both lines drop together to about 1.6 V and drift back to 2.16 V over several microseconds with zero differential: the receiver bias network recharging, normal, and confusing on a single-ended trace.</p>
<div class="figs">
{img("scope_classic_edge.png", "500 kbps rising edge at 100 ns/div", "500 kbps, board transmitting, 100 ns/div. One recessive-to-dominant edge; CANH rises to 3.0 V and CANL falls to 1.1 V in the same sample.")}
{img("scope_classic_eye.png", "500 kbps persistence eye at 500 ns/div", "500 kbps eye, 500 ns/div, infinite persistence. The slow ramps after the bit are the common-mode recovery, identical on both lines.")}
{img("scope_fd2m_frame.png", "FD frame at 50 µs/div", "One FD frame at 50 µs/div: sparse 500 kbps arbitration bits from the start-of-frame, then the dense 2 Mbps data phase.")}
{img("scope_fd2m_eye.png", "FD 2 Mbps eye at 250 ns/div", "FD 2 Mbps data phase, board transmitting, 250 ns/div. 500 ns bits, flat within 20 ns of each edge.")}
{img("scope_fd5m_eye.png", "FD 5 Mbps eye at 100 ns/div", "FD 5 Mbps, board transmitting, 100 ns/div. 200 ns bits; the eye at the 75 % sample point is fully open.")}
{img("scope_fd5m_host_eye.png", "FD 5 Mbps eye, adapter transmitting", "FD 5 Mbps, adapter transmitting, 100 ns/div: what the board receives. Slower 65 ns edges, still about 85 ns of settled level before a 75 % sample point.")}
{img("scope_fd5m_frame.png", "FD 5 Mbps frame at 50 µs/div", "A 5 Mbps FD frame at 50 µs/div, board transmitting: 154 µs from start-of-frame to the ACK bit, about 170 µs with the recessive end-of-frame, for 64 bytes.")}
{img("scope_loopdelay_fall.png", "Transceiver loop delay", "Loop delay, 50 ns/div: TXD (magenta) falls at 0, the bus follows 29 ns later, RXD (blue) 72 ns later. The ringing on the logic pins is the probe ground leads.")}
</div>
<h3>Transceiver loop delay</h3>
<p>Probes on the MAX33041E TXD and RXD pins, 223 edges each way at 500 kbps, spread under ±2 ns.</p>
<div class="tablewrap"><table><thead><tr><th>edge</th><th>TXD → bus</th><th>bus → RXD</th><th>TXD → RXD</th></tr></thead><tbody>
<tr><td>recessive → dominant</td><td class="mono">29 ns</td><td class="mono">43 ns</td><td class="mono">72.5 ns</td></tr>
<tr><td>dominant → recessive</td><td class="mono">16 ns</td><td class="mono">43 ns</td><td class="mono">60 ns</td></tr>
</tbody></table></div>
<p>Inside the datasheet's 140 ns maximum (70–90 ns typical). The dominant bit starts 29 ns late on the bus and ends 16 ns late, so the 13 ns asymmetry shortens it by 6.5 % of a 5 Mbps bit, normal for a CAN FD transceiver. The MCP2518FD's TDC in auto mode measures this itself; these are the numbers for a manual offset if auto is ever disabled.</p>
</section>

<section id="bugs">
<h2>Agent bugs found on the way</h2>
<p>Fixed before the final run, and kept here because a production driver can make the same mistakes and the symptoms are instructive.</p>
<ul>
<li><strong>FD data bit timing.</strong> TSEG2 one quantum too long at 2 and 4 Mbps (21 and 11 quanta per bit instead of 20 and 10). The board's own loopback passed because it agreed with itself; every exchange with the adapter at those rates failed with data-phase stuff errors while 1, 5 and 8 Mbps were perfect. Check <code>1 + (TSEG1+1) + (TSEG2+1)</code> for every entry.</li>
<li><strong>Message RAM overrun.</strong> With receive timestamps an FD RX object is 76 bytes, not 72; 4 TX + 24 RX objects came to 2112 bytes in 2048. The controller wrapped silently: 2.7 % of FD frames came back with the first four data bytes correct and the rest zero, no error flag anywhere. Budget the RAM from PLSIZE, FSIZE and RXTSEN.</li>
<li><strong>One-shot TX.</strong> As above; the agent now resets the FIFO when it finds a message with TXREQ clear.</li>
</ul>
</section>

<section id="limits">
<h2>Not testable on this bench</h2>
<ul>
<li>Signal integrity on a real harness: stub reflections, ground offset between ECUs and emissions from the fast edges need the vehicle wiring or an EMC bench.</li>
<li>Crystal ppm and drift over temperature. The tolerance test only resolves about ±0.3 % relative to the adapter's clock; a frequency counter is needed for ppm figures.</li>
<li>Fault protection of the MAX33041E (±40 V, shorts to battery or ground) and the NUP2105L clamping.</li>
<li>Sleep and standby currents, and the draw from a real harness.</li>
<li>Multi-node behaviour, high bus load from other nodes, wake-on-CAN with vehicle-specific frames, and ISO 15765 / OBD, which the firmware does not implement yet.</li>
<li>Bus-off recovery: the two-node rig never produced a data-field collision.</li>
</ul>
</section>

<section id="repeat">
<h2>Repeat it</h2>
<p>Everything is in the firmware repository: <code>src/can_bench.c</code> (agent), <code>can_bench/</code> (driver, server, suite, README with the procedure), <code>can_bench/adapter_firmware/</code> (adapter images and DFU tools), <code>can_bench/scope/</code> (SCPI helper, traffic generator, eye and loop-delay analysis) and <code>can_bench/results/</code> (JSON, Markdown, console transcript, all 21 scope captures with a statistics README). The Markdown report <code>CAN_BENCH_REPORT.md</code> carries the same content.</p>
</section>
<hr><p class="small">l0destar firmware · CAN bench test · 2 September 2026</p>
</div>
'''
out = f"{R}/CAN_BENCH_REPORT.html"
open(out, "w").write(html)
print(out, round(os.path.getsize(out)/1e6, 2), "MB")
