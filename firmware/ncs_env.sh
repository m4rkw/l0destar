# Sourced by build.sh and ifmcu/build.sh: finds the nRF Connect SDK and defines
# in_ncs, which runs a command inside its toolchain from the SDK's workspace.
#
# The caller sets NCS_VERSION.  NCS_ROOT defaults to where nRF Util's
# sdk-manager puts that version: /opt/nordic/ncs/<version> on macOS,
# ~/ncs/<version> on Linux.
#
# The toolchain is nRF Util's when it has one installed for the version, which
# is the case on macOS, Windows and x86_64 Linux.  Nordic publishes no toolchain
# for arm64 Linux, so there the SDK is installed with west and the Zephyr SDK,
# and west comes from a virtual environment inside the SDK directory
# (<NCS_ROOT>/.venv) or from PATH - see the board setup prerequisites in the
# documentation.

if [[ -z "${NCS_ROOT:-}" ]]; then
	for _candidate in "/opt/nordic/ncs/$NCS_VERSION" "$HOME/ncs/$NCS_VERSION"; do
		if [[ -d "$_candidate" ]]; then
			NCS_ROOT="$_candidate"
			break
		fi
	done
fi
if [[ -z "${NCS_ROOT:-}" || ! -d "$NCS_ROOT" ]]; then
	echo "Error: nRF Connect SDK $NCS_VERSION is not in /opt/nordic/ncs or ~/ncs; set NCS_ROOT." >&2
	exit 1
fi

if command -v nrfutil >/dev/null 2>&1 \
	&& nrfutil sdk-manager toolchain list 2>/dev/null | grep -q "^$NCS_VERSION[[:space:]]"; then
	in_ncs() {
		nrfutil sdk-manager toolchain launch --ncs-version "$NCS_VERSION" --chdir "$NCS_ROOT" -- "$@"
	}
else
	if [[ -x "$NCS_ROOT/.venv/bin/west" ]]; then
		PATH="$NCS_ROOT/.venv/bin:$PATH"
	fi
	if ! command -v west >/dev/null 2>&1; then
		echo "Error: no toolchain for nRF Connect SDK $NCS_VERSION: nRF Util has none" >&2
		echo "       installed, and west is not in $NCS_ROOT/.venv or on PATH." >&2
		exit 1
	fi
	in_ncs() {
		(cd "$NCS_ROOT" && "$@")
	}
fi
