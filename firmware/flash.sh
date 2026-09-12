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
#
# The pin reset also drops the SoC out of debug interface mode, so APPROTECT
# is re-armed in hardware and the firmware has to clear it at every boot. It
# does (CONFIG_NRF_APPROTECT_USE_UICR=y) but only while UICR.APPROTECT reads
# Unprotected (0x50FA50FA). That makes the two pyocd calls below want opposite
# settings of auto_unlock, which decides whether pyocd mass-erases a part that
# reports APPROTECT engaged:
#
#   load   keeps the default (on). If the part does come up locked this is the
#          one place an erase is recoverable -- pyocd wipes flash and UICR,
#          writes UICR.APPROTECT/SECUREAPPROTECT back to Unprotected, and the
#          firmware is reprogrammed seconds later. Modem firmware lives outside
#          the erased region and survives.
#   reset  turns it off. Nothing may erase the image that was just programmed.
#
# reset.sh turns it off for the same reason: a reset must never erase.
set -e
pyocd load -t nrf91 --no-reset build/merged.hex
pyocd reset -t nrf91 -m hw -O auto_unlock=false
