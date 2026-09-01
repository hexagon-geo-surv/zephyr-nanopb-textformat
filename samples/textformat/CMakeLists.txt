# Copyright (c) 2026 Rodrigo Peixoto
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# The library lives two levels up; pull it in as a Zephyr module. A real
# application would instead name this repository in its west manifest.
list(APPEND EXTRA_ZEPHYR_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/../..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nanopb_textformat_sample)

# zephyr_nanopb_sources() lives in zephyr/modules/nanopb/nanopb.cmake and is not
# auto-included, because nanopb's module.yml uses cmake-ext.
list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)

# No --c-style here, so nanopb's default naming applies and PB_TF_DEFINE's two
# arguments coincide. Both styles work; see src/main.c.
zephyr_nanopb_sources(app src/config.proto)

target_sources(app PRIVATE src/main.c)
