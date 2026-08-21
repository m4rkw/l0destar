#!/usr/bin/env bash
# Build, sign and publish a firmware release for over-the-air update.
#
#   ./push_fw.sh              build, then push the signed image + manifest
#   ./push_fw.sh --no-build   push whatever is already in build/
#   ./push_fw.sh --force      allow re-publishing a version <= the live one
#
# The signed image (build/firmware/zephyr/zephyr.signed.bin — MCUboot header +
# TF-M + app, signature applied during the build) is copied to the server and
# the manifest is updated last, atomically, so the endpoint never advertises a
# file that isn't fully uploaded.  Devices learn about the release from the
# fota=<version> tag the tracker server appends to every telemetry response,
# compare it against their running build, and pull + install this image on
# their next wake.  Bump VERSION before running — devices only install
# strictly-newer versions.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

SERVER="${FW_SERVER:-a}"                 # ssh host
FW_DIR="${FW_DIR:-/var/www/tracker/fw}"
# Devices fetch from the telemetry TLS port (65481) — the only forwarded TCP
# path to the server; its listener doubles as the /fw/ HTTP endpoint.  The
# LAN can't hairpin through the public IP, so verification curls run on the
# server itself against 127.0.0.1 with the production SNI.
#
# The hostname comes from CONFIG_APP_SERVER_HOST in local.conf (gitignored) so
# no endpoint is baked into the repo.  Override with FW_HOST or VERIFY_URL.
VERIFY_PORT="${VERIFY_PORT:-65481}"
FW_HOST="${FW_HOST:-$(sed -n 's/^CONFIG_APP_SERVER_HOST="\(.*\)"$/\1/p' local.conf 2>/dev/null | tail -1)}"
if [[ -z "$FW_HOST" ]]; then
    echo "error: no server hostname — set CONFIG_APP_SERVER_HOST in local.conf" >&2
    echo "       (see QUICKSTART.md) or pass FW_HOST=..." >&2
    exit 1
fi
VERIFY_URL="${VERIFY_URL:-https://${FW_HOST}:${VERIFY_PORT}}"
VERIFY_RESOLVE="--resolve ${FW_HOST}:${VERIFY_PORT}:127.0.0.1"

BUILD=1
FORCE=0
for arg in "$@"; do
    case "$arg" in
        --no-build) BUILD=0 ;;
        --force)    FORCE=1 ;;
        *) echo "unknown option: $arg" >&2; exit 1 ;;
    esac
done

if [[ $BUILD -eq 1 ]]; then
    ./build.sh
fi

IMG=build/firmware/zephyr/zephyr.signed.bin
CONF=build/firmware/zephyr/.config
[[ -f $IMG ]] || { echo "error: $IMG not found — build first" >&2; exit 1; }

# The version the image was actually signed with (from the VERSION file via
# CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION) — never trust a hand-typed one.
VER=$(sed -n 's/^CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="\(.*\)+.*"$/\1/p' "$CONF")
[[ -n $VER ]] || { echo "error: couldn't read signed version from $CONF" >&2; exit 1; }

# Refuse a stale build: the image must embed the current VERSION file.
SRC_VER=$(awk -F' = ' '/^VERSION_MAJOR/{a=$2} /^VERSION_MINOR/{b=$2} /^PATCHLEVEL/{c=$2} END{print a"."b"."c}' VERSION)
if [[ $VER != "$SRC_VER" ]]; then
    echo "error: build is $VER but VERSION file says $SRC_VER — rebuild" >&2
    exit 1
fi

# Devices only install strictly-newer versions, so pushing <= the live
# manifest would be a silent no-op fleet-wide.  To roll back, bump VERSION
# and rebuild the old code under the higher number.
ver_num() { echo "$1" | awk -F. '{printf "%d", $1*65536 + $2*256 + $3}'; }
LIVE=$(ssh "$SERVER" "sed -n 's/^version=//p' $FW_DIR/manifest.txt 2>/dev/null" || true)
if [[ -n $LIVE && $FORCE -eq 0 ]]; then
    if [[ $(ver_num "$VER") -le $(ver_num "$LIVE") ]]; then
        echo "error: live manifest is $LIVE, this build is $VER — devices" >&2
        echo "       won't install it.  Bump VERSION (or --force to republish)." >&2
        exit 1
    fi
fi

FILE="l0destar-$VER.bin"
SIZE=$(stat -f %z "$IMG")
echo "publishing $VER ($FILE, $SIZE bytes) -> $SERVER:$FW_DIR"

ssh "$SERVER" "mkdir -p $FW_DIR"
scp -q "$IMG" "$SERVER:$FW_DIR/$FILE"

# Manifest last, atomically: mv within the same directory, so a device
# fetching mid-push sees either the old release or the new one, never a
# manifest naming a half-uploaded image.
ssh "$SERVER" "printf 'version=%s\nfile=fw/%s\n' '$VER' '$FILE' > $FW_DIR/.manifest.tmp \
               && mv $FW_DIR/.manifest.tmp $FW_DIR/manifest.txt"

# Verify the endpoint end-to-end the way a device will: the l0destar CA must
# vouch for the cert, the manifest must advertise what we just pushed, and
# the image must come back 206 to a range request (the nRF91 downloads in
# 2 KB ranges over modem TLS — a server that ignores Range breaks OTA).
CA=/var/www/tracker/certs/ca.crt
GOT=$(ssh "$SERVER" "curl -s --cacert $CA $VERIFY_RESOLVE --max-time 15 \
      '$VERIFY_URL/fw/manifest.txt'" | sed -n 's/^version=//p') || GOT=""
if [[ $GOT != "$VER" ]]; then
    echo "error: $VERIFY_URL/fw/manifest.txt serves '$GOT', expected '$VER'" >&2
    exit 1
fi
RANGE=$(ssh "$SERVER" "curl -s --cacert $CA $VERIFY_RESOLVE --max-time 15 \
        -o /dev/null -w '%{http_code} %{size_download}' \
        -H 'Range: bytes=0-2047' '$VERIFY_URL/fw/$FILE'")
if [[ $RANGE != "206 2048" ]]; then
    echo "error: range request returned '$RANGE', expected '206 2048'" >&2
    exit 1
fi
echo "verified: manifest live, CA trusted, range requests OK"

echo "done: devices will pull $VER on their next telemetry exchange or power-on"
