#!/usr/bin/env bash
# Flash the built blinky firmware to the connected nRF9151 DK.
# If multiple boards are attached, set SERIAL=<jlink-sn> to disambiguate.
set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$APP_DIR/build}"
HEX="$BUILD_DIR/merged.hex"

if [[ ! -f "$HEX" ]]; then
	echo "Firmware not found at $HEX — run ./build.sh first." >&2
	exit 1
fi

SERIAL_ARG=()
if [[ -n "${SERIAL:-}" ]]; then
	SERIAL_ARG=(--serial-number "$SERIAL")
else
	# Auto-pick the single connected J-Link if there's exactly one.
	# `nrfutil device list` prints one block per device, each starting with the
	# bare serial number on its own line.
	mapfile -t SNS < <(nrfutil device list 2>/dev/null | awk '/^[0-9]+$/ {print}')
	if [[ ${#SNS[@]} -eq 0 ]]; then
		echo "No Nordic devices found. Plug in the DK or set SERIAL=<sn>." >&2
		exit 1
	elif [[ ${#SNS[@]} -gt 1 ]]; then
		echo "Multiple devices found: ${SNS[*]}. Set SERIAL=<sn> to choose." >&2
		exit 1
	fi
	SERIAL_ARG=(--serial-number "${SNS[0]}")
fi

nrfutil device reset --reset-kind RESET_SYSTEM "${SERIAL_ARG[@]}"
