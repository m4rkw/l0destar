# K-wire discovery — quickstart

**Before building, installing or relying on any of this, read the [project disclaimer](../DISCLAIMER.md).**

How to find out what a vehicle speaks on the K wire (OBD pin 7).  Run this
once per vehicle; it prints a summary ending in the `local.conf` block that
configures normal operation.  Full detail in [KWIRE.md](KWIRE.md).

## Running it

1. In `local.conf`, confirm the board has the interface fitted and turn
   discovery on:

   ```
   CONFIG_APP_BOARD_HAS_KLINE=y
   CONFIG_APP_KLINE_DISCOVER=y
   CONFIG_APP_KLINE_IDENT=y     # ask each responder what it supports
   CONFIG_APP_KLINE_DTC=y       # and read its fault codes
   ```

2. Build, flash, and watch the console with the **ignition on**:

   ```
   ./build.sh && ./flash.sh
   tail -f ~/.log/serial.log
   ```

3. Wait.  On an unknown vehicle it sweeps every ECU address and takes up to
   15 minutes; with `CONFIG_APP_KLINE_INIT_ADDRS` already set to known
   addresses it takes seconds.  The board parks when it finishes rather than
   starting the tracker, so the log survives.

4. Copy the suggested block from the summary into `local.conf` and rebuild.
   That turns discovery off and points the runtime at the right address and
   data rate.

If nothing answers, the console says where it stopped.  The usual causes are
the harness rather than the protocol — see the troubleshooting section in
[KWIRE.md](KWIRE.md), and note that a clean transceiver loopback proves
nothing about the wiring beyond the transceiver.

## Example run

A 2006 Toyota Harrier 2.4 (ACU30, JDM import), with the engine off and
`CONFIG_APP_KLINE_INIT_ADDRS="13"` already known from an earlier sweep:

```
*** Booting MCUboot v2.3.0-dev-fce4dac2e629 ***
*** Using nRF Connect SDK v3.3.0-ba167d9f3db4 ***
*** Using Zephyr OS v4.3.99-fd9204a02d52 ***
I: Starting bootloader
I: Primary image: magic=good, swap_type=0x2, copy_done=0x1, image_ok=0x1
I: Secondary image: magic=unset, swap_type=0x1, copy_done=0x3, image_ok=0x3
I: Boot source: none
I: Image index: 0, Swap type: none
I: Bootloader chainload address offset: 0x10000
I: Image version: v0.4.0
All pins have been configured as non-secure
Booting TF-M v2.2.2**
[Sec Thread] Secure image initializing!
*** Booting My Application v0.4.0-ab50a53880b6 ***
*** Using nRF Connect SDK v3.3.0-ba167d9f3db4 ***
*** Using Zephyr OS v4.3.99-fd9204a02d52 ***
[00:00:00.264,587] <inf> main: === l0destar firmware boot (v0.4.0, board v3.1+kline) ===
[00:00:00.273,040] <inf> crypto: ready
[00:00:00.277,282] <inf> settings: apn=sensor.net user=
[00:00:00.282,867] <inf> settings: int=0 ma=1
[00:00:00.287,628] <inf> settings: imei=(unset)
[00:00:00.307,800] <inf> hw_domain: AUX domain on (P0.13)
[00:00:00.323,638] <inf> hw_selftest: === power rail self-test ===
[00:00:00.330,200] <inf> hw_selftest: PP3V3_GPS: OK
[00:00:00.335,479] <inf> hw_selftest: self-test: all rails OK
[00:00:00.351,837] <inf> hw_common: INA228 pins: SCL(P0.21)=1 SDA(P0.20)=1 OK
[00:00:00.384,216] <inf> hw_power: INA228 ready — VBUS=12.339 V
[00:00:00.400,909] <inf> hw_common: ACC pins: SCL(P0.21)=1 SDA(P0.20)=1 OK
[00:00:00.493,896] <inf> hw_accel: ACC ready (WHO_AM_I=0x6B, XL 416 Hz +/-8g, G 104 Hz +/-250dps)

*** K-LINE INIT ***
[00:00:00.520,294] <inf> hw_domain: K domain on (P0.10)
[00:00:01.126,098] <inf> hw_kline: K-line configured (TX=P0.1 RX=P0.3)
TX=P0.1 RX=P0.3
bit timing: loop runs 99.06 us/bit uncorrected, waits trimmed to 93/47 us
bus idle: RX=1, low for 0/300 ms
slow init: address 0x33 on K only, reply at 9600 baud
  no sync byte within 400 ms
slow init: address 0x13 on K only, reply at 9600 baud
  0x13: sync 0x55 after 88 ms at 9600 baud
  key bytes: KB1=0xE9 KB2=0x8F — ISO 14230-4 KWP2000
  ack: 0xEC (expect 0xEC)
identify 0x13:
  StartDiagnosticSession mode 81
  > 82 13 F1 10 81 17
  < (no response)
  StartDiagnosticSession mode 85
  > 82 13 F1 10 85 1B
  < (no response)
  StartDiagnosticSession mode 86
  > 82 13 F1 10 86 1C
  < (no response)
  StartDiagnosticSession mode 89
  > 82 13 F1 10 89 1F
  < (no response)
  StartDiagnosticSession mode C0
  > 82 13 F1 10 C0 56
  < (no response)
  (no diagnostic session — trying data services anyway)
  ReadEcuIdentification: data table
  > 82 13 F1 1A 80 20
  < (no response)
  ReadEcuIdentification: VIN
  > 82 13 F1 1A 90 30
  < (no response)
  ReadEcuIdentification: HW number
  > 82 13 F1 1A 91 31
  < (no response)
  ReadEcuIdentification: supplier HW
  > 82 13 F1 1A 92 32
  < (no response)
  ReadEcuIdentification: supplier SW
  > 82 13 F1 1A 94 34
  < (no response)
  ReadEcuIdentification: system name
  > 82 13 F1 1A 97 37
  < (no response)
  ReadEcuIdentification: serial number
  > 82 13 F1 1A 9F 3F
  < (no response)
  OBD mode 01: supported PIDs 01-20
  > 82 13 F1 01 00 87
  < 86 F1 13 41 00 BE 1F B8 00 60
    PID bitmap BE 1F B8 00 — an engine/powertrain ECU
  OBD mode 01: MIL + stored DTC count
  > 82 13 F1 01 01 88
  < 83 F1 13 41 01 00 C9
    MIL off, 0 stored DTCs
  OBD mode 01: engine RPM
  > 82 13 F1 01 0C 93
  < 84 F1 13 41 0C 00 00 D5
    RPM 0 — engine ECU
  OBD mode 01: coolant temperature
  > 82 13 F1 01 05 8C
  < 83 F1 13 41 05 4E 1B
    coolant 38 C — engine ECU
  OBD mode 09: VIN
  > 82 13 F1 09 02 91
  < (no response)
  OBD mode 03: stored DTCs
  > 81 13 F1 03 88
  < (no response)
  OBD mode 07: pending DTCs
  > 81 13 F1 07 8C
  < 87 F1 13 47 00 00 00 00 00 00 D2
    0 codes reported
  OBD mode 0A: permanent DTCs
  > 81 13 F1 0A 8F
  < (no response)
  StopCommunication
  > 81 13 F1 82 07
  < 81 F1 13 C2 47
identify 0x13: 4 of 12 requests answered
addresses: 1 of 1 replied, 1 handshake completed
PASS: K-line communication established via 5-baud init, ECU 0x13 (ISO 14230-4 KWP2000)
*** K-LINE INIT DONE ***

[00:00:24.179,687] <inf> hw_kline: K-line power off
[00:00:24.185,028] <inf> hw_domain: K domain off (P0.10)

=== K-WIRE DISCOVERY SUMMARY ===
  protocol       ISO 14230-4 KWP2000, 5-baud init
  data rate      9600 baud
  L wire         not needed
  addresses      13
  engine ECU     0x13
  mode 01 PIDs   01 03 04 05 06 07 0C 0D 0E 0F 10 11 13 14 15
  engine RPM     yes (PID 0C)
  vehicle speed  yes (PID 0D)
  more PIDs      no, nothing above 0x15
  fault codes    mode 03 silent, mode 07 yes, mode 0A silent
  identification none (no 0x1A, no mode 09 VIN)
  diag session   not needed / not accepted

  Suggested local.conf:
    CONFIG_APP_KLINE_BAUD=9600
    CONFIG_APP_KLINE_ECU_ADDR=0x13
    CONFIG_APP_KLINE_INIT_ADDRS="13"
    CONFIG_APP_KLINE_INIT_FAST=n
    CONFIG_APP_KLINE_INIT_SWEEP=n
    CONFIG_APP_KLINE_DISCOVER=n
=== END ===
```

The last block is the point of the exercise: paste it into `local.conf`, add
`CONFIG_APP_KLINE_TELEMETRY=y` and `CONFIG_APP_KLINE_DTC_REPORT=y` if you want
OBD data and fault codes in the telemetry, and rebuild.
