#!/usr/bin/env bash
# Build the Makerdiary IF MCU firmware for the nRF52820.
#
# Clones the official Makerdiary nRF9151 ConnectKit repo (if not already
# present), applies the patches in ifmcu/patches/ and builds it.  The SYSTEM
# OFF on USB disconnect fix - without which the nRF52820 holds HFCLK at ~2 mA
# after the cable is removed - is upstream as of
# makerdiary/nrf9151-connectkit#19 (an existing clone from before that merge
# needs a `git pull`).  ifmcu/patches/ carries the follow-up that is not
# merged yet: the SEVONPEND clear from #19 applied to the charger-poll and
# shell power-off paths too, which otherwise can still leave the chip
# spinning at ~2 mA.  Drop the patch once it lands upstream.
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

# --- clone the Makerdiary repo if missing ---
if [[ ! -d "$REPO_DIR" ]]; then
	echo "Cloning makerdiary/nrf9151-connectkit..."
	git clone --depth 1 https://github.com/makerdiary/nrf9151-connectkit.git "$REPO_DIR"
else
	echo "Using existing repo at $REPO_DIR"
fi

APP_DIR="$REPO_DIR/applications/ifmcu_firmware"

# The whole reason we build this ourselves: warn loudly if the checkout
# predates the power fix, otherwise we'd silently produce ~2 mA firmware.
# Upstream names the helper enter_system_off; the patch below renames it to
# ifmcu_system_off, so accept either.
if ! grep -qE "enter_system_off|ifmcu_system_off" "$APP_DIR/src/main.c"; then
	echo "Error: $REPO_DIR predates the USB-disconnect power fix." >&2
	echo "Update it, then re-run:" >&2
	echo "  git -C $REPO_DIR pull" >&2
	exit 1
fi

# --- apply local patches ---
# Each patch is applied once: one that already reverse-applies is skipped;
# one that neither applies nor reverse-applies (upstream moved, or the clone
# has been edited by hand) aborts rather than building something
# half-patched.
for p in "$SCRIPT_DIR"/patches/*.patch; do
	[[ -e "$p" ]] || continue
	name="$(basename "$p")"
	if git -C "$REPO_DIR" apply --reverse --check "$p" 2>/dev/null; then
		echo "Patch already applied: $name"
	elif git -C "$REPO_DIR" apply --check "$p" 2>/dev/null; then
		echo "Applying patch: $name"
		git -C "$REPO_DIR" apply "$p"
	else
		echo "Error: $name does not apply to $REPO_DIR." >&2
		echo "Either upstream changed (rebase the patch, or drop it if merged)" >&2
		echo "or the clone has local edits: see 'git -C $REPO_DIR status'." >&2
		exit 1
	fi
done

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
