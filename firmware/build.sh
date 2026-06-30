#!/usr/bin/env bash
# Build the tracker firmware (NCS v3.3.0).  Two board profiles, auto-selected by
# which debug probe is attached — override with PROFILE=dk|makerdiary:
#
#   dk          Nordic nRF9151 DK (default).  Console on the J-Link VCOM pins
#               (P0.27/P0.26) via the committed boards/nrf9151dk overlay.
#   makerdiary  Makerdiary nRF9151 Connect Kit, detected via its CMSIS-DAP probe.
#               Builds the vendored nrf9151_connectkit board target (boards/
#               makerdiary/, found via BOARD_ROOT) so the console pins AND the
#               GNSS antenna config match the hardware — the DK target wrongly
#               sends AT%XCOEX0 for the DK's RF switch, which breaks GPS on this
#               board.  makerdiary.conf re-parks the two app GPIOs that collide
#               with the board's console pins (P0.11/P0.12).
set -euo pipefail

if [ "${1:-}" = "pristine" ] ; then
    rm -rf build
fi

NCS_VERSION="${NCS_VERSION:-v3.3.0}"
NCS_ROOT="${NCS_ROOT:-/opt/nordic/ncs/$NCS_VERSION}"
BOARD_OVERRIDE="${BOARD:-}"   # explicit BOARD=... wins over the per-profile default
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

# --- board profile ---------------------------------------------------------
# Build for the Makerdiary nRF9151 Connect Kit when its CMSIS-DAP probe is
# attached, otherwise the Nordic DK.  `pyocd list` names the probe
# ("Makerdiary IFMCU CMSIS-DAP"); the DK's J-Link doesn't appear there, so a
# name match is an unambiguous Makerdiary signal.  Set PROFILE to force either.
PROFILE="${PROFILE:-}"
if [[ -z "$PROFILE" ]]; then
	if command -v pyocd >/dev/null 2>&1 && pyocd list 2>/dev/null | grep -qiE 'makerdiary'; then
		PROFILE=makerdiary
	else
		PROFILE=dk
	fi
fi
# Board target per profile (an explicit BOARD=... still wins).
if [[ -n "$BOARD_OVERRIDE" ]]; then
	BOARD="$BOARD_OVERRIDE"
elif [[ "$PROFILE" == makerdiary ]]; then
	BOARD="nrf9151_connectkit/nrf9151/ns"
else
	BOARD="nrf9151dk/nrf9151/ns"
fi
echo "Board profile: $PROFILE  (target: $BOARD)"

# Switching profiles/targets leaves the other board's devicetree/Kconfig cached
# in the build dir; force a clean rebuild when the (profile, board) signature
# differs from last time.
MARKER="$BUILD_DIR/.board_profile"
SIG="$PROFILE|$BOARD"
if [[ -f "$MARKER" ]]; then
	LAST="$(cat "$MARKER" 2>/dev/null || true)"
	if [[ -n "$LAST" && "$LAST" != "$SIG" ]]; then
		echo "Build target changed ($LAST -> $SIG) — forcing pristine rebuild."
		PRISTINE=always
	fi
fi

CMAKE_ARGS=()
CONF_OVERLAYS=()
DTC_OVERLAYS=()

# Makerdiary profile: the board target supplies the console pins and GNSS
# antenna default, so we only need BOARD_ROOT (to find the vendored board def)
# and makerdiary.conf for the app GPIO re-park.  Layered first so the personal
# local.* files below can still override on the bench.
if [[ "$PROFILE" == makerdiary ]]; then
	CMAKE_ARGS+=("-DBOARD_ROOT=$APP_DIR")
	if [[ ! -d "$APP_DIR/boards/makerdiary/nrf9151_connectkit" ]]; then
		echo "WARNING: makerdiary profile selected but boards/makerdiary/nrf9151_connectkit" >&2
		echo "         is missing — the $BOARD target won't resolve. Re-vendor the board def." >&2
	fi
	if [[ -f "$APP_DIR/makerdiary.conf" ]]; then
		CONF_OVERLAYS+=("makerdiary.conf")
	fi
fi

if [[ -f "$APP_DIR/local.conf" ]]; then
	CONF_OVERLAYS+=("local.conf")
fi
# PROV=1 layers prov.conf on top (AT-host bridge for nRF Cloud onboarding).
if [[ "${PROV:-}" == "1" && -f "$APP_DIR/prov.conf" ]]; then
	CONF_OVERLAYS+=("prov.conf")
fi
# Optional bench-only devicetree overlay (gitignored), applied last so it wins.
if [[ -f "$APP_DIR/local.overlay" ]]; then
	DTC_OVERLAYS+=("local.overlay")
fi

if [[ ${#CONF_OVERLAYS[@]} -gt 0 ]]; then
	CMAKE_ARGS+=("-DOVERLAY_CONFIG=$(IFS=';'; echo "${CONF_OVERLAYS[*]}")")
fi
if [[ ${#DTC_OVERLAYS[@]} -gt 0 ]]; then
	CMAKE_ARGS+=("-DEXTRA_DTC_OVERLAY_FILE=$(IFS=';'; echo "${DTC_OVERLAYS[*]}")")
fi

EXTRA_ARGS=()
if [[ ${#CMAKE_ARGS[@]} -gt 0 ]]; then
	EXTRA_ARGS+=("--" "${CMAKE_ARGS[@]}")
fi

# `west` must run from inside the NCS workspace so it can find the manifest;
# the app itself can live anywhere — we pass it as an absolute path.
nrfutil sdk-manager toolchain launch \
	--ncs-version "$NCS_VERSION" \
	--chdir "$NCS_ROOT" \
	-- west build -p "$PRISTINE" -b "$BOARD" -d "$BUILD_DIR" "$APP_DIR" \
	"${EXTRA_ARGS[@]}"

# Record what this build dir now holds (drives the switch check above).
echo "$SIG" > "$MARKER"

echo
echo "Built: $BUILD_DIR/merged.hex  (profile: $PROFILE, target: $BOARD)"
