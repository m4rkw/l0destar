"""accel_crash_low_power(), compiled out of src/hw_accel.c: the accelerometer
in the key-on, engine-off sleep at each CONFIG_APP_IGN_SLEEP_IMU_HZ.

    uv run --python 3.12 --with pytest pytest -q firmware/tests/test_ign_sleep_imu.py
"""
import re
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).parent.parent
SRC = (ROOT / "src" / "hw_accel.c").read_text()

HARNESS = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define EIO 5
#define ACC_ADDR 0x6A
#define FIFO_SAMPLE_MS 38
static int s_fifo_sample_ms = -1;
static bool s_ok = true, s_fifo_awake = true;
static float s_mg_per_lsb;
static int acc_bus;
static bool bb_write_reg(const int *b, int a, int reg, int v)
{ printf("w %02x %02x\n", reg, v & 0xff); return true; }
static void k_msleep(int ms) { printf("ms %d\n", ms); }
%defines
%func
int main(void)
{
    int r = accel_crash_low_power();
    printf("end %d %d %d %d\n", r, s_fifo_sample_ms, (int)(s_mg_per_lsb * 1000 + 0.5f), s_fifo_awake);
    return 0;
}
"""


def run(tmp_path, hz):
    start = SRC.index("int accel_crash_low_power(void)\n{")
    func = SRC[start:SRC.index("\n}\n", start) + 3]
    defines = "\n".join(re.findall(r"^#define ACC_(?:CTRL|FIFO)\w*\s+0x\w+", SRC, re.M))
    c = tmp_path / "t.c"
    c.write_text(HARNESS.replace("%defines", defines).replace("%func", func))
    exe = tmp_path / "t"
    subprocess.run(["cc", f"-DCONFIG_APP_IGN_SLEEP_IMU_HZ={hz}", "-o", exe, c], check=True)
    return subprocess.run([exe], check=True, capture_output=True, text=True).stdout.split("\n")[:-1]


# hz: CTRL1_XL, settle ms, FIFO_CTRL3, ms per FIFO sample
@pytest.mark.parametrize("hz,ctrl1,settle,bdr,sample", [
    (12, "1c", 400, "01", 80),
    (26, "2c", 200, "02", 38),
    (52, "3c", 100, "02", 38),
])
def test_rate(tmp_path, hz, ctrl1, settle, bdr, sample):
    assert run(tmp_path, hz) == [
        "w 11 00",                      # gyro off
        "w 15 10",                      # low power
        f"w 10 {ctrl1}",                # the rate, +-8 g
        f"ms {settle}",
        "w 0a 00",                      # FIFO emptied,
        f"w 09 {bdr}",                  # accel alone,
        "w 0a 06",                      # ring
        f"end 0 {sample} 244 0",
    ]


def test_default_is_12_5_hz():
    kconfig = (ROOT / "Kconfig").read_text()
    block = kconfig[kconfig.index("config APP_IGN_SLEEP_IMU_HZ"):]
    assert "default 12\n" in block[:200]
