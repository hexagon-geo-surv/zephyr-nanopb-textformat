# Copyright (c) 2026 Rodrigo Peixoto
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_NANOPB_TEXTFORMAT)

  zephyr_library_named(nanopb_textformat)

  # common.c carries the descriptor lookup, pb_tf_clear, pb_tf_strerror and
  # pb_tf_out_buf, which either direction needs. The two halves are independent:
  # neither calls the other, so either can be left out.
  zephyr_library_sources(src/nanopb_textformat_common.c)

  zephyr_library_sources_ifdef(CONFIG_NANOPB_TEXTFORMAT_PARSE
    src/nanopb_textformat_parse.c
  )

  zephyr_library_sources_ifdef(CONFIG_NANOPB_TEXTFORMAT_PRINT
    src/nanopb_textformat_print.c
  )

  zephyr_include_directories(include)

endif()
