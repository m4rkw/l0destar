#!/usr/bin/env bash
# Build the Makerdiary IF MCU firmware for the nRF52820.
#
# Clones the official Makerdiary nRF9151 ConnectKit repo (if not already
# present) and builds it unmodified.  The SYSTEM OFF on USB disconnect fix -
# without which the nRF52820 holds HFCLK at ~2 mA after the cable is removed -
# is upstream as makerdiary/nrf9151-connectkit#19 plus the #20 follow-up that
# applies the SEVONPEND clear to the charger-poll and shell power-off paths
# too.  Both are merged on main but neither is in a published release, hence
# building from source.  An existing clone from before #20 needs a `git pull`.
#
# Output: build_ifmcu/ifmcu_firmware/zephyr/zephyr.uf2
#
# Flash by double-pressing the ConnectKit reset button (enters UF2
# bootloader), then copying the .uf2 to the mass-storage device.
set -euo pipefail

NCS_VERSION="${NCS_VERSION:-v3.4.0}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$(dirname "$SCRIPT_DIR")"

# Handle iCloud Drive space-in-path.
if [[ "$FIRMWARE_DIR" == *" "* ]]; then
	ICLOUD="$HOME/Library/Mobile Documents/com~apple~CloudDocs"
	ALT="${FIRMWARE_DIR/$ICLOUD/$HOME}"
	if [[ "$ALT" != "$FIRMWARE_DIR" && -d "$ALT" ]]; then
		FIRMWARE_DIR="$ALT"
		SCRIPT_DIR="$FIRMWARE_DIR/ifmcu"
	else
		echo "Error: path contains a space and no matching space-free firmlink found." >&2
		exit 1
	fi
fi

source "$FIRMWARE_DIR/ncs_env.sh"

REPO_DIR="$FIRMWARE_DIR/ifmcu/.makerdiary-repo"
BUILD_DIR="$FIRMWARE_DIR/build_ifmcu"
BOARD="nrf9151_connectkit/nrf52820"

# --- clone the Makerdiary repo if missing ---
if [[ ! -d "$REPO_DIR" ]]; then
	echo "Cloning makerdiary/nrf9151-connectkit..."
	git clone --depth 1 https://github.com/makerdiary/nrf9151-connectkit.git "$REPO_DIR"
else
	echo "Using existing repo at $REPO_DIR"
fi

APP_DIR="$REPO_DIR/applications/ifmcu_firmware"

# The whole reason we build this ourselves: refuse a checkout that predates
# the power fix, otherwise we'd silently produce ~2 mA firmware.  #20 is the
# one that introduced ifmcu_system_off(); a clone with only #19 (which named
# its helper enter_system_off) still has the charger-poll race.
if ! grep -q "ifmcu_system_off" "$APP_DIR/src/main.c"; then
	echo "Error: $REPO_DIR predates makerdiary/nrf9151-connectkit#20 (the IF MCU power fix)." >&2
	echo "Update it, then re-run.  If the clone has local edits (e.g. the old" >&2
	echo "l0destar patch), discard them first:" >&2
	echo "  git -C $REPO_DIR checkout -- . && git -C $REPO_DIR pull" >&2
	exit 1
fi

echo "Building IF MCU firmware for $BOARD"

# The board definitions come from the same Makerdiary checkout.
in_ncs west build -p auto -b "$BOARD" -d "$BUILD_DIR" "$APP_DIR" -- "-DBOARD_ROOT=$REPO_DIR"

UF2="$BUILD_DIR/ifmcu_firmware/zephyr/zephyr.uf2"
echo
echo "Built: $UF2"
echo
echo "To flash: double-press reset on the Connect Kit, then copy it onto the"
echo "UF2BOOT drive that appears."
