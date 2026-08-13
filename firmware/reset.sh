#!/bin/bash
# Reset the Makerdiary Connect Kit's nRF9151 via its CMSIS-DAP probe.
# sysresetreq resets only the nRF9151 core; the default reset kind doesn't
# reboot it, and `-m hw` pulses the board-level reset line, which also resets
# the DAPLink interface MCU — USB re-enumerates and any attached serial
# session (screen) loses the port.
pyocd reset -t nrf91 -m sysresetreq
