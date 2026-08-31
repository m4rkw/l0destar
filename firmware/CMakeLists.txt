# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(l0destar)

target_sources(app PRIVATE
    src/main.c
    src/crypto.c
    src/settings.c
    src/hw_common.c
    src/hw_domain.c
    src/hw_selftest.c
    src/hw_power.c
    src/hw_can.c
    src/hw_accel.c
    src/hw_kline.c
    src/gnss.c
    src/agnss.c
    src/modem.c
    src/transport.c
    src/data.c
    src/alert.c
    src/commands.c
    src/fota.c
    src/led.c
    src/watchdog.c
    src/reboot.c
)

# Interactive bring-up rig (board_test.sh) — bench builds only.
target_sources_ifdef(CONFIG_APP_BOARD_TEST app PRIVATE src/board_test.c)

# LTE TX power / brown-out rig — bench builds only.
target_sources_ifdef(CONFIG_APP_LTE_POWER_TEST app PRIVATE src/lte_power_test.c)

target_include_directories(app PRIVATE src)
