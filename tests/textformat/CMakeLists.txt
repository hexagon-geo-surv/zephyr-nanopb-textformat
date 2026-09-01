# Copyright (c) 2026 Rodrigo Peixoto
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# The library under test lives two levels up; pull it in as a Zephyr module.
list(APPEND EXTRA_ZEPHYR_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/../..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nanopb_textformat_tests)

# zephyr_nanopb_sources() lives in zephyr/modules/nanopb/nanopb.cmake and is
# not auto-included (nanopb's module.yml uses cmake-ext).
list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)

# --c-style is one of the two naming styles nanopb can emit, and the one where
# PB_TF_DEFINE's two arguments differ. fixture3.proto covers the other.
set(NANOPB_OPTIONS "--c-style")
# fixture.proto is proto3; fixture2.proto is proto2, for `required`, an
# `extensions` range and the non-STATIC allocations proto3 cannot express.
zephyr_nanopb_sources(app src/fixture.proto src/fixture2.proto)

# fixture3.proto is generated with nanopb's DEFAULT naming, which is what proves
# PB_TF_DEFINE works for both styles: under --c-style its two arguments differ
# (FX_SHAPES vs fx_shapes_t), here they coincide (FxDef, FxDef). NANOPB_OPTIONS
# is read per call, so clearing it applies only to this one.
set(NANOPB_OPTIONS "")
zephyr_nanopb_sources(app src/fixture3.proto)

target_sources(app PRIVATE src/main.c)

# White-box suite: the macro tests assert on the generated row table through the
# library's internal helpers (pb_tf_field_find, pb_tf_shape_supported), which are
# declared in src/nanopb_textformat_priv.h rather than the public header.
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
