# Copyright (c) 2026 Rodrigo Peixoto
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_NANOPB_TEXTFORMAT)

  zephyr_library_named(nanopb_textformat)

  zephyr_library_sources(
    src/nanopb_textformat_common.c
    src/nanopb_textformat_parse.c
    src/nanopb_textformat_print.c
  )

  zephyr_include_directories(include)

endif()
