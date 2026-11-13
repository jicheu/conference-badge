# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# Out-of-tree devicetree binding for the SSD1677 panel (dts/bindings/).
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(conference_badge)

target_sources(app PRIVATE src/main.c src/ssd1677.c)
target_sources_ifdef(CONFIG_BADGE_REMOTE app PRIVATE src/remote.c)
target_sources_ifdef(CONFIG_BADGE_BLE app PRIVATE src/ble.c)
