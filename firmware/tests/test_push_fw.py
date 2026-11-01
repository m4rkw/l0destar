"""Host tests for the board id push_fw.sh publishes, and --as-board, the option
that moves a unit from one board id to another.

push_fw.sh is run as it is, in a scratch directory, against a stand-in for the
server: a curl that answers from a directory of manifests, and an ssh and scp
that act on that directory.  With --no-build the image is whatever is in the
fake build directory, so nothing is compiled.

    uv run --python 3.12 --with pytest pytest -q firmware/tests/test_push_fw.py
"""
import os
import shutil
import stat
import subprocess

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "push_fw.sh")
IMEI = "355025936386877"

CURL = r"""#!/bin/bash
url="${@: -1}"
case "$url" in
    *"/fw/manifest.txt?imei="*)
        imei=${url#*imei=}; imei=${imei%%&*}
        [[ -f "$FAKE_FW/manifest-$imei.txt" ]] && cat "$FAKE_FW/manifest-$imei.txt"
        exit 0 ;;
    */fw/l0destar-*) printf '206 2048'; exit 0 ;;
esac
exit 22
"""
SSH = '#!/bin/bash\nbash -c "$2"\n'
SCP = '#!/bin/bash\nshift\ncp "$1" "${2#*:}"\n'


def write_exec(path, text):
    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)


def build_config(version, board_id="v4.0p1"):
    return "\n".join([
        'CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="%s+0"' % version,
        'CONFIG_APP_BOARD_ID="%s"' % board_id,
        "CONFIG_APP_BOARD_HAS_KLINE=y",
        "CONFIG_APP_DEBUG_IGNITION=-1",
        "CONFIG_APP_DEBUG_BATTERY_MV=0",
        "CONFIG_APP_DEBUG_DROP_DATAGRAMS=0",
        "# CONFIG_APP_CAN_TEST is not set",
        "# CONFIG_APP_KLINE_TEST is not set",
        "# CONFIG_APP_ACCEL_TEST is not set",
        "",
    ])


class Rig:
    def __init__(self, root):
        self.root = str(root)
        self.fw = os.path.join(self.root, "server_fw")
        os.makedirs(self.fw)
        bindir = os.path.join(self.root, "bin")
        os.makedirs(bindir)
        write_exec(os.path.join(bindir, "curl"), CURL)
        write_exec(os.path.join(bindir, "ssh"), SSH)
        write_exec(os.path.join(bindir, "scp"), SCP)
        shutil.copy(SCRIPT, os.path.join(self.root, "push_fw.sh"))
        os.makedirs(os.path.join(self.root, "certs"))
        open(os.path.join(self.root, "certs", "ca.crt"), "w").write("ca\n")
        open(os.path.join(self.root, "VERSION"), "w").write("0.4\n")
        open(os.path.join(self.root, "remote.conf"), "w").write(
            '[common]\nCONFIG_APP_SERVER_HOST="fw.example"\n\n'
            "[%s]\nname = car\nprofile = makerdiary\nCONFIG_APP_BOARD_L0DESTAR_V4_0P1=y\n" % IMEI)
        self.env = dict(os.environ, PATH=bindir + os.pathsep + os.environ["PATH"],
                        FW_SERVER="test@host", FW_DIR=self.fw, FAKE_FW=self.fw)

    def built(self, version, board_id="v4.0p1"):
        d = os.path.join(self.root, "build_remote_%s" % IMEI, "firmware", "zephyr")
        os.makedirs(d, exist_ok=True)
        open(os.path.join(d, "zephyr.signed.bin"), "wb").write(b"\0" * 4096)
        open(os.path.join(d, ".config"), "w").write(build_config(version, board_id))

    def live(self, version, board):
        open(os.path.join(self.fw, "manifest-%s.txt" % IMEI), "w").write(
            "version=%s\nfile=fw/l0destar-%s-%s.bin\nboard=%s\n" % (version, version, IMEI, board))

    def manifest(self):
        text = open(os.path.join(self.fw, "manifest-%s.txt" % IMEI)).read()
        return dict(line.split("=", 1) for line in text.splitlines())

    def push(self, *args):
        return subprocess.run(["bash", "push_fw.sh", "--no-build", *args], cwd=self.root,
                              env=self.env, capture_output=True, text=True)


@pytest.fixture
def rig(tmp_path):
    return Rig(tmp_path)


def test_a_release_is_published_under_the_builds_own_board(rig):
    rig.built("0.4.96")
    r = rig.push("--device", IMEI)
    assert r.returncode == 0, r.stderr
    assert rig.manifest() == {"version": "0.4.96", "file": "fw/l0destar-0.4.96-%s.bin" % IMEI,
                              "board": "v4.0p1+kline"}
    assert "warning" not in r.stderr


def test_the_bridge_release_carries_the_id_the_unit_knows(rig):
    rig.built("0.4.96")
    rig.live("0.4.95", "v4.0+kline")
    r = rig.push("--device", IMEI, "--as-board", "v4.0+kline")
    assert r.returncode == 0, r.stderr
    assert rig.manifest()["board"] == "v4.0+kline"
    assert rig.manifest()["version"] == "0.4.96"
    assert "bridge" in r.stdout
    # the image itself is the build's: only the manifest was relabelled
    assert os.path.getsize(os.path.join(rig.fw, "l0destar-0.4.96-%s.bin" % IMEI)) == 4096


def test_the_release_after_a_bridge_goes_out_under_the_builds_own_id(rig):
    rig.built("0.4.96")
    rig.live("0.4.95", "v4.0+kline")
    assert rig.push("--device", IMEI, "--as-board", "v4.0+kline").returncode == 0
    rig.built("0.4.97")
    r = rig.push("--device", IMEI)
    assert r.returncode == 0, r.stderr
    assert rig.manifest()["board"] == "v4.0p1+kline"
    assert rig.manifest()["version"] == "0.4.97"


def test_an_ordinary_release_to_a_unit_on_another_id_says_so(rig):
    rig.built("0.4.96")
    rig.live("0.4.95", "v4.0+kline")
    r = rig.push("--device", IMEI)
    assert r.returncode == 0, r.stderr
    assert rig.manifest()["board"] == "v4.0p1+kline"
    assert "board=v4.0+kline" in r.stderr and "--as-board v4.0+kline" in r.stderr


def test_the_same_board_gives_no_warning(rig):
    rig.built("0.4.96")
    rig.live("0.4.95", "v4.0p1+kline")
    r = rig.push("--device", IMEI)
    assert r.returncode == 0, r.stderr
    assert "warning" not in r.stderr


def test_the_bridge_is_for_one_unit(rig):
    rig.built("0.4.96")
    r = rig.push("--as-board", "v4.0+kline")
    assert r.returncode != 0
    assert "--device" in r.stderr
    assert not os.path.exists(os.path.join(rig.fw, "manifest-%s.txt" % IMEI))


def test_the_bridge_to_the_builds_own_id_is_refused(rig):
    rig.built("0.4.96")
    r = rig.push("--device", IMEI, "--as-board", "v4.0p1+kline")
    assert r.returncode != 0
    assert "own board id" in r.stderr


def test_a_malformed_id_is_refused(rig):
    rig.built("0.4.96")
    for bad in ("two words", "x" * 32, ""):
        r = rig.push("--device", IMEI, "--as-board", bad)
        assert r.returncode != 0, bad
        assert not os.path.exists(os.path.join(rig.fw, "manifest-%s.txt" % IMEI))


def test_the_bridge_must_name_the_id_the_unit_was_last_offered(rig):
    rig.built("0.4.96")
    rig.live("0.4.95", "v3.4+kline")
    r = rig.push("--device", IMEI, "--as-board", "v4.0+kline")
    assert r.returncode != 0
    assert "v3.4+kline" in r.stderr
    assert rig.manifest()["version"] == "0.4.95"
    r = rig.push("--device", IMEI, "--as-board", "v4.0+kline", "--force")
    assert r.returncode == 0, r.stderr
    assert rig.manifest()["board"] == "v4.0+kline"


def test_a_unit_with_no_manifest_yet_can_be_bridged(rig):
    rig.built("0.4.96")
    r = rig.push("--device", IMEI, "--as-board", "v4.0+kline")
    assert r.returncode == 0, r.stderr
    assert rig.manifest()["board"] == "v4.0+kline"
