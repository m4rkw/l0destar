# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(l0destar)

# The receiver: the nRF9151's own (gnss.c, and the nRF Cloud A-GNSS that feeds
# it) or, on a board that has one, the external u-blox on its own UART.
if(CONFIG_APP_BOARD_HAS_EXT_GNSS)
    set(GNSS_SOURCES src/gnss_ext.c src/gnss_ubx.c src/gnss_nmea.c)
    if(CONFIG_APP_ASSISTNOW)
        list(APPEND GNSS_SOURCES src/assistnow.c)
    endif()
else()
    set(GNSS_SOURCES src/gnss.c src/agnss.c)
endif()

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
    ${GNSS_SOURCES}
    src/modem.c
    src/data.c
    src/alert.c
    src/commands.c
    src/fota.c
    src/led.c
    src/watchdog.c
    src/reboot.c
    src/fatal.c
)

# Telemetry transport: the l0destar server's encrypted UDP, or a Traccar
# server's OsmAnd HTTP.  Both implement the transport_* interface in app.h.
if(CONFIG_APP_TRACCAR)
    target_sources(app PRIVATE src/traccar.c)
else()
    target_sources(app PRIVATE src/transport.c)
endif()

# Interactive bring-up rig (board_test.sh) — bench builds only.
target_sources_ifdef(CONFIG_APP_BOARD_TEST app PRIVATE src/board_test.c)

# CAN bench-test agent (can_bench/) — bench builds only.
target_sources_ifdef(CONFIG_APP_CAN_BENCH app PRIVATE src/can_bench.c)

# LTE TX power / brown-out rig — bench builds only.
target_sources_ifdef(CONFIG_APP_LTE_POWER_TEST app PRIVATE src/lte_power_test.c)

# Backlog buffer for records that could not be sent.
target_sources(app PRIVATE src/databuf.c)
# Warnings/errors captured for the server (see src/dbglog.c).
target_sources_ifdef(CONFIG_APP_DEBUG_LOG app PRIVATE src/dbglog.c)

# Demo mode's coordinate masker for the console (either receiver).
target_sources_ifdef(CONFIG_APP_DEMO_MODE app PRIVATE src/demo_mask.c)

# The motion through gaps between GNSS fixes (see src/motion.c).
target_sources_ifdef(CONFIG_APP_DEAD_RECKONING app PRIVATE src/motion.c)

# When to take LTE away from a network that holds the radio (see Kconfig).
target_sources_ifdef(CONFIG_APP_HOLD_BREAK app PRIVATE src/holdbreak.c src/crumb.c)

# OBD-II telemetry / fault codes over the K wire.
target_sources_ifdef(CONFIG_APP_KLINE_OBD app PRIVATE src/kline_obd.c)
# Threshold alerts on that telemetry (CONFIG_APP_OBD_ALERTS).
target_sources_ifdef(CONFIG_APP_KLINE_TELEMETRY app PRIVATE src/obd_alert.c)

target_include_directories(app PRIVATE src)
