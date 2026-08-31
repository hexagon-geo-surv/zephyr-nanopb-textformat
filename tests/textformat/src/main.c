/*
 * Copyright (c) 2026 Rodrigo Peixoto
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>

#include <nanopb_textformat.h>
#include <pb_common.h>

/* White-box: the row table is asserted through the library's internal helpers. */
#include "nanopb_textformat_priv.h"

/* zephyr_nanopb_sources() adds only ${CMAKE_CURRENT_BINARY_DIR} to the include
 * path (zephyr/modules/nanopb/nanopb.cmake:35) while protoc mirrors the proto's
 * source-relative path into the output tree, so the generated header is at
 * <build>/src/fixture.pb.h. Same include as zephlet/tests/shell_macros. */
#include "src/fixture.pb.h"

/*
 * One PB_TF_DEFINE per message. Order is deliberately "wrong" -- fx_shapes
 * references fx_inner and is defined first -- to prove PB_TF_GEN_EXTERN's
 * forward declarations make row order irrelevant.
 */
PB_TF_DEFINE(FX_SHAPES, fx_shapes_t);
PB_TF_DEFINE(FX_INNER, fx_inner_t);
PB_TF_DEFINE(FX_SCALARS, fx_scalars_t);
PB_TF_DEFINE(FX_EMPTY, fx_empty_t);
PB_TF_DEFINE(FX_DEEP3, fx_deep3_t);
PB_TF_DEFINE(FX_DEEP2, fx_deep2_t);
PB_TF_DEFINE(FX_DEEP1, fx_deep1_t);
PB_TF_DEFINE(FX_ANON, fx_anon_t);

/* The tables must agree with nanopb's own field counts. */
BUILD_ASSERT(PB_TF_FIELD_COUNT(FX_SCALARS_FIELDLIST) == 20);
BUILD_ASSERT(PB_TF_FIELD_COUNT(FX_SHAPES_FIELDLIST) == 11);
BUILD_ASSERT(PB_TF_FIELD_COUNT(FX_INNER_FIELDLIST) == 2);
BUILD_ASSERT(PB_TF_FIELD_COUNT(FX_EMPTY_FIELDLIST) == 0);
BUILD_ASSERT(PB_TF_FIELD_COUNT(FX_ANON_FIELDLIST) == 3);

ZTEST_SUITE(pb_tf_macro, NULL, NULL, NULL, NULL, NULL);

/* The descriptor must describe the same message nanopb does. */
ZTEST(pb_tf_macro, test_descriptor_matches_nanopb)
{
	zassert_equal_ptr(fx_scalars_t_tf.desc, &fx_scalars_t_msg);
	zassert_equal(fx_scalars_t_tf.count, 20, "got %u", fx_scalars_t_tf.count);
	zassert_equal(fx_scalars_t_tf.struct_size, sizeof(fx_scalars_t));

	zassert_equal(fx_shapes_t_tf.count, 11, "got %u", fx_shapes_t_tf.count);
	zassert_equal(fx_shapes_t_tf.struct_size, sizeof(fx_shapes_t));

	/* An empty message: nothing but the sentinel row. */
	zassert_equal(fx_empty_t_tf.count, 0, "got %u", fx_empty_t_tf.count);
	zassert_not_null(fx_empty_t_tf.fields);
	zassert_is_null(fx_empty_t_tf.fields[0].name);
}

/* Names come from #fieldname, and lookup is by exact match. */
ZTEST(pb_tf_macro, test_field_names_and_tags)
{
	const pb_tf_field_t *f = &fx_scalars_t_tf.fields[0];

	zassert_str_equal(f->name, "f_int32");
	zassert_equal(f->tag, FX_SCALARS_F_INT32_TAG);

	/* Rows are in tag order, matching desc->field_info. */
	for (uint16_t i = 0U; i < fx_scalars_t_tf.count; i++) {
		zassert_equal(fx_scalars_t_tf.fields[i].tag, i + 1U, "row %u tag %u", i,
			      fx_scalars_t_tf.fields[i].tag);
	}
}

/*
 * The whole reason the row carries a vtype: nanopb's runtime pb_type_t
 * collapses float/fixed32/sfixed32 into PB_LTYPE_FIXED32, so a printer
 * driven by it alone could not tell whether to emit 1.0 or 1.
 */
ZTEST(pb_tf_macro, test_vtype_preserves_what_nanopb_loses)
{
	const pb_tf_field_t *fl = pb_tf_field_find(&fx_scalars_t_tf, "f_float", 7);
	const pb_tf_field_t *fx = pb_tf_field_find(&fx_scalars_t_tf, "f_fixed32", 9);
	const pb_tf_field_t *sf = pb_tf_field_find(&fx_scalars_t_tf, "f_sfixed32", 10);

	zassert_not_null(fl);
	zassert_not_null(fx);
	zassert_not_null(sf);

	zassert_equal(fl->vtype, PB_TF_VT_FLOAT);
	zassert_equal(fx->vtype, PB_TF_VT_FIXED32);
	zassert_equal(sf->vtype, PB_TF_VT_SFIXED32);

	/* Signed and unsigned enums stay distinguishable too. */
	zassert_equal(pb_tf_field_find(&fx_scalars_t_tf, "f_uenum", 7)->vtype, PB_TF_VT_UENUM);
	zassert_equal(pb_tf_field_find(&fx_scalars_t_tf, "f_enum", 6)->vtype, PB_TF_VT_ENUM);
}

/*
 * The other reason: PB_HTYPE_OPTIONAL == PB_HTYPE_SINGULAR and
 * PB_HTYPE_REPEATED == PB_HTYPE_FIXARRAY in nanopb, so the distinction only
 * survives in the FIELDLIST token.
 */
ZTEST(pb_tf_macro, test_rules_preserve_htype)
{
	const pb_tf_msg_t *tf = &fx_shapes_t_tf;

	zassert_equal(PB_TF_HT(pb_tf_field_find(tf, "plain_scalar", 12)->rules),
		      PB_TF_HT_SINGULAR);
	zassert_equal(PB_TF_HT(pb_tf_field_find(tf, "opt_scalar", 10)->rules), PB_TF_HT_OPTIONAL);
	zassert_equal(PB_TF_HT(pb_tf_field_find(tf, "rep_scalar", 10)->rules), PB_TF_HT_REPEATED);
	zassert_equal(PB_TF_HT(pb_tf_field_find(tf, "fixed_scalar", 12)->rules),
		      PB_TF_HT_FIXARRAY);
	zassert_equal(PB_TF_HT(pb_tf_field_find(tf, "c_num", 5)->rules), PB_TF_HT_ONEOF);

	/* All STATIC in this fixture. */
	zassert_equal(PB_TF_AT(pb_tf_field_find(tf, "plain_scalar", 12)->rules), PB_TF_AT_STATIC);
}

/*
 * A proto3 singular message field is emitted as OPTIONAL with a has_ bit, so
 * `fx_inner sing_msg` and `optional fx_inner opt_msg` are the same shape --
 * exactly the claim in zephlet#58.
 */
ZTEST(pb_tf_macro, test_singular_and_optional_submessage_are_identical)
{
	const pb_tf_field_t *s = pb_tf_field_find(&fx_shapes_t_tf, "sing_msg", 8);
	const pb_tf_field_t *o = pb_tf_field_find(&fx_shapes_t_tf, "opt_msg", 7);

	zassert_equal(s->vtype, PB_TF_VT_MESSAGE);
	zassert_equal(o->vtype, PB_TF_VT_MESSAGE);
	zassert_equal(s->rules, o->rules, "0x%02x vs 0x%02x", s->rules, o->rules);
}

/* The recursive tree: a message row points at the submessage's descriptor. */
ZTEST(pb_tf_macro, test_submessage_descriptors_are_linked)
{
	const pb_tf_field_t *sing = pb_tf_field_find(&fx_shapes_t_tf, "sing_msg", 8);
	const pb_tf_field_t *rep = pb_tf_field_find(&fx_shapes_t_tf, "rep_msg", 7);
	const pb_tf_field_t *cmsg = pb_tf_field_find(&fx_shapes_t_tf, "c_msg", 5);

	/* Defined before fx_inner_t_tf in this file, so this also proves the
	 * PB_TF_GEN_EXTERN forward declarations work.
	 */
	zassert_equal_ptr(sing->sub, &fx_inner_t_tf);
	zassert_equal_ptr(rep->sub, &fx_inner_t_tf);
	zassert_equal_ptr(cmsg->sub, &fx_inner_t_tf, "oneof submessage row not linked");

	/* Scalars must not claim a submessage. */
	zassert_is_null(pb_tf_field_find(&fx_shapes_t_tf, "plain_scalar", 12)->sub);

	/* Three levels deep, each link resolved independently. */
	zassert_equal_ptr(pb_tf_field_find(&fx_deep1_t_tf, "d2", 2)->sub, &fx_deep2_t_tf);
	zassert_equal_ptr(pb_tf_field_find(&fx_deep2_t_tf, "d3", 2)->sub, &fx_deep3_t_tf);
	zassert_is_null(pb_tf_field_find(&fx_deep3_t_tf, "v", 1)->sub);
}

/* Oneof rows must be named for the member, not the parenthesised tuple. */
ZTEST(pb_tf_macro, test_oneof_member_names)
{
	zassert_not_null(pb_tf_field_find(&fx_shapes_t_tf, "c_num", 5));
	zassert_not_null(pb_tf_field_find(&fx_shapes_t_tf, "c_str", 5));
	zassert_not_null(pb_tf_field_find(&fx_shapes_t_tf, "c_msg", 5));

	/* The union itself is not a field. */
	zassert_is_null(pb_tf_field_find(&fx_shapes_t_tf, "choice", 6));
}

ZTEST(pb_tf_macro, test_field_find_rejects_non_fields)
{
	zassert_is_null(pb_tf_field_find(&fx_scalars_t_tf, "nope", 4));
	/* Prefixes and extensions of a real name must not match. */
	zassert_is_null(pb_tf_field_find(&fx_scalars_t_tf, "f_int", 5));
	zassert_is_null(pb_tf_field_find(&fx_scalars_t_tf, "f_int32x", 8));
	/* An empty message has nothing to find. */
	zassert_is_null(pb_tf_field_find(&fx_empty_t_tf, "anything", 8));
}

/* Only STATIC, non-FIXARRAY, non-EXTENSION shapes are writable. */
ZTEST(pb_tf_macro, test_shape_support)
{
	zassert_true(pb_tf_shape_supported(pb_tf_field_find(&fx_shapes_t_tf, "rep_msg", 7)));
	zassert_true(pb_tf_shape_supported(pb_tf_field_find(&fx_shapes_t_tf, "c_str", 5)));
	zassert_false(pb_tf_shape_supported(pb_tf_field_find(&fx_shapes_t_tf, "fixed_scalar", 12)),
		      "fixed_count must be rejected");
}

/*
 * Widths are never assumable: int_size:IS_8 gives a 1-byte field, and
 * PB_WITHOUT_64BIT narrows the 64-bit ones. Read them from the iterator the
 * same way the value writer will.
 */
ZTEST(pb_tf_macro, test_data_size_is_not_assumable)
{
	fx_scalars_t msg = FX_SCALARS_INIT_ZERO;
	pb_field_iter_t it;

	zassert_true(pb_field_iter_begin(&it, &fx_scalars_t_msg, &msg));

	zassert_true(pb_field_iter_find(&it, FX_SCALARS_F_NARROW_TAG));
	zassert_equal(it.data_size, 1, "int_size:IS_8 should be 1 byte, got %u", it.data_size);
	zassert_equal(sizeof(msg.f_narrow), 1);

	/* int_size:IS_32 on a proto int64 is what actually narrows storage, and it
	 * does so in every build config. */
	zassert_true(pb_field_iter_find(&it, FX_SCALARS_F_NARROW32_TAG));
	zassert_equal(it.data_size, 4, "int_size:IS_32 should be 4 bytes, got %u", it.data_size);
	zassert_equal(sizeof(msg.f_narrow32), 4);

	/* PB_WITHOUT_64BIT does NOT narrow storage: the generator never sees it
	 * (it is a codec-side define only), so the struct member stays 8 bytes and
	 * pb_decode.c:452-453 instead fails such a field at runtime with
	 * "invalid data_size". So this holds regardless of CONFIG_NANOPB_WITHOUT_64BIT. */
	zassert_true(pb_field_iter_find(&it, FX_SCALARS_F_INT64_TAG));
	zassert_equal(it.data_size, sizeof(msg.f_int64));
	zassert_equal(it.data_size, 8, "int64 storage is %u bytes", it.data_size);
}

/* pSize is NULL for SINGULAR and non-NULL for OPTIONAL/REPEATED. */
ZTEST(pb_tf_macro, test_psize_presence)
{
	fx_shapes_t msg = FX_SHAPES_INIT_ZERO;
	pb_field_iter_t it;

	zassert_true(pb_field_iter_begin(&it, &fx_shapes_t_msg, &msg));

	zassert_true(pb_field_iter_find(&it, FX_SHAPES_PLAIN_SCALAR_TAG));
	zassert_is_null(it.pSize, "SINGULAR must have no has_ member");

	zassert_true(pb_field_iter_find(&it, FX_SHAPES_OPT_SCALAR_TAG));
	zassert_equal_ptr(it.pSize, &msg.has_opt_scalar);

	zassert_true(pb_field_iter_find(&it, FX_SHAPES_REP_SCALAR_TAG));
	zassert_equal_ptr(it.pSize, &msg.rep_scalar_count);
	zassert_equal(it.array_size, 4);
	zassert_equal(it.data_size, sizeof(msg.rep_scalar[0]));
}

/* pb_field_iter_begin() reports false for a message with no fields. */
ZTEST(pb_tf_macro, test_empty_message_iterator)
{
	fx_empty_t msg = FX_EMPTY_INIT_ZERO;
	pb_field_iter_t it;

	zassert_false(pb_field_iter_begin(&it, &fx_empty_t_msg, &msg),
		      "empty message must report no fields");
}

ZTEST(pb_tf_macro, test_clear_zeroes_whole_struct)
{
	fx_shapes_t msg;

	(void)memset(&msg, 0xAA, sizeof(msg));
	pb_tf_clear(&fx_shapes_t_tf, &msg);

	for (size_t i = 0U; i < sizeof(msg); i++) {
		zassert_equal(((const uint8_t *)&msg)[i], 0U, "byte %zu not cleared", i);
	}
}

ZTEST(pb_tf_macro, test_out_buf_terminates_and_flags_overflow)
{
	char small[4];
	pb_tf_buf_t b = {.buf = small, .size = sizeof(small)};

	for (int i = 0; i < 3; i++) {
		(void)pb_tf_out_buf('x', &b);
	}
	zassert_str_equal(small, "xxx");
	zassert_false(b.overflow);
	zassert_equal(b.used, 3);

	(void)pb_tf_out_buf('y', &b);
	zassert_true(b.overflow, "writing past the buffer must set overflow");
	zassert_str_equal(small, "xxx", "overflow must not corrupt what fits");
}

ZTEST(pb_tf_macro, test_strerror_covers_every_code)
{
	for (int c = PB_TF_OK; c <= PB_TF_ERR_INTERNAL; c++) {
		const char *s = pb_tf_strerror((pb_tf_err_t)c);

		zassert_not_null(s);
		zassert_true(strlen(s) > 0U);
		zassert_true(strcmp(s, "unknown error") != 0, "code %d has no message", c);
	}
}

/*
 * ===========================================================================
 * Milestone 2: lexer and scalar parsing.
 *
 * Everything here is field shapes and integer/bool values. Submessages
 * (milestone 3), strings and bytes (milestone 4) and floats (milestone 6) get
 * their own suites; where their current answer is already final -- a float
 * field under CONFIG_NANOPB_TEXTFORMAT_FLOAT=n -- it is asserted below.
 * ===========================================================================
 */

ZTEST_SUITE(pb_tf_parse, NULL, NULL, NULL, NULL, NULL);

/** Offset of @p needle within @p hay, for error-position assertions. */
static uint16_t off_of(const char *hay, const char *needle)
{
	const char *at = strstr(hay, needle);

	zassert_not_null(at, "'%s' not in '%s'", needle, hay);

	return (uint16_t)(at - hay);
}

ZTEST(pb_tf_parse, test_singular_scalar)
{
	fx_shapes_t m;
	pb_tf_status_t st;

	pb_tf_err_t err = pb_tf_parse(&fx_shapes_t_tf, &m, "plain_scalar: 42", 0U, &st);

	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.plain_scalar, 42);
}

/* An implicit-presence scalar has no has_ bit; an optional one does. */
ZTEST(pb_tf_parse, test_optional_sets_presence)
{
	fx_shapes_t m;

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "plain_scalar: 1", 0U, NULL), PB_TF_OK);
	zassert_false(m.has_opt_scalar, "opt_scalar was not mentioned");

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "opt_scalar: 0", 0U, NULL), PB_TF_OK);
	zassert_true(m.has_opt_scalar, "presence must be set even for a zero value");
	zassert_equal(m.opt_scalar, 0);
}

ZTEST(pb_tf_parse, test_repeated_by_repetition_appends)
{
	fx_shapes_t m;

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "rep_scalar: 1 rep_scalar: 2 rep_scalar: 3",
				  0U, NULL),
		      PB_TF_OK);
	zassert_equal(m.rep_scalar_count, 3, "got %u", m.rep_scalar_count);
	zassert_equal(m.rep_scalar[0], 1);
	zassert_equal(m.rep_scalar[1], 2);
	zassert_equal(m.rep_scalar[2], 3);
}

ZTEST(pb_tf_parse, test_repeated_short_list)
{
	fx_shapes_t m;

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "rep_scalar: [7, 8]", 0U, NULL), PB_TF_OK);
	zassert_equal(m.rep_scalar_count, 2, "got %u", m.rep_scalar_count);
	zassert_equal(m.rep_scalar[0], 7);
	zassert_equal(m.rep_scalar[1], 8);
}

/* "x: []" is legal text format and appends nothing. */
ZTEST(pb_tf_parse, test_empty_list_is_a_noop)
{
	fx_shapes_t m;

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "rep_scalar: [] plain_scalar: 5", 0U, NULL),
		      PB_TF_OK);
	zassert_equal(m.rep_scalar_count, 0, "got %u", m.rep_scalar_count);
	zassert_equal(m.plain_scalar, 5);
}

/* A list and repetition both append, so they compose. */
ZTEST(pb_tf_parse, test_list_then_repetition_accumulates)
{
	fx_shapes_t m;

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "rep_scalar: [1,2] rep_scalar: 3", 0U, NULL),
		      PB_TF_OK);
	zassert_equal(m.rep_scalar_count, 3, "got %u", m.rep_scalar_count);
	zassert_equal(m.rep_scalar[2], 3);
}

ZTEST(pb_tf_parse, test_separators_are_optional)
{
	static const char *const forms[] = {
		"plain_scalar: 1 opt_scalar: 2",
		"plain_scalar: 1, opt_scalar: 2",
		"plain_scalar: 1; opt_scalar: 2",
		"plain_scalar:1,opt_scalar:2",
		"  plain_scalar : 1 ,\n opt_scalar : 2  ",
	};

	ARRAY_FOR_EACH(forms, i) {
		fx_shapes_t m;
		pb_tf_status_t st;

		pb_tf_err_t err = pb_tf_parse(&fx_shapes_t_tf, &m, forms[i], 0U, &st);

		zassert_equal(err, PB_TF_OK, "form %zu (%s): %s", i, forms[i],
			      pb_tf_strerror(st.code));
		zassert_equal(m.plain_scalar, 1, "form %zu", i);
		zassert_equal(m.opt_scalar, 2, "form %zu", i);
	}
}

/* Outer braces are the one documented extension, and they are optional. */
ZTEST(pb_tf_parse, test_outer_braces_optional)
{
	fx_shapes_t m;

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "{plain_scalar: 9}", 0U, NULL), PB_TF_OK);
	zassert_equal(m.plain_scalar, 9);

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "  { plain_scalar: 8 }  ", 0U, NULL),
		      PB_TF_OK);
	zassert_equal(m.plain_scalar, 8);

	/* An empty body, braced or not. */
	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "{}", 0U, NULL), PB_TF_OK);
	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "", 0U, NULL), PB_TF_OK);
	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "   ", 0U, NULL), PB_TF_OK);
}

ZTEST(pb_tf_parse, test_integer_bases)
{
	static const struct {
		const char *text;
		int32_t want;
	} cases[] = {
		{"f_int32: 0", 0},
		{"f_int32: 42", 42},
		{"f_int32: -42", -42},
		{"f_int32: 0x1f", 31},
		{"f_int32: 0X1F", 31},
		{"f_int32: -0x10", -16},
		{"f_int32: 0755", 493},
		{"f_int32: -0755", -493},
		{"f_int32: 2147483647", INT32_MAX},
		{"f_int32: -2147483648", INT32_MIN},
	};

	ARRAY_FOR_EACH(cases, i) {
		fx_scalars_t m;
		pb_tf_status_t st;

		pb_tf_err_t err = pb_tf_parse(&fx_scalars_t_tf, &m, cases[i].text, 0U, &st);

		zassert_equal(err, PB_TF_OK, "'%s': %s", cases[i].text, pb_tf_strerror(st.code));
		zassert_equal(m.f_int32, cases[i].want, "'%s' gave %d", cases[i].text,
			      (int)m.f_int32);
	}
}

ZTEST(pb_tf_parse, test_bool_spellings)
{
	static const struct {
		const char *text;
		bool want;
	} cases[] = {
		{"f_bool: true", true},   {"f_bool: True", true},   {"f_bool: t", true},
		{"f_bool: 1", true},      {"f_bool: false", false}, {"f_bool: False", false},
		{"f_bool: f", false},     {"f_bool: 0", false},
	};

	ARRAY_FOR_EACH(cases, i) {
		fx_scalars_t m;
		pb_tf_status_t st;

		pb_tf_err_t err = pb_tf_parse(&fx_scalars_t_tf, &m, cases[i].text, 0U, &st);

		zassert_equal(err, PB_TF_OK, "'%s': %s", cases[i].text, pb_tf_strerror(st.code));
		zassert_equal(m.f_bool, cases[i].want, "'%s'", cases[i].text);
	}
}

/* Enums are numeric in both directions; the signed one accepts negatives. */
ZTEST(pb_tf_parse, test_enums_are_numeric)
{
	fx_scalars_t m;
	pb_tf_status_t st;

	pb_tf_err_t err = pb_tf_parse(&fx_scalars_t_tf, &m, "f_uenum: 2 f_enum: -1", 0U, &st);

	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	/*
	 * Note the doubled prefix: --c-style prepends the enum's own name to each
	 * value, so the idiomatic FX_MODE_FAST is emitted as FX_MODE_FX_MODE_FAST
	 * (nanopb_generator.py:499). One more token shape the preprocessor cannot
	 * derive from a FIELDLIST row, and one more reason enum-by-name is out of
	 * scope.
	 */
	zassert_equal(m.f_uenum, FX_MODE_FX_MODE_FAST);
	zassert_equal(m.f_enum, FX_TRIM_FX_TRIM_DOWN);

	/* By name is out of scope, and must not silently succeed. */
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_uenum: FX_MODE_FAST", 0U, &st),
		      PB_TF_ERR_SYNTAX);
}

/*
 * The whole point of dispatching on iter.data_size: an int_size-narrowed field
 * is 1 or 4 bytes, and a value that does not fit must be refused rather than
 * silently truncated.
 */
ZTEST(pb_tf_parse, test_narrowed_widths_range_check)
{
	fx_scalars_t m;
	pb_tf_status_t st;

	/* int_size:IS_8 -> int8_t. */
	zassert_equal(sizeof(m.f_narrow), 1);
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_narrow: 127", 0U, NULL), PB_TF_OK);
	zassert_equal(m.f_narrow, 127);
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_narrow: -128", 0U, NULL), PB_TF_OK);
	zassert_equal(m.f_narrow, -128);

	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_narrow: 128", 0U, &st),
		      PB_TF_ERR_RANGE, "128 must not truncate into an int8_t");
	zassert_str_equal(st.field, "f_narrow");
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_narrow: -129", 0U, NULL),
		      PB_TF_ERR_RANGE);

	/* int_size:IS_32 on a proto int64 -> int32_t. */
	zassert_equal(sizeof(m.f_narrow32), 4);
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_narrow32: 2147483647", 0U, NULL),
		      PB_TF_OK);
	zassert_equal(m.f_narrow32, INT32_MAX);
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_narrow32: 2147483648", 0U, NULL),
		      PB_TF_ERR_RANGE);
}

ZTEST(pb_tf_parse, test_unsigned_rejects_negative)
{
	fx_scalars_t m;
	pb_tf_status_t st;

	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_uint32: -1", 0U, &st), PB_TF_ERR_RANGE,
		      "a negative literal must not wrap into an unsigned field");
	zassert_str_equal(st.field, "f_uint32");

	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_uint32: 4294967295", 0U, NULL),
		      PB_TF_OK);
	zassert_equal(m.f_uint32, UINT32_MAX);
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_uint32: 4294967296", 0U, NULL),
		      PB_TF_ERR_RANGE);
}

/* sint and fixed widths take the same path as the plain varints. */
ZTEST(pb_tf_parse, test_sint_and_fixed_widths)
{
	fx_scalars_t m;
	pb_tf_status_t st;

	pb_tf_err_t err = pb_tf_parse(
		&fx_scalars_t_tf, &m, "f_sint32: -5 f_fixed32: 4294967295 f_sfixed32: -2147483648",
		0U, &st);

	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.f_sint32, -5);
	zassert_equal(m.f_fixed32, UINT32_MAX);
	zassert_equal(m.f_sfixed32, INT32_MIN);
}

/*
 * A genuine 8-byte field. PB_WITHOUT_64BIT narrows only the codec's
 * accumulator, not the struct member, so such a field must report a clean
 * error there rather than corrupt half of it.
 */
ZTEST(pb_tf_parse, test_64bit_fields_follow_the_build)
{
	fx_scalars_t m;
	pb_tf_status_t st;
	pb_tf_err_t err = pb_tf_parse(&fx_scalars_t_tf, &m, "f_int64: 1", 0U, &st);

	zassert_equal(sizeof(m.f_int64), 8, "the generator never narrows this");

	if (IS_ENABLED(CONFIG_NANOPB_WITHOUT_64BIT)) {
		zassert_equal(err, PB_TF_ERR_UNSUPPORTED, "got %s", pb_tf_strerror(err));
		zassert_str_equal(st.field, "f_int64");
	} else {
		zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
		zassert_equal(m.f_int64, 1);

		zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m,
					  "f_int64: 9223372036854775807", 0U, NULL),
			      PB_TF_OK);
		zassert_equal(m.f_int64, INT64_MAX);
		zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m,
					  "f_int64: -9223372036854775808", 0U, NULL),
			      PB_TF_OK);
		zassert_equal(m.f_int64, INT64_MIN);
		zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m,
					  "f_int64: 9223372036854775808", 0U, NULL),
			      PB_TF_ERR_RANGE);
	}
}

ZTEST(pb_tf_parse, test_oneof_selects_member)
{
	fx_shapes_t m;
	pb_tf_status_t st;

	pb_tf_err_t err = pb_tf_parse(&fx_shapes_t_tf, &m, "c_num: 77", 0U, &st);

	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.which_choice, FX_SHAPES_C_NUM_TAG);
	zassert_equal(m.choice.c_num, 77);

	/* Re-stating the same member overwrites and keeps the selection. */
	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "c_num: 1 c_num: 2", 0U, NULL), PB_TF_OK);
	zassert_equal(m.which_choice, FX_SHAPES_C_NUM_TAG);
	zassert_equal(m.choice.c_num, 2);
}

/* Parsing clears first unless PB_TF_NOINIT says otherwise. */
ZTEST(pb_tf_parse, test_clear_and_noinit)
{
	fx_shapes_t m;

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "plain_scalar: 1 rep_scalar: [1,2]", 0U,
				  NULL),
		      PB_TF_OK);
	zassert_equal(m.rep_scalar_count, 2);

	/* Default: the previous contents are gone. */
	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "opt_scalar: 3", 0U, NULL), PB_TF_OK);
	zassert_equal(m.plain_scalar, 0, "parse must clear by default");
	zassert_equal(m.rep_scalar_count, 0, "parse must clear by default");

	/* NOINIT: merge into what is there, and repeated fields keep appending. */
	m.plain_scalar = 5;
	m.rep_scalar_count = 1;
	m.rep_scalar[0] = 9;
	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "rep_scalar: 10", PB_TF_NOINIT, NULL),
		      PB_TF_OK);
	zassert_equal(m.plain_scalar, 5, "NOINIT must preserve untouched fields");
	zassert_equal(m.rep_scalar_count, 2);
	zassert_equal(m.rep_scalar[1], 10);
}

/* A float field under CONFIG_NANOPB_TEXTFORMAT_FLOAT=n: final behaviour. */
ZTEST(pb_tf_parse, test_float_field_without_float_support)
{
	fx_scalars_t m;
	pb_tf_status_t st;
	pb_tf_err_t err = pb_tf_parse(&fx_scalars_t_tf, &m, "f_float: 1.5", 0U, &st);

	if (IS_ENABLED(CONFIG_NANOPB_TEXTFORMAT_FLOAT)) {
		/* TODO(milestone 6): this becomes PB_TF_OK. */
		zassert_equal(err, PB_TF_ERR_UNSUPPORTED, "got %s", pb_tf_strerror(err));
	} else {
		zassert_equal(err, PB_TF_ERR_NO_FLOAT, "got %s", pb_tf_strerror(err));
		zassert_str_equal(st.field, "f_float");
	}
}

/*
 * ===========================================================================
 * The error table: every code reachable from milestone 2, with its offset and
 * field name.
 * ===========================================================================
 */

ZTEST_SUITE(pb_tf_errors, NULL, NULL, NULL, NULL, NULL);

ZTEST(pb_tf_errors, test_unknown_field)
{
	fx_shapes_t m;
	pb_tf_status_t st;
	const char *txt = "plain_scalar: 1 nosuchfield: 2";

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, txt, 0U, &st), PB_TF_ERR_UNKNOWN_FIELD);
	zassert_equal(st.offset, off_of(txt, "nosuchfield"), "got offset %u", st.offset);
	zassert_is_null(st.field, "there is no field to name");
}

ZTEST(pb_tf_errors, test_missing_colon)
{
	fx_shapes_t m;
	pb_tf_status_t st;

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "plain_scalar 1", 0U, &st),
		      PB_TF_ERR_SYNTAX);
	zassert_str_equal(st.field, "plain_scalar");
}

ZTEST(pb_tf_errors, test_unterminated_brace)
{
	fx_shapes_t m;
	pb_tf_status_t st;

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "{plain_scalar: 1", 0U, &st),
		      PB_TF_ERR_SYNTAX);
	zassert_equal(st.offset, strlen("{plain_scalar: 1"), "must point at end of input");
}

/* Fact 15: outer braces are only unambiguous if '}' has to be the last token. */
ZTEST(pb_tf_errors, test_trailing_junk_after_brace)
{
	fx_shapes_t m;
	pb_tf_status_t st;
	const char *txt = "{plain_scalar: 1} junk";

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, txt, 0U, &st), PB_TF_ERR_SYNTAX);
	zassert_equal(st.offset, off_of(txt, "junk"));

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "plain_scalar: 1 }", 0U, &st),
		      PB_TF_ERR_SYNTAX, "an unopened '}' is not a field name either");
}

ZTEST(pb_tf_errors, test_list_on_non_repeated_field)
{
	fx_shapes_t m;
	pb_tf_status_t st;
	const char *txt = "plain_scalar: [1, 2]";

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, txt, 0U, &st), PB_TF_ERR_SYNTAX);
	zassert_equal(st.offset, off_of(txt, "["));
	zassert_str_equal(st.field, "plain_scalar");
}

ZTEST(pb_tf_errors, test_repeated_past_capacity)
{
	fx_shapes_t m;
	pb_tf_status_t st;

	/* max_count is 4. */
	pb_tf_err_t err = pb_tf_parse(&fx_shapes_t_tf, &m, "rep_scalar: [1,2,3,4]", 0U, &st);

	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.rep_scalar_count, 4);

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "rep_scalar: [1,2,3,4,5]", 0U, &st),
		      PB_TF_ERR_TOO_MANY);
	zassert_str_equal(st.field, "rep_scalar");
	zassert_equal(m.rep_scalar_count, 4, "the count must not exceed the array");
}

/* A failed value must not be counted, so the array stays consistent. */
ZTEST(pb_tf_errors, test_failed_element_is_not_committed)
{
	fx_shapes_t m;
	pb_tf_status_t st;

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "rep_scalar: [1, -2]", 0U, &st),
		      PB_TF_ERR_RANGE);
	zassert_equal(m.rep_scalar_count, 1, "only the good element counts");
	zassert_equal(m.rep_scalar[0], 1);
}

ZTEST(pb_tf_errors, test_float_literal_in_integer_field)
{
	fx_scalars_t m;
	pb_tf_status_t st;

	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_int32: 1.5", 0U, &st), PB_TF_ERR_TYPE);
	zassert_str_equal(st.field, "f_int32");

	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_int32: 1e3", 0U, NULL),
		      PB_TF_ERR_TYPE);
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_int32: 1f", 0U, NULL), PB_TF_ERR_TYPE);
}

ZTEST(pb_tf_errors, test_bad_bool_word)
{
	fx_scalars_t m;
	pb_tf_status_t st;

	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_bool: yes", 0U, &st), PB_TF_ERR_TYPE);
	zassert_str_equal(st.field, "f_bool");
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_bool: 2", 0U, NULL), PB_TF_ERR_RANGE);
}

ZTEST(pb_tf_errors, test_malformed_numbers)
{
	static const struct {
		const char *text;
		pb_tf_err_t want;
	} cases[] = {
		{"f_int32:", PB_TF_ERR_SYNTAX},
		{"f_int32: ", PB_TF_ERR_SYNTAX},
		{"f_int32: -", PB_TF_ERR_SYNTAX},
		{"f_int32: 0x", PB_TF_ERR_SYNTAX},
		{"f_int32: +1", PB_TF_ERR_SYNTAX},
		{"f_int32: 09", PB_TF_ERR_SYNTAX},
		{"f_int32: 99999999999999999999999999", PB_TF_ERR_RANGE},
	};

	ARRAY_FOR_EACH(cases, i) {
		fx_scalars_t m;

		zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, cases[i].text, 0U, NULL),
			      cases[i].want, "'%s'", cases[i].text);
	}
}

/* A fixed_count array has no count member to grow: a clean runtime error. */
ZTEST(pb_tf_errors, test_fixarray_is_unsupported)
{
	fx_shapes_t m;
	pb_tf_status_t st;
	const char *txt = "fixed_scalar: [1, 2]";

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, txt, 0U, &st), PB_TF_ERR_UNSUPPORTED);
	zassert_equal(st.offset, off_of(txt, "fixed_scalar"));
	zassert_str_equal(st.field, "fixed_scalar");
}

/* An empty message has no addressable fields at all. */
ZTEST(pb_tf_errors, test_empty_message_rejects_any_field)
{
	fx_empty_t m;
	pb_tf_status_t st;

	zassert_equal(pb_tf_parse(&fx_empty_t_tf, &m, "", 0U, &st), PB_TF_OK);
	zassert_equal(pb_tf_parse(&fx_empty_t_tf, &m, "{}", 0U, &st), PB_TF_OK);
	zassert_equal(pb_tf_parse(&fx_empty_t_tf, &m, "anything: 1", 0U, &st),
		      PB_TF_ERR_UNKNOWN_FIELD);
}

ZTEST(pb_tf_errors, test_status_is_optional_and_reset)
{
	fx_shapes_t m;
	pb_tf_status_t st;

	/* NULL status must not crash. */
	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "plain_scalar: 1", 0U, NULL), PB_TF_OK);

	/* A stale failure must be cleared on the next success. */
	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "bogus: 1", 0U, &st),
		      PB_TF_ERR_UNKNOWN_FIELD);
	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "plain_scalar: 1", 0U, &st), PB_TF_OK);
	zassert_equal(st.code, PB_TF_OK);
	zassert_equal(st.offset, 0);
	zassert_is_null(st.field);
}

ZTEST(pb_tf_errors, test_null_arguments)
{
	fx_shapes_t m;

	zassert_equal(pb_tf_parse(NULL, &m, "", 0U, NULL), PB_TF_ERR_INTERNAL);
	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, NULL, "", 0U, NULL), PB_TF_ERR_INTERNAL);
	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, NULL, 0U, NULL), PB_TF_ERR_INTERNAL);
}

/*
 * nanopb's anonymous_oneof option emits the union without a name, so the
 * FIELDLIST row's third tuple element loses its "pick." prefix. PB_TF_DEFINE
 * reads only the first two elements, and the _MSGTYPE symbol is unaffected, so
 * everything below must behave exactly as the named oneof does. That the
 * submessage member even compiles is the load-bearing part.
 */
ZTEST(pb_tf_parse, test_anonymous_oneof)
{
	fx_anon_t m;
	pb_tf_status_t st;
	pb_tf_err_t err;
	const pb_tf_field_t *f;

	/* The rows carry the member names, not the union name. */
	f = pb_tf_field_find(&fx_anon_t_tf, "p_num", 5);
	zassert_not_null(f, "anonymous oneof members must still be addressable");
	zassert_equal(PB_TF_HT(f->rules), PB_TF_HT_ONEOF);
	zassert_is_null(pb_tf_field_find(&fx_anon_t_tf, "pick", 4),
			"the union itself is not a field");

	/* The submessage member resolved its descriptor through _MSGTYPE. */
	f = pb_tf_field_find(&fx_anon_t_tf, "p_msg", 5);
	zassert_not_null(f);
	zassert_equal_ptr(f->sub, &fx_inner_t_tf);

	err = pb_tf_parse(&fx_anon_t_tf, &m, "keep: 4 p_num: 9", 0U, &st);
	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.keep, 4);
	zassert_equal(m.which_pick, FX_ANON_P_NUM_TAG);
	/* Anonymous: the member is reached directly, with no .pick in the path. */
	zassert_equal(m.p_num, 9);
}
