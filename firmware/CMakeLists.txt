# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(l0destar)

target_sources(app PRIVATE
    src/main.c
    src/crypto.c
    src/chacha20_poly1305.c
    src/settings.c
    src/hw_common.c
    src/hw_power.c
    src/hw_accel.c
    src/hw_relay.c
    src/hw_kline.c
    src/gnss.c
    src/modem.c
    src/transport.c
    src/data.c
    src/alert.c
    src/commands.c
    src/led.c
    src/watchdog.c
    src/reboot.c
)

target_include_directories(app PRIVATE src)
