#!/usr/bin/env bash
# board_test.sh — interactive l0destar board bring-up.
#
# Asks which PCB is on the bench (plus which OBD interface is populated on
# v3.1+ boards and, if local.conf doesn't already have one, an APN), generates
# a Kconfig overlay (board_test.conf, gitignored), builds the firmware with
# CONFIG_APP_BOARD_TEST=y, flashes it, and drops into a screen session on the
# serial console where src/board_test.c walks the operator through every
# fitted subsystem (rails, ignition, voltage, IMU, GPS, modem, OBD loopbacks).
#
# Environment overrides, all optional:
#   SERIAL=/dev/cu.usbmodemXXX   serial console port (skips the port menu)
#   PROFILE=dk|makerdiary         forwarded to build.sh (bench DK: PROFILE=dk)
#   BOARD=<zephyr target>         forwarded to build.sh
#
# local.conf is not layered into the test build (its board choice and bench
# debug overrides would fight the test), but these values are read from it:
#   CONFIG_APP_APN                     used for the modem test (falls back to
#                                      the profile overlay, e.g. makerdiary.conf,
#                                      then prj.conf — the same layering as
#                                      build.sh — before asking)
#   CONFIG_APP_BOARD_TEST_HIDE_COORDS  =y hides the GPS fix lat/lon (demos)
#   CONFIG_APP_CRASH_THRESHOLD_MG      impact threshold (default 1200 mg here:
#                                      a firm desk bang is 1.5-3 g)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

FRAGMENT=board_test.conf
SESSION=l0destar-test
BAUD=115200

# --- board table: newest first; first entry is the default -------------------
# id|menu label|Kconfig board select|flags (obd = ask which OBD interface)
BOARDS=(
    "v3.3|l0destar v3.3  (v3.1 map, fixed L-line + L sense)|CONFIG_APP_BOARD_L0DESTAR_V3_3=y|obd"
    "v3.1|l0destar v3.1  (combined CAN/K-line, rail sensing)|CONFIG_APP_BOARD_L0DESTAR_V3_1=y|obd"
    "v3.0|l0destar v3.0  (combined CAN/K-line, jumper-selected)|CONFIG_APP_BOARD_L0DESTAR_V3_0=y|"
    "v2.6c|l0destar v2.6C (CAN)|CONFIG_APP_BOARD_L0DESTAR_V2_6_CAN=y|"
    "v2.6k|l0destar v2.6K (ISO-9141 K-line)|CONFIG_APP_BOARD_L0DESTAR_V2_6_KLINE=y|"
    "v2.6m|l0destar v2.6M (micro, no OBD)|CONFIG_APP_BOARD_L0DESTAR_V2_6_MICRO=y|"
    "v2.5c|l0destar v2.5C (CAN)|CONFIG_APP_BOARD_L0DESTAR_V2_5_CAN=y|"
    "v2.5k|l0destar v2.5K (ISO-9141 K-line)|CONFIG_APP_BOARD_L0DESTAR_V2_5_KLINE=y|"
    "v2.5m|l0destar v2.5M (micro, no OBD)|CONFIG_APP_BOARD_L0DESTAR_V2_5_MICRO=y|"
    "v2.1|l0destar v2.1  (CAN + ISO-9141 + AIO + relay)|CONFIG_APP_BOARD_L0DESTAR_V2_1=y|"
    "v2.1m|l0destar v2.1 mini (no OBD, 5 LEDs, relay)|CONFIG_APP_BOARD_L0DESTAR_V2_1_MINI=y|"
    "bench|bench (DK / Connect Kit + breadboard)|CONFIG_APP_BOARD_BENCH=y|"
)

field() { echo "$1" | cut -d'|' -f"$2"; }

# Pull the (uncommented) value of a CONFIG_FOO="..." string out of a file.
conf_str() {
    [[ -f "$2" ]] || return 0
    sed -n "s/^[[:space:]]*$1=\"\(.*\)\".*/\1/p" "$2" | tail -1
}
local_conf_str() { conf_str "$1" local.conf; }

# Resolve the build profile the way build.sh does (PROFILE env wins, else the
# Makerdiary CMSIS-DAP probe, else makerdiary) so the profile overlay that the
# test build will actually layer is the one consulted here.
resolve_profile() {
    if [[ -n "${PROFILE:-}" ]]; then echo "$PROFILE"; return; fi
    if command -v pyocd >/dev/null 2>&1 \
       && pyocd list 2>/dev/null | grep -qiE 'makerdiary'; then
        echo makerdiary
    else
        echo makerdiary
    fi
}

# Effective CONFIG_APP_APN in build.sh's layering order: local.conf beats the
# profile overlay beats prj.conf.  Prints the value and, on a second line,
# the file it came from.
resolve_apn() {
    local v
    v="$(local_conf_str CONFIG_APP_APN)"
    if [[ -n "$v" ]]; then printf '%s\nlocal.conf\n' "$v"; return; fi
    if [[ "$(resolve_profile)" == makerdiary ]]; then
        v="$(conf_str CONFIG_APP_APN makerdiary.conf)"
        if [[ -n "$v" ]]; then printf '%s\nmakerdiary.conf\n' "$v"; return; fi
    fi
    v="$(conf_str CONFIG_APP_APN prj.conf)"
    if [[ -n "$v" ]]; then printf '%s\nprj.conf\n' "$v"; return; fi
}
local_conf_has_y() {
    [[ -f local.conf ]] && grep -qE "^[[:space:]]*$1=y" local.conf
}
local_conf_int() {
    [[ -f local.conf ]] || return 0
    sed -n "s/^[[:space:]]*$1=\([0-9][0-9]*\).*/\1/p" local.conf | tail -1
}

# --- 1. board version --------------------------------------------------------
echo "l0destar board test"
echo
echo "Select board version:"
for i in "${!BOARDS[@]}"; do
    n=$((i + 1))
    label="$(field "${BOARDS[$i]}" 2)"
    if [[ $n -eq 1 ]]; then
        printf "  %2d) %s   [default]\n" "$n" "$label"
    else
        printf "  %2d) %s\n" "$n" "$label"
    fi
done
read -r -p "Board [1]: " sel
sel="${sel:-1}"
if ! [[ "$sel" =~ ^[0-9]+$ ]] || (( sel < 1 || sel > ${#BOARDS[@]} )); then
    echo "Invalid selection: $sel" >&2
    exit 1
fi
entry="${BOARDS[$((sel - 1))]}"
BOARD_ID="$(field "$entry" 1)"
BOARD_KCONF="$(field "$entry" 3)"
BOARD_FLAGS="$(field "$entry" 4)"
echo "  -> $(field "$entry" 2)"

# --- 2. OBD interface (v3.1+: one populated variant per board) ---------------
OBD_MODE=""
if [[ "$BOARD_FLAGS" == *obd* ]]; then
    echo
    echo "OBD interface populated on this board:"
    echo "   1) none                 [default]"
    echo "   2) CAN"
    echo "   3) K-line / ISO-9141"
    read -r -p "OBD [1]: " obd
    obd="${obd:-1}"
    case "$obd" in
        1) OBD_MODE=0; echo "  -> no OBD" ;;
        2) OBD_MODE=1; echo "  -> CAN" ;;
        3) OBD_MODE=2; echo "  -> K-line / ISO-9141" ;;
        *) echo "Invalid selection: $obd" >&2; exit 1 ;;
    esac
elif [[ "$BOARD_ID" == "v3.0" ]]; then
    echo
    echo "Note: v3.0 carries both OBD circuits and the build assumes both are"
    echo "populated; if one isn't, its loopback test will just fail."
fi

# --- 3. APN ------------------------------------------------------------------
echo
APN_INFO="$(resolve_apn)"
APN="$(echo "$APN_INFO" | sed -n 1p)"
APN_SRC="$(echo "$APN_INFO" | sed -n 2p)"
SKIP_MODEM=0
if [[ -n "$APN" ]]; then
    echo "APN from $APN_SRC: $APN"
else
    echo "No APN configured (local.conf, profile overlay or prj.conf)."
    read -r -p "APN for the modem test (empty = skip modem test): " APN
    if [[ -z "$APN" ]]; then
        SKIP_MODEM=1
        echo "  -> modem + DNS test will be skipped"
    else
        echo "  -> using APN $APN"
    fi
fi

# --- generate the overlay ----------------------------------------------------
CRASH_MG="$(local_conf_int CONFIG_APP_CRASH_THRESHOLD_MG)"
CRASH_MG="${CRASH_MG:-1200}"
{
    echo "# Generated by board_test.sh on $(date '+%Y-%m-%d %H:%M:%S') — do not commit."
    echo "$BOARD_KCONF"
    [[ -n "$OBD_MODE" ]] && echo "CONFIG_APP_OBD_MODE=$OBD_MODE"
    echo "CONFIG_APP_BOARD_TEST=y"
    echo "CONFIG_LOG_MODE_IMMEDIATE=y"
    echo "# bounded registration attempt for the bench (default is 600 s)"
    echo "CONFIG_LTE_NETWORK_TIMEOUT=180"
    echo "# desk-bang friendly impact threshold"
    echo "CONFIG_APP_CRASH_THRESHOLD_MG=$CRASH_MG"
    [[ -n "$APN" ]] && echo "CONFIG_APP_APN=\"$APN\""
    [[ "$SKIP_MODEM" == 1 ]] && echo "CONFIG_APP_BOARD_TEST_SKIP_MODEM=y"
    if local_conf_has_y CONFIG_APP_BOARD_TEST_HIDE_COORDS; then
        echo "CONFIG_APP_BOARD_TEST_HIDE_COORDS=y"
    fi
} > "$FRAGMENT"
echo
echo "Wrote $FRAGMENT:"
sed 's/^/    /' "$FRAGMENT"

# --- probe / power -----------------------------------------------------------
# The bench powers the unit through a switched USB hub (`power on`).  If no
# debug probe is visible yet and that command exists, try it.
no_probe() {
    pyocd list 2>/dev/null | grep -qi "no available debug probes"
}
if command -v pyocd >/dev/null 2>&1 && command -v power >/dev/null 2>&1 \
   && no_probe; then
    echo
    echo "No debug probe visible — running 'power on'..."
    power on || true
    for _ in $(seq 1 15); do
        no_probe || break
        sleep 1
    done
fi

# --- build + flash -----------------------------------------------------------
echo
echo "Building test firmware..."
LOCAL_CONF="$FRAGMENT" ./build.sh

echo
echo "Flashing..."
./flash.sh

# --- serial console ----------------------------------------------------------
if [[ -n "${SERIAL:-}" ]]; then
    PORT="$SERIAL"
else
    shopt -s nullglob
    ports=(/dev/cu.usbmodem*)
    shopt -u nullglob
    if [[ ${#ports[@]} -eq 0 ]]; then
        echo "No /dev/cu.usbmodem* serial port found." >&2
        echo "Power/cable? (bench: try 'power on')  Or set SERIAL=/dev/..." >&2
        exit 1
    elif [[ ${#ports[@]} -eq 1 ]]; then
        PORT="${ports[0]}"
    else
        echo
        echo "Serial ports (Connect Kit's DAPLink exposes two CDC ports; the"
        echo "console is usually the first — if it's silent, rerun with the other):"
        for i in "${!ports[@]}"; do
            printf "  %2d) %s\n" "$((i + 1))" "${ports[$i]}"
        done
        read -r -p "Port [1]: " psel
        psel="${psel:-1}"
        if ! [[ "$psel" =~ ^[0-9]+$ ]] || (( psel < 1 || psel > ${#ports[@]} )); then
            echo "Invalid selection: $psel" >&2
            exit 1
        fi
        PORT="${ports[$((psel - 1))]}"
    fi
fi
echo
echo "Console: $PORT @ $BAUD"

# Fresh screen session, started detached so it's already capturing when the
# target comes out of reset (output lands in scrollback: Ctrl-A [ ).
# `screen -ls` exits 1 whenever no session is attached, so under pipefail a
# plain `screen -ls | grep -q` reads as failure even when the session exists.
session_up() {
    local out
    out="$(screen -ls 2>/dev/null || true)"
    grep -q "\.$SESSION[[:space:]]" <<<"$out"
}
# Kill every prior test session, not just one (a failed run can leave several).
while session_up; do
    screen -S "$SESSION" -X quit >/dev/null 2>&1 || break
    sleep 0.2
done
# The DAPLink CDC port can report busy (or drop and re-enumerate) for a few
# seconds after pyocd has been at the probe, so retry rather than giving up on
# the first attempt.
opened=0
for _ in $(seq 1 20); do
    if [[ -e "$PORT" ]]; then
        screen -dmS "$SESSION" -L "$PORT" "$BAUD" 2>/dev/null || true
        sleep 0.5
        if session_up; then
            opened=1
            break
        fi
    fi
    sleep 0.5
done
if [[ $opened -ne 1 ]]; then
    echo "screen could not open $PORT after 10 s." >&2
    if [[ -e "$PORT" ]]; then
        holder="$(lsof "$PORT" 2>/dev/null | tail -n +2)"
        if [[ -n "$holder" ]]; then
            echo "Held by:" >&2
            echo "$holder" | sed 's/^/  /' >&2
        else
            echo "Nothing has it open (lsof) — the device may still be" >&2
            echo "re-enumerating; try again." >&2
        fi
    else
        echo "$PORT has gone away — the probe re-enumerated?  Re-run." >&2
    fi
    exit 1
fi

echo "Resetting target..."
./reset.sh

echo
echo "Attaching to the console.  In the test session:"
echo "  - press ENTER to start the test sequence"
echo "  - Ctrl-A [  scrollback     Ctrl-A d  detach (screen -r $SESSION)"
echo "  - Ctrl-A k  kill session   (log: ./screenlog.0)"
echo
exec screen -r "$SESSION"
