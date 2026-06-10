#!/usr/bin/env bash
# Build the blinky firmware for the nRF9151 DK using NCS v3.3.0.
set -euo pipefail

if [ "${1:-}" = "pristine" ] ; then
    rm -rf build
fi

NCS_VERSION="${NCS_VERSION:-v3.3.0}"
NCS_ROOT="${NCS_ROOT:-/opt/nordic/ncs/$NCS_VERSION}"
BOARD="${BOARD:-nrf9151dk/nrf9151/ns}"
APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Zephyr's CMake/kconfig pipeline mishandles spaces in source paths. When the
# repo lives under iCloud Drive ("Mobile Documents"), reroute to the space-free
# firmlink at ~/code/<repo> if it points at the same inode.
if [[ "$APP_DIR" == *" "* ]]; then
	ICLOUD="$HOME/Library/Mobile Documents/com~apple~CloudDocs"
	ALT="${APP_DIR/$ICLOUD/$HOME}"
	if [[ "$ALT" != "$APP_DIR" && -d "$ALT" ]] \
		&& [[ "$(stat -f '%d:%i' "$ALT")" == "$(stat -f '%d:%i' "$APP_DIR")" ]]; then
		echo "Rerouting through space-free path: $ALT"
		APP_DIR="$ALT"
	else
		echo "Error: APP_DIR contains a space and no matching space-free path was found." >&2
		echo "  APP_DIR=$APP_DIR" >&2
		exit 1
	fi
fi

BUILD_DIR="${BUILD_DIR:-$APP_DIR/build}"

# `-p auto` re-runs cmake when build settings change; pass `-p always` to force a clean build.
PRISTINE="${PRISTINE:-auto}"

EXTRA_ARGS=()
if [[ -f "$APP_DIR/local.conf" ]]; then
	EXTRA_ARGS+=("--" "-DOVERLAY_CONFIG=local.conf")
fi

# `west` must run from inside the NCS workspace so it can find the manifest;
# the app itself can live anywhere — we pass it as an absolute path.
nrfutil sdk-manager toolchain launch \
	--ncs-version "$NCS_VERSION" \
	--chdir "$NCS_ROOT" \
	-- west build -p "$PRISTINE" -b "$BOARD" -d "$BUILD_DIR" "$APP_DIR" \
	"${EXTRA_ARGS[@]}"

echo
echo "Built: $BUILD_DIR/merged.hex"
