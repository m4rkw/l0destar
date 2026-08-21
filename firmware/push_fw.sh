#!/usr/bin/env bash
# Build, sign and publish a firmware release for over-the-air update — one
# image per deployed device.
#
#   ./push_fw.sh                    build + push every device in remote.conf
#   ./push_fw.sh --device <imei>    just that one
#   ./push_fw.sh --no-build         push whatever is already in build_remote_*/
#   ./push_fw.sh --force            allow re-publishing a version <= the live one
#   ./push_fw.sh --list             show what remote.conf describes, push nothing
#   ./push_fw.sh --patch <n>        pin the patch number instead of deriving it
#
# VERSIONING
# ----------
# VERSION reads `0.4` and nothing else.  The patch number is derived here from
# what is actually published on the server — the highest MAJOR.MINOR.x among
# the images and manifests in $FW_DIR, plus one — and handed to build.sh as
# FW_PATCH, which generates the format Zephyr wants on the fly.  The counter
# lives with the fleet; the repo never carries a patch number.
#
# WHY PER-DEVICE
# --------------
# A single published image is only correct for units whose hardware matches
# whatever was on the bench when it was built.  It isn't: v3.0 boards ship
# with CAN and K-line both laid out and only one populated, and local.conf
# carries bench-only debug settings besides.  MCUboot won't save you — it
# checks the signature, not the hardware the image expects — so a CAN build
# installs cleanly on a K-line unit and then misbehaves in the field.
#
# So remote.conf describes the fleet by IMEI, this script builds each device
# its own image from [common] + that device's section (local.conf is not
# layered in at all), and publishes:
#
#   fw/l0destar-<ver>-<imei>.bin    the signed image for that unit
#   fw/manifest-<imei>.txt          version= / file= / board= for that unit
#
# The device already sends ?imei=<imei> on its manifest GET, so the server
# hands back that unit's manifest and nothing else.  A device missing from
# remote.conf gets no manifest, and the server stops advertising fota= to it —
# no update is the safe failure; a stranger's build is not.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

SERVER="${FW_SERVER:-a}"                 # ssh host
FW_DIR="${FW_DIR:-/var/www/tracker/fw}"
REMOTE_CONF="${REMOTE_CONF:-remote.conf}"
FRAG_DIR=.remote                         # generated Kconfig fragments
# Devices fetch from the telemetry TLS port (65481) — the only forwarded TCP
# path to the server; its listener doubles as the /fw/ HTTP endpoint.  Every
# read here goes to that endpoint the same way a tracker does: public
# hostname, private CA, no shell access.  So the checks exercise the path a
# device actually takes, port forward included, rather than loopback on the
# server.  ssh is used only for the writes — upload and manifest swap — since
# there is no HTTP write API and there should not be one.
#
# The hostname comes from CONFIG_APP_SERVER_HOST in remote.conf's [common]
# section (gitignored) so no endpoint is baked into the repo.  Override with
# FW_HOST or VERIFY_URL.
VERIFY_PORT="${VERIFY_PORT:-65481}"

BUILD=1
FORCE=0
LIST=0
ONLY=""
PIN_PATCH=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-build) BUILD=0 ;;
        --force)    FORCE=1 ;;
        --list)     LIST=1 ;;
        --device)   ONLY="${2:-}"; shift
                    [[ -n $ONLY ]] || { echo "--device needs an IMEI" >&2; exit 1; } ;;
        --patch)    PIN_PATCH="${2:-}"; shift
                    [[ $PIN_PATCH =~ ^[0-9]+$ ]] || { echo "--patch needs a number" >&2; exit 1; } ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

[[ -f $REMOTE_CONF ]] || {
    echo "error: $REMOTE_CONF not found — copy remote.conf.example and fill in" >&2
    echo "       the devices you have in the field (see FOTA.md)." >&2
    exit 1
}

# --- remote.conf ------------------------------------------------------------
# INI sections keyed by IMEI, plus a [common] section layered under every
# device.  Inside a section, lowercase `key = value` lines are build metadata
# and CONFIG_* lines are Kconfig fragment lines copied through verbatim.

# List the device sections (everything except [common]), in file order.
sections() {
    awk '
        /^[[:space:]]*[#;]/ { next }
        /^[[:space:]]*\[/ {
            s = $0
            sub(/^[[:space:]]*\[/, "", s); sub(/\][[:space:]]*\r?$/, "", s)
            if (s != "common") print s
        }
    ' "$REMOTE_CONF"
}

# Kconfig lines for a section, in file order.
conf_for() {
    awk -v want="$1" '
        /^[[:space:]]*[#;]/ { next }
        /^[[:space:]]*\[/ {
            sec = $0
            sub(/^[[:space:]]*\[/, "", sec); sub(/\][[:space:]]*\r?$/, "", sec)
            next
        }
        sec == want && /^[[:space:]]*CONFIG_[A-Za-z0-9_]+=/ {
            sub(/^[[:space:]]+/, ""); sub(/\r$/, ""); print
        }
    ' "$REMOTE_CONF"
}

# One metadata value (`key = value`) from a section; empty if unset.
meta_for() {
    awk -v want="$1" -v key="$2" '
        /^[[:space:]]*[#;]/ { next }
        /^[[:space:]]*\[/ {
            sec = $0
            sub(/^[[:space:]]*\[/, "", sec); sub(/\][[:space:]]*\r?$/, "", sec)
            next
        }
        sec == want {
            line = $0; sub(/\r$/, "", line)
            if (match(line, "^[[:space:]]*" key "[[:space:]]*=")) {
                v = substr(line, RSTART + RLENGTH)
                sub(/^[[:space:]]+/, "", v); sub(/[[:space:]]+$/, "", v)
                print v; exit
            }
        }
    ' "$REMOTE_CONF"
}

DEVICES=()
while IFS= read -r _d; do
    if [[ -n $_d ]]; then DEVICES+=("$_d"); fi
done < <(sections)
[[ ${#DEVICES[@]} -gt 0 ]] || { echo "error: $REMOTE_CONF lists no devices" >&2; exit 1; }

if [[ -n $ONLY ]]; then
    found=0
    for d in "${DEVICES[@]}"; do
        if [[ $d == "$ONLY" ]]; then found=1; fi
    done
    [[ $found -eq 1 ]] || {
        echo "error: $ONLY is not in $REMOTE_CONF (have: ${DEVICES[*]})" >&2
        exit 1
    }
    DEVICES=("$ONLY")
fi

FW_HOST="${FW_HOST:-$(conf_for common | sed -n 's/^CONFIG_APP_SERVER_HOST="\(.*\)"$/\1/p' | tail -1)}"
if [[ -z "$FW_HOST" ]]; then
    echo "error: no server hostname — set CONFIG_APP_SERVER_HOST in remote.conf" >&2
    echo "       [common] (see QUICKSTART.md) or pass FW_HOST=..." >&2
    exit 1
fi
VERIFY_URL="${VERIFY_URL:-https://${FW_HOST}:${VERIFY_PORT}}"
CA_LOCAL="${CA_LOCAL:-certs/ca.crt}"      # the l0destar CA, this side

if [[ ! -f $CA_LOCAL ]]; then
    echo "error: $CA_LOCAL not found — need the l0destar CA to verify the" >&2
    echo "       firmware endpoint (see certs/gen_certs.sh), or set CA_LOCAL." >&2
    exit 1
fi

# A request straight at the firmware endpoint, exactly as a device makes it:
# public hostname, port 65481, verified against the private CA.
fw_curl() { curl -s --cacert "$CA_LOCAL" --max-time 15 "$@"; }

ver_num() { echo "$1" | awk -F. '{printf "%d", $1*65536 + $2*256 + $3}'; }

# VERSION reads MAJOR.MINOR; the patch is derived below.  Accept the generated
# long form too, in case a build was killed before build.sh could restore it.
if grep -q '^VERSION_MAJOR' VERSION 2>/dev/null; then
    MAJOR=$(sed -n 's/^VERSION_MAJOR[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p' VERSION | head -1)
    MINOR=$(sed -n 's/^VERSION_MINOR[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p' VERSION | head -1)
else
    _v=$(tr -d '[:space:]' < VERSION)
    MAJOR="${_v%%.*}"
    MINOR="${_v#*.}"
    MINOR="${MINOR%%.*}"
fi
if ! [[ $MAJOR =~ ^[0-9]+$ && $MINOR =~ ^[0-9]+$ ]]; then
    echo "error: VERSION should read MAJOR.MINOR (e.g. 0.4); got '$(cat VERSION)'" >&2
    exit 1
fi

# Everything MAJOR.MINOR.x ever published, asked for the same way a tracker
# asks: an HTTPS GET on the firmware endpoint, verified against the same
# private CA.  No shell access needed to work out the next version — the
# server answers from its own fw/ directory, images and manifests alike.
#
# Devices only install strictly newer, so undershooting here would publish a
# release the fleet silently ignores: a failed probe is fatal, never "start
# again from 0".
PUBLISHED=""
fetch_published() {
    local body code
    body=$(fw_curl -w '\n%{http_code}' "$VERIFY_URL/fw/published.txt") || {
        echo "error: couldn't reach $VERIFY_URL/fw/published.txt" >&2
        echo "       (is $FW_HOST:$VERIFY_PORT reachable, and is $CA_LOCAL current?)" >&2
        return 1
    }
    code=${body##*$'\n'}
    if [[ $code != 200 ]]; then
        echo "error: $VERIFY_URL/fw/published.txt returned HTTP $code" >&2
        echo "       — the server needs the main.py that serves it." >&2
        return 1
    fi
    PUBLISHED="${body%$'\n'*}"
}

next_patch() {
    printf '%s\n' "$PUBLISHED" | awk -F. -v maj="$MAJOR" -v min="$MINOR" '
        $1 == maj && $2 == min && $3 ~ /^[0-9]+$/ && $3 + 0 >= max { max = $3 + 0; seen = 1 }
        END { print seen ? max + 1 : 0 }
    '
}

if [[ $LIST -eq 1 || $BUILD -eq 1 ]]; then
    fetch_published || exit 1
fi

if [[ $LIST -eq 1 ]]; then
    echo "$REMOTE_CONF -> next version $MAJOR.$MINOR.$(next_patch), host $FW_HOST"
    for imei in "${DEVICES[@]}"; do
        _name=$(meta_for "$imei" name)
        _prof=$(meta_for "$imei" profile)
        printf '\n[%s] %s\n' "$imei" "$_name"
        printf '  profile: %s\n' "${_prof:-auto-detect}"
        { conf_for common; conf_for "$imei"; } | sed 's/^/  /'
    done
    exit 0
fi

# --- version ----------------------------------------------------------------
mkdir -p "$FRAG_DIR"

if [[ $BUILD -eq 1 ]]; then
    PATCH="${PIN_PATCH:-$(next_patch)}"
    SRC_VER="$MAJOR.$MINOR.$PATCH"
    if [[ $PATCH -gt 255 ]]; then
        echo "error: patch $PATCH exceeds the 255 the MCUboot image header holds" >&2
        echo "       — bump the minor in VERSION (e.g. $MAJOR.$MINOR -> $MAJOR.$((MINOR + 1)))" >&2
        echo "         and the count restarts from 0." >&2
        exit 1
    fi
    echo "version $SRC_VER (patch derived from $SERVER:$FW_DIR)"
else
    # Nothing is being rebuilt, so the images already carry a version — adopt
    # it rather than inventing a new one.  Read it below from the first
    # device's .config and hold every other device to the same value.
    SRC_VER=""
fi

# --- per-device build + publish ---------------------------------------------
ssh "$SERVER" "mkdir -p $FW_DIR"

for imei in "${DEVICES[@]}"; do
    if [[ ! $imei =~ ^[0-9]{15}$ ]]; then
        echo "error: '$imei' is not a 15-digit IMEI — fix $REMOTE_CONF" >&2
        exit 1
    fi
    NAME=$(meta_for "$imei" name)
    PROFILE=$(meta_for "$imei" profile)
    BOARD=$(meta_for "$imei" board)
    BUILD_SUBDIR="build_remote_$imei"
    FRAG="$FRAG_DIR/$imei.conf"

    echo
    echo "=== $imei${NAME:+ ($NAME)} ================================================"

    # [common] first, the device's own lines second so they win.
    {
        echo "# Generated by push_fw.sh from $REMOTE_CONF — do not edit."
        echo "# Device $imei${NAME:+ ($NAME)}, version $SRC_VER."
        conf_for common
        conf_for "$imei"
    } > "$FRAG"

    if [[ $BUILD -eq 1 ]]; then
        # LOCAL_CONF replaces local.conf rather than layering on top of it:
        # nothing from the bench reaches a deployed unit.
        BUILD_ENV=(LOCAL_CONF="$FRAG" BUILD_SUBDIR="$BUILD_SUBDIR" FW_PATCH="$PATCH")
        if [[ -n $PROFILE ]]; then BUILD_ENV+=(PROFILE="$PROFILE"); fi
        if [[ -n $BOARD   ]]; then BUILD_ENV+=(BOARD="$BOARD");     fi
        env "${BUILD_ENV[@]}" ./build.sh
    fi

    IMG="$BUILD_SUBDIR/firmware/zephyr/zephyr.signed.bin"
    CONF="$BUILD_SUBDIR/firmware/zephyr/.config"
    [[ -f $IMG ]] || { echo "error: $IMG not found — build first" >&2; exit 1; }
    [[ -f $CONF ]] || { echo "error: $CONF not found — build first" >&2; exit 1; }

    # The version the image was actually signed with — never a hand-typed one.
    VER=$(sed -n 's/^CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="\(.*\)+.*"$/\1/p' "$CONF")
    [[ -n $VER ]] || { echo "error: couldn't read signed version from $CONF" >&2; exit 1; }
    if [[ -z $SRC_VER ]]; then
        SRC_VER="$VER"                     # --no-build: first image sets it
        echo "version $SRC_VER (adopted from $CONF, --no-build)"
    elif [[ $VER != "$SRC_VER" ]]; then
        echo "error: $imei build is $VER but this release is $SRC_VER —" >&2
        echo "       rebuild (drop --no-build)." >&2
        exit 1
    fi

    # Refuse to publish a build carrying bench settings.  remote.conf keeps
    # local.conf out, but overlays that are not bench-specific by name still
    # reach a deployed image — makerdiary.conf applies to every Connect Kit
    # build and once carried a DEBUG_IGNITION override that local.conf masked
    # on the bench and nothing masked here.  Assert the production values
    # rather than trusting the layering.
    bench_check() {   # $1 = config symbol, $2 = required value
        local got
        got=$(sed -n "s/^CONFIG_$1=//p" "$CONF")
        if [[ -z $got ]] && grep -q "^# CONFIG_$1 is not set" "$CONF"; then
            got=n
        fi
        if [[ $got != "$2" ]]; then
            echo "error: $imei build has CONFIG_$1=${got:-<unset>}, expected $2" >&2
            echo "       — a bench override reached a deployed image.  Check the" >&2
            echo "       overlays build.sh layered (makerdiary.conf, prj.conf)." >&2
            exit 1
        fi
    }
    bench_check APP_DEBUG_IGNITION -1      # live ignition GPIO, not forced
    bench_check APP_DEBUG_BATTERY_MV 0     # live INA228 reading, not forced
    bench_check APP_CAN_TEST n             # boot-time loopback harnesses
    bench_check APP_KLINE_TEST n
    bench_check APP_ACCEL_TEST n

    # Board identity as the device composes it in fota.c: the carrier board
    # plus the fitted interfaces.  Published as board= so a device handed the
    # wrong manifest refuses the image instead of installing it.
    BID=$(sed -n 's/^CONFIG_APP_BOARD_ID="\(.*\)"$/\1/p' "$CONF")
    [[ -n $BID ]] || { echo "error: no CONFIG_APP_BOARD_ID in $CONF" >&2; exit 1; }
    if grep -q '^CONFIG_APP_BOARD_HAS_CAN=y'   "$CONF"; then BID="$BID+can";   fi
    if grep -q '^CONFIG_APP_BOARD_HAS_KLINE=y' "$CONF"; then BID="$BID+kline"; fi
    if grep -q '^CONFIG_APP_BOARD_HAS_AIO=y'   "$CONF"; then BID="$BID+aio";   fi

    # Devices only install strictly-newer versions, so pushing <= what this
    # unit is already offered would be a silent no-op.
    LIVE=$(fw_curl "$VERIFY_URL/fw/manifest.txt?imei=$imei&v=0.0.0" \
           | sed -n 's/^version=//p' || true)
    if [[ -n $LIVE && $FORCE -eq 0 ]]; then
        if [[ $(ver_num "$VER") -le $(ver_num "$LIVE") ]]; then
            echo "error: $imei is already offered $LIVE, this build is $VER —" >&2
            echo "       it won't install.  Bump VERSION (or --force to republish)." >&2
            exit 1
        fi
    fi

    FILE="l0destar-$VER-$imei.bin"
    SIZE=$(stat -f %z "$IMG")
    echo "publishing $VER board=$BID ($FILE, $SIZE bytes) -> $SERVER:$FW_DIR"
    scp -q "$IMG" "$SERVER:$FW_DIR/$FILE"

    # Manifest last, atomically: mv within the same directory, so a device
    # fetching mid-push sees either the old release or the new one, never a
    # manifest naming a half-uploaded image.
    ssh "$SERVER" "printf 'version=%s\nfile=fw/%s\nboard=%s\n' '$VER' '$FILE' '$BID' \
                   > $FW_DIR/.manifest-$imei.tmp \
                   && mv $FW_DIR/.manifest-$imei.tmp $FW_DIR/manifest-$imei.txt"

    # Verify the endpoint end-to-end the way this device will: fetch the
    # manifest through the same ?imei= query the firmware sends, check the CA
    # vouches for the cert, and confirm the image comes back 206 to a range
    # request (the nRF91 downloads in 2 KB ranges over modem TLS — a server
    # that ignores Range breaks OTA).
    GOT=$(fw_curl "$VERIFY_URL/fw/manifest.txt?imei=$imei&v=0.0.0") || GOT=""
    GOT_VER=$(sed -n 's/^version=//p' <<<"$GOT")
    GOT_FILE=$(sed -n 's/^file=//p' <<<"$GOT")
    GOT_BID=$(sed -n 's/^board=//p' <<<"$GOT")
    if [[ $GOT_VER != "$VER" || $GOT_FILE != "fw/$FILE" || $GOT_BID != "$BID" ]]; then
        echo "error: manifest for $imei served as:" >&2
        sed 's/^/         /' <<<"$GOT" >&2
        echo "       expected version=$VER file=fw/$FILE board=$BID" >&2
        echo "       (does the server resolve manifest.txt?imei= per device?)" >&2
        exit 1
    fi
    RANGE=$(fw_curl -o /dev/null -w '%{http_code} %{size_download}' \
            -H 'Range: bytes=0-2047' "$VERIFY_URL/fw/$FILE")
    if [[ $RANGE != "206 2048" ]]; then
        echo "error: range request returned '$RANGE', expected '206 2048'" >&2
        exit 1
    fi
    echo "verified: manifest live for $imei, CA trusted, range requests OK"
done

echo
echo "done: ${#DEVICES[@]} device(s) will pull $SRC_VER on their next telemetry"
echo "      exchange or power-on"
