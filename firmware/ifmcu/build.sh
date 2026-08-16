#!/usr/bin/env bash
# Build the patched Makerdiary IF MCU firmware for the nRF52820.
#
# Clones the official Makerdiary nRF9151 ConnectKit repo (if not already
# present), applies our power-fix patch, and builds.  The patch adds
# automatic SYSTEM OFF on USB disconnect so the nRF52820 doesn't hold
# HFCLK at ~2 mA after the cable is removed.
#
# Output: build_ifmcu/zephyr/zephyr.uf2
#
# Flash by double-pressing the ConnectKit reset button (enters UF2
# bootloader), then copying the .uf2 to the mass-storage device.
set -euo pipefail

NCS_VERSION="${NCS_VERSION:-v3.4.0}"
NCS_ROOT="${NCS_ROOT:-/opt/nordic/ncs/$NCS_VERSION}"
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

REPO_DIR="$FIRMWARE_DIR/ifmcu/.makerdiary-repo"
BUILD_DIR="$FIRMWARE_DIR/build_ifmcu"
BOARD="nrf9151_connectkit/nrf52820"
PATCH="$SCRIPT_DIR/power-fix.patch"

# --- clone the Makerdiary repo if missing ---
if [[ ! -d "$REPO_DIR" ]]; then
	echo "Cloning makerdiary/nrf9151-connectkit..."
	git clone --depth 1 https://github.com/makerdiary/nrf9151-connectkit.git "$REPO_DIR"
	echo "Apply the power-fix patch after cloning:"
	echo "  cd $REPO_DIR && git apply $PATCH"
	echo "  (or edit applications/ifmcu_firmware/src/main.c manually)"
else
	echo "Using existing repo at $REPO_DIR"
fi

APP_DIR="$REPO_DIR/applications/ifmcu_firmware"

echo "Building IF MCU firmware for $BOARD"

nrfutil sdk-manager toolchain launch \
	--ncs-version "$NCS_VERSION" \
	--chdir "$NCS_ROOT" \
	-- west build -p auto -b "$BOARD" -d "$BUILD_DIR" "$APP_DIR" \
	-- "-DBOARD_ROOT=$FIRMWARE_DIR"

UF2="$BUILD_DIR/ifmcu_firmware/zephyr/zephyr.uf2"
echo
echo "Built: $UF2"
echo
echo "To flash: double-press reset on the ConnectKit, then:"
echo "  cp $UF2 /Volumes/UF2BOOT/"
