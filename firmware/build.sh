#!/usr/bin/env bash
# Build the tracker firmware (NCS v3.3.0).  Two board profiles, auto-selected by
# which debug probe is attached — override with PROFILE=dk|makerdiary:
#
#   dk          Nordic nRF9151 DK (default).  Console on the J-Link VCOM pins
#               (P0.27/P0.26) via the committed boards/nrf9151dk overlay.
#   makerdiary  Makerdiary nRF9151 Connect Kit, detected via its CMSIS-DAP probe.
#               Builds the vendored nrf9151_connectkit board target (boards/
#               makerdiary/, found via BOARD_ROOT) so the console pins match the
#               hardware.  That board target does NOT default CONFIG_MODEM_ANTENNA
#               on (only the DK target does), so makerdiary.conf re-enables it and
#               sends AT%XCOEX0=1,1,1565,1586 — the Connect Kit's GNSS LNA LDO is
#               gated by COEX0, so without that command the LNA stays off and GPS
#               sees no satellites.  makerdiary.conf also re-parks the two app
#               GPIOs that collide with the board's console pins (P0.11/P0.12).
#
# The first build also fetches the Connect Kit board definition and creates the
# firmware signing key, and every build keeps src/ca_cert.h in step with
# certs/ca.crt, your server's CA.  ncs_env.sh finds the SDK and its toolchain.
set -euo pipefail

if [ "${1:-}" = "pristine" ] ; then
    rm -rf build
fi

NCS_VERSION="${NCS_VERSION:-v3.3.0}"
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

source "$APP_DIR/ncs_env.sh"

# BUILD_SUBDIR names a build directory relative to APP_DIR — use it rather
# than an absolute BUILD_DIR so the space-free reroute above still applies
# (push_fw.sh gives every device its own).
BUILD_DIR="${BUILD_DIR:-$APP_DIR/${BUILD_SUBDIR:-build}}"

# Personal bench overlay, named relative to APP_DIR.  push_fw.sh points
# LOCAL_CONF at a per-device fragment generated from remote.conf instead, so an
# image published to a deployed unit never picks up bench-only settings.
# LOCAL_CONF= (empty) layers none at all.
LOCAL_CONF="${LOCAL_CONF-local.conf}"
if [[ -n "$LOCAL_CONF" && ! -f "$APP_DIR/$LOCAL_CONF" ]]; then
	echo "Error: LOCAL_CONF=$LOCAL_CONF not found under $APP_DIR." >&2
	exit 1
fi

# `-p auto` re-runs cmake when build settings change; pass `-p always` to force a clean build.
PRISTINE="${PRISTINE:-auto}"

# --- version ---------------------------------------------------------------
# VERSION holds MAJOR.MINOR and nothing else, so no patch number is ever
# committed.  Zephyr insists on reading $APP_DIR/VERSION in its own format
# (cmake/modules/version.cmake hard-errors unless PATCHLEVEL is present, and an
# absent line silently reuses the previous regex capture), and -DVERSION_FILE
# does not survive sysbuild's per-image cmake — tested, it built 0.4.0 while
# pointed at a file saying 42.  So the long form is generated in place for the
# duration of the build and the short form is put back on the way out.
#
# FW_PATCH comes from push_fw.sh, which derives it from what is published on
# the server.  A bench build gets 0.
#
# Both forms are accepted on the way in, so a build killed hard enough to skip
# the trap self-heals next run instead of leaving the long form behind.
if grep -q '^VERSION_MAJOR' "$APP_DIR/VERSION" 2>/dev/null; then
	FW_MAJOR=$(sed -n 's/^VERSION_MAJOR[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$APP_DIR/VERSION" | head -1)
	FW_MINOR=$(sed -n 's/^VERSION_MINOR[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$APP_DIR/VERSION" | head -1)
else
	_v=$(tr -d '[:space:]' < "$APP_DIR/VERSION")
	FW_MAJOR="${_v%%.*}"
	FW_MINOR="${_v#*.}"
	FW_MINOR="${FW_MINOR%%.*}"
fi
FW_PATCH="${FW_PATCH:-0}"
for _f in "$FW_MAJOR" "$FW_MINOR" "$FW_PATCH"; do
	if ! [[ "$_f" =~ ^[0-9]+$ ]] || [[ "$_f" -gt 255 ]]; then
		echo "Error: version field '$_f' is not 0-255 (VERSION should read MAJOR.MINOR)." >&2
		exit 1
	fi
done

restore_version() { printf '%s.%s\n' "$FW_MAJOR" "$FW_MINOR" > "$APP_DIR/VERSION"; }
trap restore_version EXIT INT TERM

printf 'VERSION_MAJOR = %s\nVERSION_MINOR = %s\nPATCHLEVEL = %s\nVERSION_TWEAK = 0\nEXTRAVERSION =\n' \
	"$FW_MAJOR" "$FW_MINOR" "$FW_PATCH" > "$APP_DIR/VERSION"

# Zephyr puts VERSION on CMAKE_CONFIGURE_DEPENDS, so a fresh mtime every build
# would re-run cmake every build.  When the generated content is byte-identical
# to last time, hand back the old mtime instead.
_vgen="$BUILD_DIR/.version.gen"
if [[ -f "$_vgen" ]] && cmp -s "$_vgen" "$APP_DIR/VERSION"; then
	touch -r "$_vgen" "$APP_DIR/VERSION"
else
	mkdir -p "$BUILD_DIR"
	cp "$APP_DIR/VERSION" "$_vgen"
fi
echo "Version: $FW_MAJOR.$FW_MINOR.$FW_PATCH"

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
		# Bench fallback.  The Connect Kit is the usual target here and its
		# CMSIS-DAP probe doesn't always enumerate, so default to it rather
		# than the DK.  PROFILE=dk still forces the DK, and push_fw.sh sets
		# PROFILE explicitly for every deployed device.
		PROFILE=makerdiary
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
SIG="$PROFILE|$BOARD|$LOCAL_CONF"
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
	# The board definition is Makerdiary's and not kept in this repository, so
	# the first build fetches it.
	if [[ ! -d "$APP_DIR/boards/makerdiary/nrf9151_connectkit" ]]; then
		MAKERDIARY_REPO="$APP_DIR/ifmcu/.makerdiary-repo"
		if [[ ! -d "$MAKERDIARY_REPO" ]]; then
			echo "Fetching the Connect Kit board definition from makerdiary/nrf9151-connectkit"
			git clone --depth 1 https://github.com/makerdiary/nrf9151-connectkit.git "$MAKERDIARY_REPO"
		fi
		mkdir -p "$APP_DIR/boards"
		cp -R "$MAKERDIARY_REPO/boards/makerdiary" "$APP_DIR/boards/"
	fi
	if [[ -f "$APP_DIR/makerdiary.conf" ]]; then
		CONF_OVERLAYS+=("makerdiary.conf")
	fi
fi

if [[ -n "$LOCAL_CONF" ]]; then
	CONF_OVERLAYS+=("$LOCAL_CONF")
fi
# PROV=1 layers prov.conf on top (AT-host bridge for nRF Cloud onboarding).
if [[ "${PROV:-}" == "1" && -f "$APP_DIR/prov.conf" ]]; then
	CONF_OVERLAYS+=("prov.conf")
fi
# LTE_TEST=1 layers lte_power_test.conf on top (TX power / brown-out rig).
if [[ "${LTE_TEST:-}" == "1" && -f "$APP_DIR/lte_power_test.conf" ]]; then
	CONF_OVERLAYS+=("lte_power_test.conf")
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

# --- signing key -----------------------------------------------------------
# MCUboot installs only images signed with this key.  A bench build creates it
# if it is missing; a release (push_fw.sh sets FW_PATCH) must be signed with
# the key the fleet's bootloaders already trust, so it stops instead.
KEY_FILE="$APP_DIR/mcuboot_priv.pem"
if [[ ! -f "$KEY_FILE" ]]; then
	if [[ "$FW_PATCH" != 0 ]]; then
		echo "Error: $KEY_FILE not found.  A release has to be signed with the key" >&2
		echo "       your devices' bootloaders already trust." >&2
		exit 1
	fi
	echo "Creating the firmware signing key: $KEY_FILE"
	in_ncs python3 "$NCS_ROOT/bootloader/mcuboot/scripts/imgtool.py" keygen -t ecdsa-p256 -k "$KEY_FILE"
	chmod 600 "$KEY_FILE"
	echo "Back it up somewhere safe: boards flashed from now on only install updates signed with it."
fi
CMAKE_ARGS+=("-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=\"$KEY_FILE\"")

# --- the server's CA -------------------------------------------------------
# The tracker firmware stores src/ca_cert.h in the modem on its first boot and
# never replaces it, so a unit built with the wrong CA can never update from
# your server.  The header is generated here from certs/ca.crt, your server's
# CA certificate, and is not kept in the repository.  The board test,
# provisioning and LTE test builds never store a CA, so without certs/ca.crt
# they are given a header that holds none.
CA_CRT="$APP_DIR/certs/ca.crt"
CA_HEADER="$APP_DIR/src/ca_cert.h"
if [[ -f "$CA_CRT" ]]; then
	CA_NOTE="src/ca_cert.h updated from certs/ca.crt"
elif [[ "${PROV:-}" == 1 || "${LTE_TEST:-}" == 1 ]] \
	|| grep -qs '^CONFIG_APP_BOARD_TEST=y' "$APP_DIR/$LOCAL_CONF"; then
	CA_CRT=""
	CA_NOTE="src/ca_cert.h written without a CA, which this build never stores"
else
	echo "Error: certs/ca.crt not found.  Copy your server's CA certificate there first" >&2
	echo "       (/srv/l0destar/certs/ca.crt on the server): the tracker firmware stores" >&2
	echo "       the CA in the modem on its first boot and never replaces it." >&2
	exit 1
fi
mkdir -p "$BUILD_DIR"
{
	echo '#ifndef CA_CERT_H'
	echo '#define CA_CERT_H'
	echo ''
	if [[ -n "$CA_CRT" ]]; then
		echo 'static const char ca_cert_pem[] ='
		sed 's/.*/"&\\n"/' "$CA_CRT"
		echo ';'
	else
		echo 'static const char ca_cert_pem[] = "";'
	fi
	echo ''
	echo '#endif'
} > "$BUILD_DIR/.ca_cert.h"
if ! cmp -s "$BUILD_DIR/.ca_cert.h" "$CA_HEADER"; then
	cp "$BUILD_DIR/.ca_cert.h" "$CA_HEADER"
	echo "$CA_NOTE"
fi

EXTRA_ARGS=("--" "${CMAKE_ARGS[@]}")

# `west` must run from inside the NCS workspace so it can find the manifest;
# the app itself can live anywhere — we pass it as an absolute path.
in_ncs west build -p "$PRISTINE" -b "$BOARD" -d "$BUILD_DIR" "$APP_DIR" "${EXTRA_ARGS[@]}"

# Record what this build dir now holds (drives the switch check above).
echo "$SIG" > "$MARKER"

echo
echo "Built: $BUILD_DIR/merged.hex  (profile: $PROFILE, target: $BOARD)"
