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
# Flash by holding the Connect Kit's DFU/RST button while plugging in USB
# (enters the UF2 bootloader; switch the 12V off first if it is on the carrier
# board), then copying the .uf2 to the mass-storage device.
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

# The whole reason we build this ourselves: refuse a checkout without the power
# fix, otherwise we'd silently produce ~2 mA firmware.  The fix is
# makerdiary/nrf9151-connectkit#20, so its merge commit has to be in the clone's
# history, and the clone must be unmodified: the l0destar patch an older copy of
# this script applied named its helper ifmcu_system_off too, so a patched
# pre-#20 clone used to pass a check for that name.
PR20_MERGE=4698a5f54481551aad5ccb7103f49ab6e0a7a584
if [[ -n "$(git -C "$REPO_DIR" status --porcelain --untracked-files=no)" ]]; then
	echo "Error: $REPO_DIR has local changes, so it is not Makerdiary's code." >&2
	echo "Discard them and update it, then re-run:" >&2
	echo "  git -C $REPO_DIR checkout -- . && git -C $REPO_DIR pull" >&2
	exit 1
fi
if [[ "$(git -C "$REPO_DIR" rev-parse --is-shallow-repository)" == true ]]; then
	git -C "$REPO_DIR" fetch --quiet --unshallow || true
fi
if ! git -C "$REPO_DIR" merge-base --is-ancestor "$PR20_MERGE" HEAD 2>/dev/null; then
	echo "Error: $REPO_DIR predates makerdiary/nrf9151-connectkit#20 (the IF MCU power fix)." >&2
	echo "Update it, then re-run:  git -C $REPO_DIR pull" >&2
	exit 1
fi

echo "Building IF MCU firmware for $BOARD"

# The board definitions come from the same Makerdiary checkout.
in_ncs west build -p auto -b "$BOARD" -d "$BUILD_DIR" "$APP_DIR" -- "-DBOARD_ROOT=$REPO_DIR"

UF2="$BUILD_DIR/ifmcu_firmware/zephyr/zephyr.uf2"
echo
echo "Built: $UF2"
echo
echo "To flash: hold DFU/RST on the Connect Kit while plugging in USB (12V off"
echo "if it is on the carrier board), then copy it onto the UF2BOOT drive."
