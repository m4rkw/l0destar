"""engine_is_running() and its sampler, compiled out of src/data.c."""
import subprocess
from pathlib import Path

SRC = (Path(__file__).parent.parent / "src" / "data.c").read_text()


def _func(sig):
    start = SRC.index(sig + "\n{")
    return SRC[start:SRC.index("\n}\n", start) + 3]


HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define IS_ENABLED(x) 1
#define ENGINE_RUNNING_VOLTAGE 13.0f
#define ENGINE_STOPPED_HOLD_S 300
#define ENGINE_STILL_HOLD_S 120
static int64_t now_ms, s_low_voltage_since = -1, s_still_since = -1;
static int64_t k_uptime_get(void) { return now_ms; }
static bool moving, still, engine_running;
static bool gnss_says_still(void) { return still && !moving; }
#define CONFIG_APP_KLINE_OBD 1
static int rpm = -1;
static int obd_rpm(void) { return rpm; }
static bool gnss_says_moving(void) { return moving; }
static float battery_v, rail;
static float battery_read_voltage(void) { return rail; }
static char ignition;
%s
static bool sample(float v, bool mv, char ign)
{ rail = v; moving = mv; ignition = ign; battery_sample_with_engine_check(); return engine_running; }
int main(void)
{
    /* key off: neither a noisy fix nor the rail starts a verdict */
    assert(!sample(12.0f, true, 1));
    assert(!sample(13.8f, false, 1));
    engine_verdict_reset();
    /* key on at 12 V with a noisy fix: not running */
    now_ms = 60000;
    assert(!sample(12.0f, true, 0));
    /* the alternator starts it; movement holds it through a charge cut */
    assert(sample(13.8f, false, 0));
    for (int i = 0; i < 100; i++) { now_ms += 10000; assert(sample(12.3f, true, 0)); }
    /* standing still ends it after the hold */
    now_ms += 1000;   assert(sample(12.3f, false, 0));
    now_ms += 299000; assert(sample(12.3f, false, 0));
    now_ms += 2000;   assert(!sample(12.3f, false, 0));
    /* seen standing still: ends after 120 s, not before; unknown restarts it */
    assert(sample(13.8f, false, 0)); still = true;
    now_ms += 1000;   assert(sample(12.0f, false, 0));
    now_ms += 119000; assert(sample(12.0f, false, 0));
    still = false; now_ms += 500; assert(sample(12.0f, false, 0)); still = true;
    now_ms += 1000;   assert(sample(12.0f, false, 0));
    now_ms += 119000; assert(sample(12.0f, false, 0));
    now_ms += 1000;   assert(!sample(12.0f, false, 0));
    /* rpm settles it either way, whatever the rail and the speed say */
    rpm = 800;        assert(sample(12.0f, false, 0));
    now_ms += 900000; assert(sample(12.0f, false, 0));
    rpm = 0;          assert(!sample(13.8f, true, 0));
    rpm = -1; still = false;
    /* sleep clears it inside the hold */
    assert(sample(13.8f, false, 0));
    engine_verdict_reset();
    assert(!sample(12.0f, true, 0));
    return 0;
}
"""


def test_verdict(tmp_path):
    body = "\n".join(_func(s) for s in (
        "bool engine_is_running(void)",
        "static float battery_sample_with_engine_check(void)",
        "void engine_verdict_reset(void)"))
    c = tmp_path / "t.c"
    c.write_text(HARNESS.replace("%s", body))
    exe = tmp_path / "t"
    subprocess.run(["cc", "-o", exe, c], check=True)
    subprocess.run([exe], check=True)


def test_sleep_entry_clears_the_verdict():
    main = (SRC_DIR / "main.c").read_text()
    body = main[main.index("static void do_sleep(void)\n{"):]
    assert "engine_verdict_reset();" in body[:300]


SRC_DIR = Path(__file__).parent.parent / "src"
