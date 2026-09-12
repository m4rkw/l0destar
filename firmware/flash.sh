#!/bin/bash
# Program the nRF9151 via the Connect Kit's CMSIS-DAP probe.
#
# --no-reset plus an explicit hardware reset: pyocd's default post-load reset
# is a soft (SYSRESETREQ) reset, which leaves the nRF9151 in debug interface
# mode. In that mode the SoC never reaches its low-power floor after USB is
# unplugged (sleep current is milliamps instead of ~134 uA) until a pin reset
# or power cycle. The board-level reset line is shared, so this also reboots
# the interface MCU and USB re-enumerates; any attached serial session will
# need reopening.
set -e
pyocd load -t nrf91 --no-reset build/merged.hex
pyocd reset -t nrf91 -m hw
