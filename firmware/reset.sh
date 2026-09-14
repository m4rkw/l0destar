#!/bin/bash
# Reset the Makerdiary Connect Kit's nRF9151 via its CMSIS-DAP probe.
#
# `-m hw` pulses the board-level reset line, as flash.sh does.  A sysresetreq
# (soft) reset would keep USB and an attached console up, but it leaves the SoC
# in debug interface mode, drawing milliamps while asleep until the next pin
# reset or power cycle.  The pin reset also reboots the DAPLink interface MCU,
# so USB re-enumerates and any attached serial session (screen) loses the port
# and has to be reopened.  board_test.sh, which attaches first and then resets,
# keeps a soft reset of its own.
#
# auto_unlock=false is not optional here. pyocd defaults it to true, and every
# pyocd connect to an nRF91 runs check_flash_security before anything else: it
# reads CTRL-AP APPROTECTSTATUS, and if the part reports APPROTECT (or
# SECUREAPPROTECT) engaged it issues CTRL-AP ERASEALL to win back debug
# access. That mass erase wipes application flash and UICR in about a second
# and leaves a blank chip -- the "quick erase that bricks the board".
#
# Nothing about this script changed to trigger that; the board state did.
# While flash.sh ended with pyocd's default soft reset the SoC stayed in debug
# interface mode, where the debug port stays enabled and APPROTECT is never
# re-armed, so this connect always found an unlocked part. flash.sh now ends
# with a pin reset, which drops the SoC back to normal mode, and APPROTECT is
# re-armed in hardware on every reset unless the firmware clears it (the app
# builds CONFIG_NRF_APPROTECT_USE_UICR=y, so it only clears it when
# UICR.APPROTECT reads Unprotected / 0x50FA50FA). A wiped UICR therefore
# self-perpetuates: erased UICR -> firmware boots locked -> next reset erases.
#
# A reset must never erase. With auto_unlock off pyocd warns and fails instead.
set -e

if ! pyocd reset -t nrf91 -m hw -O auto_unlock=false ; then
    cat >&2 <<'EOF'

Reset failed. If pyocd logged "APPROTECT enabled", the nRF9151 booted with
debug access locked. That means UICR.APPROTECT is not Unprotected
(0x50FA50FA), which is the state a previous mass erase leaves behind.

Recover with ./flash.sh -- its load step is allowed to unlock by mass erase,
restores UICR, and reprograms the firmware in the same run.
EOF
    exit 1
fi
