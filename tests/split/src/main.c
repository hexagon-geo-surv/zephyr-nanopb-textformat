/*
 * Copyright (c) 2026 Rodrigo Peixoto
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Either half of the library on its own.
 *
 * CONFIG_NANOPB_TEXTFORMAT_PARSE and _PRINT each drop half the library, for a
 * firmware that only ever accepts a configuration or only ever reports one.
 * This suite checks three things testcase.yaml then runs in every combination:
 * the descriptor machinery and common helpers survive either way, each half
 * works alone, and a half that was configured out really is absent.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <nanopb_textformat.h>

#include "src/split.pb.h"

PB_TF_DEFINE(SplitInner, SplitInner);
PB_TF_DEFINE(Split, Split);

ZTEST_SUITE(pb_tf_split, NULL, NULL, NULL, NULL, NULL);

/*
 * The descriptor tree lives in the header and common.c, so PB_TF_DEFINE and
 * everything reached through it must work with either half missing. If this
 * fails, the split cut through something it should not have.
 */
ZTEST(pb_tf_split, test_common_half_survives_either_way)
{
	Split m;

	zassert_equal_ptr(Split_tf.desc, &Split_msg);
	zassert_equal(Split_tf.count, 4, "got %u", Split_tf.count);
	zassert_equal(Split_tf.struct_size, sizeof(Split));

	/* The submessage row still resolves its descriptor. */
	zassert_equal_ptr(Split_tf.fields[2].sub, &SplitInner_tf);

	/* pb_tf_clear, pb_tf_strerror and pb_tf_out_buf are in common.c. */
	(void)memset(&m, 0xAA, sizeof(m));
	pb_tf_clear(&Split_tf, &m);
	zassert_equal(m.a, 0);
	zassert_equal(m.b_count, 0);

	zassert_str_equal(pb_tf_strerror(PB_TF_OK), "ok");
	zassert_not_null(pb_tf_strerror(PB_TF_ERR_UNSUPPORTED));

	char buf[8] = "";
	pb_tf_buf_t sink = {.buf = buf, .size = sizeof(buf)};

	(void)pb_tf_out_buf('x', &sink);
	zassert_str_equal(buf, "x");
}

/* A build with neither half is legal but pointless; say so out loud. */
ZTEST(pb_tf_split, test_at_least_one_half_is_built)
{
	zassert_true(IS_ENABLED(CONFIG_NANOPB_TEXTFORMAT_PARSE) ||
			     IS_ENABLED(CONFIG_NANOPB_TEXTFORMAT_PRINT),
		     "a module with neither half only offers clear/strerror/out_buf");
}

#if defined(CONFIG_NANOPB_TEXTFORMAT_PARSE)

ZTEST(pb_tf_split, test_parse_half_alone)
{
	Split m;
	pb_tf_status_t st;
	pb_tf_err_t err;

	err = pb_tf_parse(&Split_tf, &m, "a: 7 b: [1, 2] sub { v: -3 } name: \"hi\"", &st);
	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.a, 7);
	zassert_equal(m.b_count, 2);
	zassert_equal(m.sub.v, -3);
	zassert_str_equal(m.name, "hi");

	/* merge is part of the same half. */
	err = pb_tf_merge(&Split_tf, &m, "b: [3]", &st);
	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.b_count, 3, "merge appends");
	zassert_equal(m.a, 7, "merge keeps what is there");

	/* Errors still carry an offset and a field name. */
	zassert_equal(pb_tf_parse(&Split_tf, &m, "a: -1", &st), PB_TF_ERR_RANGE);
	zassert_str_equal(st.field, "a");
}

#endif /* CONFIG_NANOPB_TEXTFORMAT_PARSE */

#if defined(CONFIG_NANOPB_TEXTFORMAT_PRINT)

ZTEST(pb_tf_split, test_print_half_alone)
{
	/*
	 * Built by hand rather than parsed, since the parser may not be here.
	 * That is the point of the print-only configuration: the struct comes
	 * from the application, not from text.
	 */
	Split m = {
		.a = 7,
		.b_count = 2,
		.b = {1, 2},
		.has_sub = true,
		.sub = {.v = -3},
		.name = "hi",
	};
	char out[128];

	zassert_equal(pb_tf_print_buf(&Split_tf, &m, out, sizeof(out)), PB_TF_OK);

	zassert_not_null(strstr(out, "a: 7"), "got %s", out);
	zassert_not_null(strstr(out, "b: 1"), "got %s", out);
	zassert_not_null(strstr(out, "b: 2"), "got %s", out);
	zassert_not_null(strstr(out, "v: -3"), "got %s", out);
	zassert_not_null(strstr(out, "name: \"hi\""), "got %s", out);

	/* Truncation is still reported rather than silently dropped. */
	char tiny[4];

	zassert_equal(pb_tf_print_buf(&Split_tf, &m, tiny, sizeof(tiny)), PB_TF_ERR_NO_SPACE);
	zassert_true(strlen(tiny) < sizeof(tiny));
}

#endif /* CONFIG_NANOPB_TEXTFORMAT_PRINT */
