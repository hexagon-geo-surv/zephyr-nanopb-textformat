/*
 * Copyright (c) 2026 Rodrigo Peixoto
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>

#if defined(CONFIG_NANOPB_TEXTFORMAT_FLOAT)
/* isinf()/isnan() for the float assertions. */
#include <math.h>
#endif

#include <nanopb_textformat.h>
#include <pb_common.h>

/* White-box: the row table is asserted through the library's internal helpers. */
#include "nanopb_textformat_priv.h"

/* zephyr_nanopb_sources() adds only ${CMAKE_CURRENT_BINARY_DIR} to the include
 * path (zephyr/modules/nanopb/nanopb.cmake:35) while protoc mirrors the proto's
 * source-relative path into the output tree, so the generated header is at
 * <build>/src/fixture.pb.h. */
#include "src/fixture.pb.h"
#include "src/fixture2.pb.h"
#include "src/fixture3.pb.h"

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
PB_TF_DEFINE(FX_WIDE, fx_wide_t);
PB_TF_DEFINE(FX_FIT, fx_fit_t);

/* proto2 shapes: required, an extensions range, MSG_W_CB, and non-STATIC. */
PB_TF_DEFINE(FX_P2, fx_p2_t);
PB_TF_DEFINE(FX_P2_INNER, fx_p2_inner_t);
PB_TF_DEFINE(FX_ALLOC, fx_alloc_t);

/*
 * Default nanopb naming. Note both arguments are the same token here, whereas
 * every definition above passes a different pair -- that is exactly the
 * difference PB_TF_DEFINE has to absorb, and it absorbs it by keying the
 * descriptor off structname and taking msgname separately, as PB_BIND does.
 */
PB_TF_DEFINE(FxDef, FxDef);
PB_TF_DEFINE(FxDefInner, FxDefInner);

/* The tables must agree with nanopb's own field counts. */
BUILD_ASSERT(PB_TF_FIELD_COUNT(FX_SCALARS_FIELDLIST) == 20);
BUILD_ASSERT(PB_TF_FIELD_COUNT(FX_SHAPES_FIELDLIST) == 12);
BUILD_ASSERT(PB_TF_FIELD_COUNT(FX_INNER_FIELDLIST) == 2);
BUILD_ASSERT(PB_TF_FIELD_COUNT(FX_WIDE_FIELDLIST) == 2);
BUILD_ASSERT(PB_TF_FIELD_COUNT(FX_FIT_FIELDLIST) == 1);
BUILD_ASSERT(PB_TF_FIELD_COUNT(FX_EMPTY_FIELDLIST) == 0);
BUILD_ASSERT(PB_TF_FIELD_COUNT(FX_ANON_FIELDLIST) == 3);
/* Six rows, because the extensions range synthesises one. */
BUILD_ASSERT(PB_TF_FIELD_COUNT(FX_P2_FIELDLIST) == 6);
BUILD_ASSERT(PB_TF_FIELD_COUNT(FX_ALLOC_FIELDLIST) == 3);
BUILD_ASSERT(PB_TF_FIELD_COUNT(FxDef_FIELDLIST) == 7);

ZTEST_SUITE(pb_tf_macro, NULL, NULL, NULL, NULL, NULL);

/* The version string and the numbers must not drift apart. */
ZTEST(pb_tf_macro, test_version_macros)
{
	char expected[16];

	(void)snprintf(expected, sizeof(expected), "%d.%d.%d", NANOPB_TEXTFORMAT_VERSION_MAJOR,
		       NANOPB_TEXTFORMAT_VERSION_MINOR, NANOPB_TEXTFORMAT_VERSION_PATCH);
	zassert_str_equal(NANOPB_TEXTFORMAT_VERSION_STRING, expected);

	/* The encoded form is what users compare against. */
	zassert_equal(NANOPB_TEXTFORMAT_VERSION,
		      (NANOPB_TEXTFORMAT_VERSION_MAJOR << 16) |
			      (NANOPB_TEXTFORMAT_VERSION_MINOR << 8) |
			      NANOPB_TEXTFORMAT_VERSION_PATCH);
}

/* The descriptor must describe the same message nanopb does. */
ZTEST(pb_tf_macro, test_descriptor_matches_nanopb)
{
	zassert_equal_ptr(fx_scalars_t_tf.desc, &fx_scalars_t_msg);
	zassert_equal(fx_scalars_t_tf.count, 20, "got %u", fx_scalars_t_tf.count);
	zassert_equal(fx_scalars_t_tf.struct_size, sizeof(fx_scalars_t));

	zassert_equal(fx_shapes_t_tf.count, 12, "got %u", fx_shapes_t_tf.count);
	zassert_equal(fx_shapes_t_tf.struct_size, sizeof(fx_shapes_t));

	/* An empty message: nothing but the sentinel row. */
	zassert_equal(fx_empty_t_tf.count, 0, "got %u", fx_empty_t_tf.count);
	zassert_not_null(fx_empty_t_tf.fields);
	zassert_is_null(fx_empty_t_tf.fields[0].name);
}

/* Names come from #fieldname, and lookup is by exact match. */
ZTEST(pb_tf_macro, test_field_names_and_tags)
{
	const struct pb_tf_field *f = &fx_scalars_t_tf.fields[0];

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
	const struct pb_tf_field *fl = pb_tf_field_find(&fx_scalars_t_tf, "f_float", 7);
	const struct pb_tf_field *fx = pb_tf_field_find(&fx_scalars_t_tf, "f_fixed32", 9);
	const struct pb_tf_field *sf = pb_tf_field_find(&fx_scalars_t_tf, "f_sfixed32", 10);

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
	const struct pb_tf_msg *tf = &fx_shapes_t_tf;

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
 * byte-identical in the generated FIELDLIST.
 */
ZTEST(pb_tf_macro, test_singular_and_optional_submessage_are_identical)
{
	const struct pb_tf_field *s = pb_tf_field_find(&fx_shapes_t_tf, "sing_msg", 8);
	const struct pb_tf_field *o = pb_tf_field_find(&fx_shapes_t_tf, "opt_msg", 7);

	zassert_equal(s->vtype, PB_TF_VT_MESSAGE);
	zassert_equal(o->vtype, PB_TF_VT_MESSAGE);
	zassert_equal(s->rules, o->rules, "0x%02x vs 0x%02x", s->rules, o->rules);
}

/* The recursive tree: a message row points at the submessage's descriptor. */
ZTEST(pb_tf_macro, test_submessage_descriptors_are_linked)
{
	const struct pb_tf_field *sing = pb_tf_field_find(&fx_shapes_t_tf, "sing_msg", 8);
	const struct pb_tf_field *rep = pb_tf_field_find(&fx_shapes_t_tf, "rep_msg", 7);
	const struct pb_tf_field *cmsg = pb_tf_field_find(&fx_shapes_t_tf, "c_msg", 5);

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
	struct pb_tf_buf b = {.buf = small, .size = sizeof(small)};

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
		const char *s = pb_tf_strerror((int)c);

		zassert_not_null(s);
		zassert_true(strlen(s) > 0U);
		zassert_true(strcmp(s, "unknown error") != 0, "code %d has no message", c);
	}
}

/*
 * ===========================================================================
 * Parsing: the lexer, field shapes, and integer and bool values.
 *
 * Submessages, strings and bytes, and floats each have their own suite below.
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
	struct pb_tf_status st;

	int err = pb_tf_parse(&fx_shapes_t_tf, &m, "plain_scalar: 42", &st);

	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.plain_scalar, 42);
}

/* An implicit-presence scalar has no has_ bit; an optional one does. */
ZTEST(pb_tf_parse, test_optional_sets_presence)
{
	fx_shapes_t m;

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "plain_scalar: 1", NULL), PB_TF_OK);
	zassert_false(m.has_opt_scalar, "opt_scalar was not mentioned");

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "opt_scalar: 0", NULL), PB_TF_OK);
	zassert_true(m.has_opt_scalar, "presence must be set even for a zero value");
	zassert_equal(m.opt_scalar, 0);
}

ZTEST(pb_tf_parse, test_repeated_by_repetition_appends)
{
	fx_shapes_t m;

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "rep_scalar: 1 rep_scalar: 2 rep_scalar: 3",
				  NULL),
		      PB_TF_OK);
	zassert_equal(m.rep_scalar_count, 3, "got %u", m.rep_scalar_count);
	zassert_equal(m.rep_scalar[0], 1);
	zassert_equal(m.rep_scalar[1], 2);
	zassert_equal(m.rep_scalar[2], 3);
}

ZTEST(pb_tf_parse, test_repeated_short_list)
{
	fx_shapes_t m;

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "rep_scalar: [7, 8]", NULL), PB_TF_OK);
	zassert_equal(m.rep_scalar_count, 2, "got %u", m.rep_scalar_count);
	zassert_equal(m.rep_scalar[0], 7);
	zassert_equal(m.rep_scalar[1], 8);
}

/* "x: []" is legal text format and appends nothing. */
ZTEST(pb_tf_parse, test_empty_list_is_a_noop)
{
	fx_shapes_t m;

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "rep_scalar: [] plain_scalar: 5", NULL),
		      PB_TF_OK);
	zassert_equal(m.rep_scalar_count, 0, "got %u", m.rep_scalar_count);
	zassert_equal(m.plain_scalar, 5);
}

/* A list and repetition both append, so they compose. */
ZTEST(pb_tf_parse, test_list_then_repetition_accumulates)
{
	fx_shapes_t m;

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "rep_scalar: [1,2] rep_scalar: 3", NULL),
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
		struct pb_tf_status st;

		int err = pb_tf_parse(&fx_shapes_t_tf, &m, forms[i], &st);

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

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "{plain_scalar: 9}", NULL), PB_TF_OK);
	zassert_equal(m.plain_scalar, 9);

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "  { plain_scalar: 8 }  ", NULL),
		      PB_TF_OK);
	zassert_equal(m.plain_scalar, 8);

	/* An empty body, braced or not. */
	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "{}", NULL), PB_TF_OK);
	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "", NULL), PB_TF_OK);
	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "   ", NULL), PB_TF_OK);
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
		struct pb_tf_status st;

		int err = pb_tf_parse(&fx_scalars_t_tf, &m, cases[i].text, &st);

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
		struct pb_tf_status st;

		int err = pb_tf_parse(&fx_scalars_t_tf, &m, cases[i].text, &st);

		zassert_equal(err, PB_TF_OK, "'%s': %s", cases[i].text, pb_tf_strerror(st.code));
		zassert_equal(m.f_bool, cases[i].want, "'%s'", cases[i].text);
	}
}

/* Enums are numeric in both directions; the signed one accepts negatives. */
ZTEST(pb_tf_parse, test_enums_are_numeric)
{
	fx_scalars_t m;
	struct pb_tf_status st;

	int err = pb_tf_parse(&fx_scalars_t_tf, &m, "f_uenum: 2 f_enum: -1", &st);

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
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_uenum: FX_MODE_FAST", &st),
		      -PB_TF_ERR_SYNTAX);
}

/*
 * The whole point of dispatching on iter.data_size: an int_size-narrowed field
 * is 1 or 4 bytes, and a value that does not fit must be refused rather than
 * silently truncated.
 */
ZTEST(pb_tf_parse, test_narrowed_widths_range_check)
{
	fx_scalars_t m;
	struct pb_tf_status st;

	/* int_size:IS_8 -> int8_t. */
	zassert_equal(sizeof(m.f_narrow), 1);
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_narrow: 127", NULL), PB_TF_OK);
	zassert_equal(m.f_narrow, 127);
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_narrow: -128", NULL), PB_TF_OK);
	zassert_equal(m.f_narrow, -128);

	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_narrow: 128", &st),
		      -PB_TF_ERR_RANGE, "128 must not truncate into an int8_t");
	zassert_str_equal(st.field, "f_narrow");
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_narrow: -129", NULL),
		      -PB_TF_ERR_RANGE);

	/* int_size:IS_32 on a proto int64 -> int32_t. */
	zassert_equal(sizeof(m.f_narrow32), 4);
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_narrow32: 2147483647", NULL),
		      PB_TF_OK);
	zassert_equal(m.f_narrow32, INT32_MAX);
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_narrow32: 2147483648", NULL),
		      -PB_TF_ERR_RANGE);
}

ZTEST(pb_tf_parse, test_unsigned_rejects_negative)
{
	fx_scalars_t m;
	struct pb_tf_status st;

	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_uint32: -1", &st), -PB_TF_ERR_RANGE,
		      "a negative literal must not wrap into an unsigned field");
	zassert_str_equal(st.field, "f_uint32");

	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_uint32: 4294967295", NULL),
		      PB_TF_OK);
	zassert_equal(m.f_uint32, UINT32_MAX);
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_uint32: 4294967296", NULL),
		      -PB_TF_ERR_RANGE);
}

/* sint and fixed widths take the same path as the plain varints. */
ZTEST(pb_tf_parse, test_sint_and_fixed_widths)
{
	fx_scalars_t m;
	struct pb_tf_status st;

	int err = pb_tf_parse(
		&fx_scalars_t_tf, &m, "f_sint32: -5 f_fixed32: 4294967295 f_sfixed32: -2147483648",
		&st);

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
	struct pb_tf_status st;
	int err = pb_tf_parse(&fx_scalars_t_tf, &m, "f_int64: 1", &st);

	zassert_equal(sizeof(m.f_int64), 8, "the generator never narrows this");

	if (IS_ENABLED(CONFIG_NANOPB_WITHOUT_64BIT)) {
		zassert_equal(err, -PB_TF_ERR_UNSUPPORTED, "got %s", pb_tf_strerror(err));
		zassert_str_equal(st.field, "f_int64");
	} else {
		zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
		zassert_equal(m.f_int64, 1);

		zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m,
					  "f_int64: 9223372036854775807", NULL),
			      PB_TF_OK);
		zassert_equal(m.f_int64, INT64_MAX);
		zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m,
					  "f_int64: -9223372036854775808", NULL),
			      PB_TF_OK);
		zassert_equal(m.f_int64, INT64_MIN);
		zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m,
					  "f_int64: 9223372036854775808", NULL),
			      -PB_TF_ERR_RANGE);
	}
}

ZTEST(pb_tf_parse, test_oneof_selects_member)
{
	fx_shapes_t m;
	struct pb_tf_status st;

	int err = pb_tf_parse(&fx_shapes_t_tf, &m, "c_num: 77", &st);

	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.which_choice, FX_SHAPES_C_NUM_TAG);
	zassert_equal(m.choice.c_num, 77);

	/* Re-stating the same member overwrites and keeps the selection. */
	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "c_num: 1 c_num: 2", NULL), PB_TF_OK);
	zassert_equal(m.which_choice, FX_SHAPES_C_NUM_TAG);
	zassert_equal(m.choice.c_num, 2);
}

/* pb_tf_parse() clears first; pb_tf_merge() does not. */
ZTEST(pb_tf_parse, test_clear_and_noinit)
{
	fx_shapes_t m;

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "plain_scalar: 1 rep_scalar: [1,2]",
				  NULL),
		      PB_TF_OK);
	zassert_equal(m.rep_scalar_count, 2);

	/* Default: the previous contents are gone. */
	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "opt_scalar: 3", NULL), PB_TF_OK);
	zassert_equal(m.plain_scalar, 0, "parse must clear by default");
	zassert_equal(m.rep_scalar_count, 0, "parse must clear by default");

	/* NOINIT: merge into what is there, and repeated fields keep appending. */
	m.plain_scalar = 5;
	m.rep_scalar_count = 1;
	m.rep_scalar[0] = 9;
	zassert_equal(pb_tf_merge(&fx_shapes_t_tf, &m, "rep_scalar: 10", NULL),
		      PB_TF_OK);
	zassert_equal(m.plain_scalar, 5, "NOINIT must preserve untouched fields");
	zassert_equal(m.rep_scalar_count, 2);
	zassert_equal(m.rep_scalar[1], 10);
}

/* A float field follows CONFIG_NANOPB_TEXTFORMAT_FLOAT in both directions. */
ZTEST(pb_tf_parse, test_float_field_follows_the_build)
{
	fx_scalars_t m;
	struct pb_tf_status st;
	int err = pb_tf_parse(&fx_scalars_t_tf, &m, "f_float: 1.5", &st);

	if (IS_ENABLED(CONFIG_NANOPB_TEXTFORMAT_FLOAT)) {
		zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	} else {
		zassert_equal(err, -PB_TF_ERR_NO_FLOAT, "got %s", pb_tf_strerror(err));
		zassert_str_equal(st.field, "f_float");
	}
}

/*
 * ===========================================================================
 * The error table: each code, with the offset and field name it reports.
 * ===========================================================================
 */

ZTEST_SUITE(pb_tf_errors, NULL, NULL, NULL, NULL, NULL);

ZTEST(pb_tf_errors, test_unknown_field)
{
	fx_shapes_t m;
	struct pb_tf_status st;
	const char *txt = "plain_scalar: 1 nosuchfield: 2";

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, txt, &st), -PB_TF_ERR_UNKNOWN_FIELD);
	zassert_equal(st.offset, off_of(txt, "nosuchfield"), "got offset %u", st.offset);
	zassert_is_null(st.field, "there is no field to name");
}

ZTEST(pb_tf_errors, test_missing_colon)
{
	fx_shapes_t m;
	struct pb_tf_status st;

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "plain_scalar 1", &st),
		      -PB_TF_ERR_SYNTAX);
	zassert_str_equal(st.field, "plain_scalar");
}

ZTEST(pb_tf_errors, test_unterminated_brace)
{
	fx_shapes_t m;
	struct pb_tf_status st;

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "{plain_scalar: 1", &st),
		      -PB_TF_ERR_SYNTAX);
	zassert_equal(st.offset, strlen("{plain_scalar: 1"), "must point at end of input");
}

/* Fact 15: outer braces are only unambiguous if '}' has to be the last token. */
ZTEST(pb_tf_errors, test_trailing_junk_after_brace)
{
	fx_shapes_t m;
	struct pb_tf_status st;
	const char *txt = "{plain_scalar: 1} junk";

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, txt, &st), -PB_TF_ERR_SYNTAX);
	zassert_equal(st.offset, off_of(txt, "junk"));

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "plain_scalar: 1 }", &st),
		      -PB_TF_ERR_SYNTAX, "an unopened '}' is not a field name either");
}

ZTEST(pb_tf_errors, test_list_on_non_repeated_field)
{
	fx_shapes_t m;
	struct pb_tf_status st;
	const char *txt = "plain_scalar: [1, 2]";

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, txt, &st), -PB_TF_ERR_SYNTAX);
	zassert_equal(st.offset, off_of(txt, "["));
	zassert_str_equal(st.field, "plain_scalar");
}

ZTEST(pb_tf_errors, test_repeated_past_capacity)
{
	fx_shapes_t m;
	struct pb_tf_status st;

	/* max_count is 4. */
	int err = pb_tf_parse(&fx_shapes_t_tf, &m, "rep_scalar: [1,2,3,4]", &st);

	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.rep_scalar_count, 4);

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "rep_scalar: [1,2,3,4,5]", &st),
		      -PB_TF_ERR_TOO_MANY);
	zassert_str_equal(st.field, "rep_scalar");
	zassert_equal(m.rep_scalar_count, 4, "the count must not exceed the array");
}

/* A failed value must not be counted, so the array stays consistent. */
ZTEST(pb_tf_errors, test_failed_element_is_not_committed)
{
	fx_shapes_t m;
	struct pb_tf_status st;

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "rep_scalar: [1, -2]", &st),
		      -PB_TF_ERR_RANGE);
	zassert_equal(m.rep_scalar_count, 1, "only the good element counts");
	zassert_equal(m.rep_scalar[0], 1);
}

ZTEST(pb_tf_errors, test_float_literal_in_integer_field)
{
	fx_scalars_t m;
	struct pb_tf_status st;

	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_int32: 1.5", &st), -PB_TF_ERR_TYPE);
	zassert_str_equal(st.field, "f_int32");

	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_int32: 1e3", NULL),
		      -PB_TF_ERR_TYPE);
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_int32: 1f", NULL), -PB_TF_ERR_TYPE);
}

ZTEST(pb_tf_errors, test_bad_bool_word)
{
	fx_scalars_t m;
	struct pb_tf_status st;

	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_bool: yes", &st), -PB_TF_ERR_TYPE);
	zassert_str_equal(st.field, "f_bool");
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_bool: 2", NULL), -PB_TF_ERR_RANGE);
}

ZTEST(pb_tf_errors, test_malformed_numbers)
{
	static const struct {
		const char *text;
		int want;
	} cases[] = {
		{"f_int32:", -PB_TF_ERR_SYNTAX},
		{"f_int32: ", -PB_TF_ERR_SYNTAX},
		{"f_int32: -", -PB_TF_ERR_SYNTAX},
		{"f_int32: 0x", -PB_TF_ERR_SYNTAX},
		{"f_int32: +1", -PB_TF_ERR_SYNTAX},
		{"f_int32: 09", -PB_TF_ERR_SYNTAX},
		{"f_int32: 99999999999999999999999999", -PB_TF_ERR_RANGE},
	};

	ARRAY_FOR_EACH(cases, i) {
		fx_scalars_t m;

		zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, cases[i].text, NULL),
			      cases[i].want, "'%s'", cases[i].text);
	}
}

/* A fixed_count array has no count member to grow: a clean runtime error. */
ZTEST(pb_tf_errors, test_fixarray_is_unsupported)
{
	fx_shapes_t m;
	struct pb_tf_status st;
	const char *txt = "fixed_scalar: [1, 2]";

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, txt, &st), -PB_TF_ERR_UNSUPPORTED);
	zassert_equal(st.offset, off_of(txt, "fixed_scalar"));
	zassert_str_equal(st.field, "fixed_scalar");
}

/* An empty message has no addressable fields at all. */
ZTEST(pb_tf_errors, test_empty_message_rejects_any_field)
{
	fx_empty_t m;
	struct pb_tf_status st;

	zassert_equal(pb_tf_parse(&fx_empty_t_tf, &m, "", &st), PB_TF_OK);
	zassert_equal(pb_tf_parse(&fx_empty_t_tf, &m, "{}", &st), PB_TF_OK);
	zassert_equal(pb_tf_parse(&fx_empty_t_tf, &m, "anything: 1", &st),
		      -PB_TF_ERR_UNKNOWN_FIELD);
}

ZTEST(pb_tf_errors, test_status_is_optional_and_reset)
{
	fx_shapes_t m;
	struct pb_tf_status st;

	/* NULL status must not crash. */
	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "plain_scalar: 1", NULL), PB_TF_OK);

	/* A stale failure must be cleared on the next success. */
	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "bogus: 1", &st),
		      -PB_TF_ERR_UNKNOWN_FIELD);
	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "plain_scalar: 1", &st), PB_TF_OK);
	zassert_equal(st.code, PB_TF_OK);
	zassert_equal(st.offset, 0);
	zassert_is_null(st.field);
}

ZTEST(pb_tf_errors, test_null_arguments)
{
	fx_shapes_t m;

	zassert_equal(pb_tf_parse(NULL, &m, "", NULL), -PB_TF_ERR_INTERNAL);
	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, NULL, "", NULL), -PB_TF_ERR_INTERNAL);
	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, NULL, NULL), -PB_TF_ERR_INTERNAL);
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
	struct pb_tf_status st;
	int err;
	const struct pb_tf_field *f;

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

	err = pb_tf_parse(&fx_anon_t_tf, &m, "keep: 4 p_num: 9", &st);
	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.keep, 4);
	zassert_equal(m.which_pick, FX_ANON_P_NUM_TAG);
	/* Anonymous: the member is reached directly, with no .pick in the path. */
	zassert_equal(m.p_num, 9);
}

/*
 * ===========================================================================
 * Milestone 3: submessages.
 * ===========================================================================
 */

ZTEST_SUITE(pb_tf_submsg, NULL, NULL, NULL, NULL, NULL);

/* The colon before a submessage is optional; the braces are not. */
ZTEST(pb_tf_submsg, test_colon_is_optional_before_braces)
{
	static const char *const forms[] = {
		"sing_msg { a: 7 }",
		"sing_msg: { a: 7 }",
		"sing_msg{a:7}",
		"sing_msg :{ a : 7 }",
	};

	ARRAY_FOR_EACH(forms, i) {
		fx_shapes_t m;
		struct pb_tf_status st;
		int err = pb_tf_parse(&fx_shapes_t_tf, &m, forms[i], &st);

		zassert_equal(err, PB_TF_OK, "form %zu (%s): %s", i, forms[i],
			      pb_tf_strerror(st.code));
		zassert_true(m.has_sing_msg, "form %zu", i);
		zassert_equal(m.sing_msg.a, 7, "form %zu", i);
	}
}

/* A scalar where a submessage belongs must not be silently accepted. */
ZTEST(pb_tf_submsg, test_submessage_requires_braces)
{
	fx_shapes_t m;
	struct pb_tf_status st;

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "sing_msg: 5", &st), -PB_TF_ERR_SYNTAX);
	zassert_str_equal(st.field, "sing_msg");
}

ZTEST(pb_tf_submsg, test_nested_two_levels)
{
	fx_deep1_t m;
	struct pb_tf_status st;
	int err = pb_tf_parse(&fx_deep1_t_tf, &m, "d2 { d3 { v: 5 } }", &st);

	if (CONFIG_NANOPB_TEXTFORMAT_MAX_DEPTH >= 2) {
		zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
		zassert_equal(m.d2.d3.v, 5);
	} else {
		zassert_equal(err, -PB_TF_ERR_DEPTH, "got %s", pb_tf_strerror(err));
	}
}

/* Each level costs a stack frame, so the cap must stop the call. */
ZTEST(pb_tf_submsg, test_depth_cap)
{
	fx_deep1_t m;
	struct pb_tf_status st;

	/* One level always fits: the Kconfig range starts at 1. */
	int err = pb_tf_parse(&fx_deep1_t_tf, &m, "d2 { }", &st);

	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));

	if (CONFIG_NANOPB_TEXTFORMAT_MAX_DEPTH == 1) {
		zassert_equal(pb_tf_parse(&fx_deep1_t_tf, &m, "d2 { d3 { v: 1 } }", &st),
			      -PB_TF_ERR_DEPTH);
		zassert_str_equal(st.field, "d3", "the field that could not be entered");
	}
}

ZTEST(pb_tf_submsg, test_repeated_submessage_list)
{
	fx_shapes_t m;
	struct pb_tf_status st;
	int err =
		pb_tf_parse(&fx_shapes_t_tf, &m, "rep_msg: [{a: 1}, {a: 2, b: -3}]", &st);

	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.rep_msg_count, 2, "got %u", m.rep_msg_count);
	zassert_equal(m.rep_msg[0].a, 1);
	zassert_equal(m.rep_msg[1].a, 2);
	zassert_equal(m.rep_msg[1].b, -3);
}

ZTEST(pb_tf_submsg, test_repeated_submessage_by_repetition)
{
	fx_shapes_t m;
	struct pb_tf_status st;
	int err =
		pb_tf_parse(&fx_shapes_t_tf, &m, "rep_msg { a: 1 } rep_msg { a: 2 }", &st);

	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.rep_msg_count, 2, "got %u", m.rep_msg_count);
	zassert_equal(m.rep_msg[0].a, 1);
	zassert_equal(m.rep_msg[1].a, 2);
}

/*
 * An appended element must not inherit the previous occupant's bytes. Fill the
 * array first, then reparse into it with NOINIT after resetting only the count.
 */
ZTEST(pb_tf_submsg, test_appended_element_is_cleared)
{
	fx_shapes_t m;

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "rep_msg: [{a: 1, b: 2}]", NULL),
		      PB_TF_OK);
	zassert_equal(m.rep_msg[0].b, 2);

	/* Slot 0 still holds b == 2; writing it again must start from zero. */
	m.rep_msg_count = 0;
	zassert_equal(pb_tf_merge(&fx_shapes_t_tf, &m, "rep_msg: [{a: 9}]", NULL),
		      PB_TF_OK);
	zassert_equal(m.rep_msg[0].a, 9);
	zassert_equal(m.rep_msg[0].b, 0, "a fresh element must not inherit ghost bytes");
}

/* A singular or optional submessage mentioned twice merges, per text format. */
ZTEST(pb_tf_submsg, test_singular_submessage_merges)
{
	fx_shapes_t m;
	struct pb_tf_status st;
	int err =
		pb_tf_parse(&fx_shapes_t_tf, &m, "sing_msg { a: 1 } sing_msg { b: 2 }", &st);

	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.sing_msg.a, 1, "the first mention must survive the second");
	zassert_equal(m.sing_msg.b, 2);
}

ZTEST(pb_tf_submsg, test_oneof_message_member)
{
	fx_shapes_t m;
	struct pb_tf_status st;
	int err = pb_tf_parse(&fx_shapes_t_tf, &m, "c_msg { a: 4 }", &st);

	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.which_choice, FX_SHAPES_C_MSG_TAG);
	zassert_equal(m.choice.c_msg.a, 4);
}

/*
 * Switching oneof members must clear the union first. c_num and c_msg overlap,
 * so without the clear the message member would start from the integer's bytes.
 */
ZTEST(pb_tf_submsg, test_oneof_switch_clears_union)
{
	fx_shapes_t m;
	struct pb_tf_status st;
	int err;

	/* Set every byte of the union, then switch to the message member. */
	err = pb_tf_parse(&fx_shapes_t_tf, &m, "c_num: 4294967295", &st);
	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));

	err = pb_tf_parse(&fx_shapes_t_tf, &m, "c_num: 4294967295 c_msg { a: 1 }", &st);
	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.which_choice, FX_SHAPES_C_MSG_TAG);
	zassert_equal(m.choice.c_msg.a, 1);
	zassert_equal(m.choice.c_msg.b, 0, "switching members must clear the union");

	/* And the other way round. */
	err = pb_tf_parse(&fx_shapes_t_tf, &m, "c_msg { a: 7, b: 7 } c_num: 3", &st);
	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.which_choice, FX_SHAPES_C_NUM_TAG);
	zassert_equal(m.choice.c_num, 3);
}

ZTEST(pb_tf_submsg, test_anonymous_oneof_message_member)
{
	fx_anon_t m;
	struct pb_tf_status st;
	int err = pb_tf_parse(&fx_anon_t_tf, &m, "keep: 1 p_msg { a: 2 }", &st);

	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.keep, 1);
	zassert_equal(m.which_pick, FX_ANON_P_MSG_TAG);
	zassert_equal(m.p_msg.a, 2);
}

ZTEST(pb_tf_submsg, test_errors_inside_a_submessage)
{
	fx_shapes_t m;
	struct pb_tf_status st;
	const char *txt = "sing_msg { nosuch: 1 }";

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, txt, &st), -PB_TF_ERR_UNKNOWN_FIELD);
	zassert_equal(st.offset, off_of(txt, "nosuch"), "offset must be inside the submessage");

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "sing_msg { a: 1", &st),
		      -PB_TF_ERR_SYNTAX, "unterminated submessage");

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "sing_msg { a: -1 }", &st),
		      -PB_TF_ERR_RANGE);
	zassert_str_equal(st.field, "a", "the innermost field is the one at fault");
}

/*
 * ===========================================================================
 * proto2-only shapes.
 * ===========================================================================
 */

/* A required field has size_offset 0, so pSize is NULL just like SINGULAR. */
ZTEST(pb_tf_submsg, test_proto2_required)
{
	fx_p2_t m;
	struct pb_tf_status st;
	pb_field_iter_t it;
	const struct pb_tf_field *f = pb_tf_field_find(&fx_p2_t_tf, "req_num", 7);
	int err;

	zassert_not_null(f);
	zassert_equal(PB_TF_HT(f->rules), PB_TF_HT_REQUIRED, "got 0x%02x", f->rules);

	zassert_true(pb_field_iter_begin(&it, &fx_p2_t_msg, &m));
	zassert_true(pb_field_iter_find(&it, FX_P2_REQ_NUM_TAG));
	zassert_is_null(it.pSize, "a required field has no presence member to write");

	err = pb_tf_parse(&fx_p2_t_tf, &m, "req_num: 11 opt_num: 12 rep_num: [1,2]", &st);
	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.req_num, 11);
	zassert_true(m.has_opt_num);
	zassert_equal(m.rep_num_count, 2);
}

/* Presence of proto2 `required` is documented as not enforced. */
ZTEST(pb_tf_submsg, test_proto2_required_is_not_enforced)
{
	fx_p2_t m;

	zassert_equal(pb_tf_parse(&fx_p2_t_tf, &m, "opt_num: 1", NULL), PB_TF_OK,
		      "a missing required field is the caller's business, not ours");
	zassert_equal(m.req_num, 0);
}

/*
 * MSG_W_CB is MESSAGE plus a pb_callback_t, which nanopb emits ahead of the
 * has_ bit. size_offset derives from has_, so the callback sits strictly below
 * pSize and writing through pData must leave it alone.
 */
ZTEST(pb_tf_submsg, test_msg_w_cb_leaves_the_callback_alone)
{
	fx_p2_t m;
	struct pb_tf_status st;
	const struct pb_tf_field *f = pb_tf_field_find(&fx_p2_t_tf, "cb_sub", 6);
	int err;

	zassert_not_null(f);
	zassert_equal(f->vtype, PB_TF_VT_MSG_W_CB, "got vtype %u", f->vtype);
	zassert_equal_ptr(f->sub, &fx_p2_inner_t_tf, "MSG_W_CB must resolve its descriptor");

	(void)memset(&m, 0, sizeof(m));
	m.cb_cb_sub.arg = (void *)&fx_p2_t_tf;

	err = pb_tf_merge(&fx_p2_t_tf, &m, "cb_sub { v: 3 }", &st);
	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.cb_sub.v, 3);
	zassert_true(m.has_cb_sub);
	zassert_equal_ptr(m.cb_cb_sub.arg, &fx_p2_t_tf, "the callback must be untouched");
}

/*
 * An extensions range synthesises a row. It must not break the build, and must
 * not be addressable -- "extensions" is nanopb's own placeholder name, not a
 * field the user wrote.
 */
ZTEST(pb_tf_submsg, test_extension_row_is_not_addressable)
{
	fx_p2_t m;
	struct pb_tf_status st;
	bool seen = false;

	/* The row exists in the table... */
	for (uint16_t i = 0U; i < fx_p2_t_tf.count; i++) {
		if (fx_p2_t_tf.fields[i].vtype == PB_TF_VT_EXTENSION) {
			seen = true;
			zassert_equal(fx_p2_t_tf.fields[i].tag, 100, "the range start");
		}
	}
	zassert_true(seen, "the extensions range must produce a row");

	/* ...but the name scan skips it. */
	zassert_is_null(pb_tf_field_find(&fx_p2_t_tf, "extensions", 10));
	zassert_equal(pb_tf_parse(&fx_p2_t_tf, &m, "extensions: 1", &st),
		      -PB_TF_ERR_UNKNOWN_FIELD);
}

/* POINTER and CALLBACK allocation: a runtime error, not a build break. */
ZTEST(pb_tf_submsg, test_non_static_allocation_is_unsupported)
{
	fx_alloc_t m;
	struct pb_tf_status st;

	/* The STATIC field in the same message still works. */
	int err = pb_tf_parse(&fx_alloc_t_tf, &m, "ok_num: 5", &st);

	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.ok_num, 5);

	zassert_equal(pb_tf_parse(&fx_alloc_t_tf, &m, "ptr_str: \"x\"", &st),
		      -PB_TF_ERR_UNSUPPORTED);
	zassert_str_equal(st.field, "ptr_str");

	zassert_equal(pb_tf_parse(&fx_alloc_t_tf, &m, "cb_str: \"x\"", &st),
		      -PB_TF_ERR_UNSUPPORTED);
	zassert_str_equal(st.field, "cb_str");
}

/*
 * ===========================================================================
 * Milestone 4: strings and bytes.
 * ===========================================================================
 */

ZTEST_SUITE(pb_tf_text, NULL, NULL, NULL, NULL, NULL);

ZTEST(pb_tf_text, test_plain_string)
{
	fx_scalars_t m;
	struct pb_tf_status st;
	int err = pb_tf_parse(&fx_scalars_t_tf, &m, "f_string: \"hello\"", &st);

	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_str_equal(m.f_string, "hello");
}

/* Either quote opens a literal; the other is then an ordinary byte inside. */
ZTEST(pb_tf_text, test_both_quote_styles)
{
	fx_scalars_t m;

	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_string: 'single'", NULL), PB_TF_OK);
	zassert_str_equal(m.f_string, "single");

	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_string: \"it's\"", NULL), PB_TF_OK);
	zassert_str_equal(m.f_string, "it's");

	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_string: 'say \"hi\"'", NULL),
		      PB_TF_OK);
	zassert_str_equal(m.f_string, "say \"hi\"");

	/* An empty literal is legal. */
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_string: \"\"", NULL), PB_TF_OK);
	zassert_str_equal(m.f_string, "");
}

ZTEST(pb_tf_text, test_simple_escapes)
{
	fx_scalars_t m;
	struct pb_tf_status st;
	int err = pb_tf_parse(&fx_scalars_t_tf, &m,
				      "f_string: \"\\a\\b\\f\\n\\r\\t\\v\\?\\\\\\'\\\"\"", &st);

	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_str_equal(m.f_string, "\a\b\f\n\r\t\v?\\'\"");
}

/* \xH and \xHH, via Zephyr's char2hex. */
ZTEST(pb_tf_text, test_hex_escapes)
{
	fx_scalars_t m;
	struct pb_tf_status st;
	int err;

	err = pb_tf_parse(&fx_scalars_t_tf, &m, "f_bytes: \"\\x41\\x7\\xfF\"", &st);
	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.f_bytes.size, 3, "got %u", m.f_bytes.size);
	zassert_equal(m.f_bytes.bytes[0], 0x41);
	zassert_equal(m.f_bytes.bytes[1], 0x07);
	zassert_equal(m.f_bytes.bytes[2], 0xFF);

	/* \x with no hex digit at all. */
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_bytes: \"\\xz\"", NULL),
		      -PB_TF_ERR_SYNTAX);
}

ZTEST(pb_tf_text, test_octal_escapes)
{
	fx_scalars_t m;
	struct pb_tf_status st;
	int err = pb_tf_parse(&fx_scalars_t_tf, &m, "f_bytes: \"\\0\\7\\77\\377\"", &st);

	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.f_bytes.size, 4, "got %u", m.f_bytes.size);
	zassert_equal(m.f_bytes.bytes[0], 0);
	zassert_equal(m.f_bytes.bytes[1], 7);
	zassert_equal(m.f_bytes.bytes[2], 077);
	zassert_equal(m.f_bytes.bytes[3], 0xFF);

	/* Three octal digits can exceed a byte. */
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_bytes: \"\\400\"", NULL),
		      -PB_TF_ERR_RANGE);
}

/* An embedded NUL is fine in bytes: the length is carried separately. */
ZTEST(pb_tf_text, test_bytes_may_contain_nul)
{
	fx_scalars_t m;
	struct pb_tf_status st;
	int err = pb_tf_parse(&fx_scalars_t_tf, &m, "f_bytes: \"a\\x00b\"", &st);

	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.f_bytes.size, 3, "got %u", m.f_bytes.size);
	zassert_equal(m.f_bytes.bytes[1], 0);
	zassert_equal(m.f_bytes.bytes[2], 'b');
}

ZTEST(pb_tf_text, test_unicode_escapes_become_utf8)
{
	static const struct {
		const char *text;
		const char *want;
		size_t len;
	} cases[] = {
		/* One, two and three UTF-8 bytes. */
		{"f_bytes: \"\\u0041\"", "\x41", 1},
		{"f_bytes: \"\\u00e9\"", "\xC3\xA9", 2},
		{"f_bytes: \"\\u20AC\"", "\xE2\x82\xAC", 3},
		{"f_bytes: \"\\u0000\"", "\x00", 1},
		{"f_bytes: \"\\u07FF\"", "\xDF\xBF", 2},
		{"f_bytes: \"\\uFFFF\"", "\xEF\xBF\xBF", 3},
	};

	ARRAY_FOR_EACH(cases, i) {
		fx_scalars_t m;
		struct pb_tf_status st;
		int err = pb_tf_parse(&fx_scalars_t_tf, &m, cases[i].text, &st);

		zassert_equal(err, PB_TF_OK, "'%s': %s", cases[i].text, pb_tf_strerror(st.code));
		zassert_equal(m.f_bytes.size, cases[i].len, "'%s' gave %u bytes", cases[i].text,
			      m.f_bytes.size);
		zassert_mem_equal(m.f_bytes.bytes, cases[i].want, cases[i].len, "'%s'",
				  cases[i].text);
	}
}

/*
 * A lone surrogate must be refused, not encoded. Emitting one would produce
 * ill-formed UTF-8 and make a CONFIG_NANOPB_VALIDATE_UTF8 build reject a struct
 * this parser had filled.
 */
ZTEST(pb_tf_text, test_lone_surrogate_is_rejected)
{
	static const char *const bad[] = {
		"f_string: \"\\ud800\"",
		"f_string: \"\\uDBFF\"",
		"f_string: \"\\udc00\"",
		"f_string: \"\\uDFFF\"",
		/* A surrogate pair is out of scope, so it fails at the first half. */
		"f_string: \"\\ud83d\\ude00\"",
	};

	ARRAY_FOR_EACH(bad, i) {
		fx_scalars_t m;

		zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, bad[i], NULL),
			      -PB_TF_ERR_SYNTAX, "'%s' must be refused", bad[i]);
	}

	/* The code points either side of the surrogate block are fine. */
	fx_scalars_t ok;

	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &ok, "f_string: \"\\ud7ff\"", NULL),
		      PB_TF_OK);
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &ok, "f_string: \"\\ue000\"", NULL),
		      PB_TF_OK);
}

ZTEST(pb_tf_text, test_malformed_escapes_and_literals)
{
	static const struct {
		const char *text;
		int want;
	} cases[] = {
		/* \U and surrogate pairs are out of scope. */
		{"f_string: \"\\U0001F600\"", -PB_TF_ERR_SYNTAX},
		{"f_string: \"\\q\"", -PB_TF_ERR_SYNTAX},
		{"f_string: \"\\u12\"", -PB_TF_ERR_SYNTAX},
		{"f_string: \"\\u\"", -PB_TF_ERR_SYNTAX},
		{"f_string: \"unterminated", -PB_TF_ERR_SYNTAX},
		{"f_string: 'mismatched\"", -PB_TF_ERR_SYNTAX},
		{"f_string: \"trailing\\", -PB_TF_ERR_SYNTAX},
		/* A raw newline is not valid inside a literal. */
		{"f_string: \"line\nbreak\"", -PB_TF_ERR_SYNTAX},
		{"f_string: \"tab\there\"", -PB_TF_ERR_SYNTAX},
		/* A bare word where a literal belongs. */
		{"f_string: hello", -PB_TF_ERR_TYPE},
		{"f_string: 5", -PB_TF_ERR_TYPE},
	};

	ARRAY_FOR_EACH(cases, i) {
		fx_scalars_t m;

		zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, cases[i].text, NULL),
			      cases[i].want, "'%s'", cases[i].text);
	}
}

/* Adjacent string concatenation is out of scope and must not silently join. */
ZTEST(pb_tf_text, test_adjacent_literals_rejected)
{
	fx_scalars_t m;

	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_string: \"a\" \"b\"", NULL),
		      -PB_TF_ERR_SYNTAX);
}

/* nanopb's bound is size + 1 <= data_size, so data_size - 1 characters fit. */
ZTEST(pb_tf_text, test_string_capacity)
{
	fx_scalars_t m;
	struct pb_tf_status st;
	const size_t cap = sizeof(m.f_string) - 1U;
	int err;

	zassert_equal(cap, 15, "the fixture declares max_size:16");

	err = pb_tf_parse(&fx_scalars_t_tf, &m, "f_string: \"123456789012345\"", &st);
	zassert_equal(err, PB_TF_OK, "%zu characters must fit: %s", cap,
		      pb_tf_strerror(st.code));
	zassert_equal(strlen(m.f_string), cap);

	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_string: \"1234567890123456\"", &st),
		      -PB_TF_ERR_NO_SPACE, "one character past the array must not fit");
	zassert_str_equal(st.field, "f_string");
}

/*
 * The bytes capacity is data_size minus the pb_bytes_array_t header. Derived
 * rather than hardcoded because tail padding can make it exceed max_size.
 */
ZTEST(pb_tf_text, test_bytes_capacity)
{
	fx_scalars_t m;
	struct pb_tf_status st;
	const size_t cap = sizeof(m.f_bytes) - offsetof(pb_bytes_array_t, bytes);
	int err;

	zassert_true(cap >= 8, "the fixture declares max_size:8, got %zu", cap);

	err = pb_tf_parse(&fx_scalars_t_tf, &m, "f_bytes: \"12345678\"", &st);
	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.f_bytes.size, 8, "got %u", m.f_bytes.size);

	if (cap == 8U) {
		zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_bytes: \"123456789\"", &st),
			      -PB_TF_ERR_NO_SPACE);
		zassert_str_equal(st.field, "f_bytes");
	}
}

/*
 * fixed_length_bytes has no count member, so nanopb accepts either exactly
 * data_size bytes or an empty value meaning all zeros.
 */
ZTEST(pb_tf_text, test_fixed_length_bytes)
{
	fx_scalars_t m;
	struct pb_tf_status st;
	int err;

	zassert_equal(sizeof(m.f_fixed_bytes), 4, "the fixture declares max_size:4");

	err = pb_tf_parse(&fx_scalars_t_tf, &m, "f_fixed_bytes: \"abcd\"", &st);
	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_mem_equal(m.f_fixed_bytes, "abcd", 4);

	/* Empty means all zeros, matching pb_decode.c's special case. */
	(void)memset(m.f_fixed_bytes, 0xAA, sizeof(m.f_fixed_bytes));
	err = pb_tf_merge(&fx_scalars_t_tf, &m, "f_fixed_bytes: \"\"", &st);
	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_mem_equal(m.f_fixed_bytes, "\0\0\0\0", 4, "an empty value means all zeros");

	/* Too short is a wrong-shaped literal, not an overflow. */
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_fixed_bytes: \"abc\"", &st),
		      -PB_TF_ERR_TYPE);
	zassert_str_equal(st.field, "f_fixed_bytes");

	/* Too long overflows. */
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_fixed_bytes: \"abcde\"", NULL),
		      -PB_TF_ERR_NO_SPACE);
}

ZTEST(pb_tf_text, test_repeated_string)
{
	fx_shapes_t m;
	struct pb_tf_status st;
	int err = pb_tf_parse(&fx_shapes_t_tf, &m, "rep_string: [\"ab\", \"cd\"]", &st);

	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.rep_string_count, 2, "got %u", m.rep_string_count);
	zassert_str_equal(m.rep_string[0], "ab");
	zassert_str_equal(m.rep_string[1], "cd");

	/* max_count is 2. */
	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "rep_string: [\"a\",\"b\",\"c\"]", NULL),
		      -PB_TF_ERR_TOO_MANY);

	/* Each element is bounded by its own element size, not the whole array. */
	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "rep_string: [\"12345678\"]", &st),
		      -PB_TF_ERR_NO_SPACE, "element capacity is sizeof(rep_string[0]) - 1");
}

ZTEST(pb_tf_text, test_oneof_string_member)
{
	fx_shapes_t m;
	struct pb_tf_status st;
	int err = pb_tf_parse(&fx_shapes_t_tf, &m, "c_str: \"pick\"", &st);

	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.which_choice, FX_SHAPES_C_STR_TAG);
	zassert_str_equal(m.choice.c_str, "pick");

	/* Switching away and back must not leave the old member's bytes behind. */
	err = pb_tf_parse(&fx_shapes_t_tf, &m, "c_str: \"1234567\" c_msg { a: 1 } c_str: \"x\"",
			  &st);
	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.which_choice, FX_SHAPES_C_STR_TAG);
	zassert_str_equal(m.choice.c_str, "x");
}

/* Strings inside a submessage take the same path. */
ZTEST(pb_tf_text, test_string_in_submessage)
{
	fx_shapes_t m;
	struct pb_tf_status st;
	int err =
		pb_tf_parse(&fx_shapes_t_tf, &m, "rep_string: [\"x\"] sing_msg { a: 1 }", &st);

	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_str_equal(m.rep_string[0], "x");
	zassert_equal(m.sing_msg.a, 1);
}

/*
 * ===========================================================================
 * The printer, and round-trip.
 *
 * The style is named per call, so one build asserts every combination rather
 * than needing a twister scenario each. Assertions that do not depend on style
 * still check for substrings, so they hold in every configuration.
 * ===========================================================================
 */

ZTEST_SUITE(pb_tf_print, NULL, NULL, NULL, NULL, NULL);

/* Print into a local array and yield it, in the default readable style. */
#define PRINT_INTO(dst, tf, msg)                                                                   \
	({                                                                                         \
		int _e = pb_tf_print_buf_multiline((tf), (msg), (dst), sizeof(dst));                  \
		zassert_equal(_e, PB_TF_OK, "print: %s", pb_tf_strerror(_e));                      \
		(dst);                                                                             \
	})

#define STYLE_SKIP IS_ENABLED(CONFIG_NANOPB_TEXTFORMAT_PRINT_SKIP_DEFAULTS)

/*
 * fx_scalars carries a field of every ltype, so printing it needs a build that
 * can represent all of them: floats, and integers wider than 32 bits. Tests
 * that print it are about something else (escapes, bools) and simply skip where
 * the message itself is unprintable.
 */
#define SCALARS_PRINTABLE                                                                          \
	(IS_ENABLED(CONFIG_NANOPB_TEXTFORMAT_FLOAT) &&                                             \
	 IS_ENABLED(CONFIG_CBPRINTF_FULL_INTEGRAL) && !IS_ENABLED(CONFIG_NANOPB_WITHOUT_64BIT))

/*
 * The exact bytes of all four combinations, in one build. This is the test that
 * would catch a stray space or a missing newline.
 */
ZTEST(pb_tf_print, test_every_style_exactly)
{
	fx_shapes_t m;
	char out[192];

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "plain_scalar: 1 opt_scalar: 2", NULL),
		      PB_TF_OK);

	if (STYLE_SKIP) {
		/* opt_scalar was set, plain_scalar is a default zero. */
		zassert_equal(pb_tf_print_buf_compact(&fx_shapes_t_tf, &m, out, sizeof(out)),
			      PB_TF_OK);
		zassert_str_equal(out, "opt_scalar: 2");
		return;
	}

	zassert_equal(pb_tf_print_buf_compact(&fx_shapes_t_tf, &m, out, sizeof(out)), PB_TF_OK);
	zassert_str_equal(out, "plain_scalar: 1, opt_scalar: 2");

	zassert_equal(pb_tf_print_buf_multiline(&fx_shapes_t_tf, &m, out, sizeof(out)), PB_TF_OK);
	zassert_str_equal(out, "plain_scalar: 1\nopt_scalar: 2\n");

	zassert_equal(pb_tf_print_buf_compact_braces(&fx_shapes_t_tf, &m, out, sizeof(out)),
		      PB_TF_OK);
	zassert_str_equal(out, "{plain_scalar: 1, opt_scalar: 2}");

	zassert_equal(pb_tf_print_buf_multiline_braces(&fx_shapes_t_tf, &m, out, sizeof(out)),
		      PB_TF_OK);
	zassert_str_equal(out, "{\n  plain_scalar: 1\n  opt_scalar: 2\n}\n");
}

/*
 * The four style-named wrappers are static inline in the public header, so this
 * asserts each one passes the pair of booleans it advertises. A wrapper wired to
 * the wrong flags still compiles and still prints valid text format -- just the
 * wrong shape -- so nothing but a byte comparison against the explicit form
 * catches it.
 */
ZTEST(pb_tf_print, test_wrappers_match_styled)
{
	static const struct {
		bool compact;
		bool braces;
		int (*wrapper)(const struct pb_tf_msg *tf, const void *msg, char *buf, size_t size);
	} cases[] = {
		{true, false, pb_tf_print_buf_compact},
		{false, false, pb_tf_print_buf_multiline},
		{true, true, pb_tf_print_buf_compact_braces},
		{false, true, pb_tf_print_buf_multiline_braces},
	};
	fx_shapes_t m;
	char named[192];
	char styled[192];
	int err;

	err = pb_tf_parse(&fx_shapes_t_tf, &m, "plain_scalar: 1 opt_scalar: 2 opt_msg {a: 3, b: -4}", NULL);
	zassert_equal(err, PB_TF_OK);

	ARRAY_FOR_EACH(cases, i) {
		err = cases[i].wrapper(&fx_shapes_t_tf, &m, named, sizeof(named));
		zassert_equal(err, PB_TF_OK, "wrapper %zu: %s", i, pb_tf_strerror(err));

		err = pb_tf_print_buf_styled(&fx_shapes_t_tf, &m, styled, sizeof(styled),
					     cases[i].compact, cases[i].braces);
		zassert_equal(err, PB_TF_OK, "styled %zu: %s", i, pb_tf_strerror(err));

		zassert_str_equal(named, styled, "wrapper %zu disagrees with styled", i);
	}
}

/* The callback form too, through pb_tf_out_buf() rather than the buffered entry. */
ZTEST(pb_tf_print, test_callback_wrappers_match_styled)
{
	fx_shapes_t m;
	char named[192];
	char styled[192];
	struct pb_tf_buf sink;
	int err;

	err = pb_tf_parse(&fx_shapes_t_tf, &m, "plain_scalar: 1 opt_msg {a: 9}", NULL);
	zassert_equal(err, PB_TF_OK);

	named[0] = '\0';
	sink.buf = named;
	sink.size = sizeof(named);
	sink.used = 0U;
	sink.overflow = false;
	err = pb_tf_print_multiline_braces(&fx_shapes_t_tf, &m, pb_tf_out_buf, &sink);
	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(err));
	zassert_false(sink.overflow);

	styled[0] = '\0';
	sink.buf = styled;
	sink.size = sizeof(styled);
	sink.used = 0U;
	sink.overflow = false;
	err = pb_tf_print_styled(&fx_shapes_t_tf, &m, pb_tf_out_buf, &sink, false, true);
	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(err));
	zassert_false(sink.overflow);

	zassert_str_equal(named, styled);
}

/*
 * A narrow leaf submessage folds onto one line in every style. fx_inner holds
 * only scalars, so it is measured rather than expanded outright, and
 * "sing_msg {a: 1, b: -2}" is 22 columns -- inside any FIT_WIDTH this suite
 * runs at.
 */
ZTEST(pb_tf_print, test_submessage_style)
{
	fx_shapes_t m;
	char out[192];

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "sing_msg { a: 1, b: -2 }", NULL),
		      PB_TF_OK);

	zassert_equal(pb_tf_print_buf_compact(&fx_shapes_t_tf, &m, out, sizeof(out)), PB_TF_OK);
	zassert_not_null(strstr(out, "sing_msg {a: 1, b: -2}"), "got %s", out);

	zassert_equal(pb_tf_print_buf_multiline(&fx_shapes_t_tf, &m, out, sizeof(out)), PB_TF_OK);
	zassert_not_null(strstr(out, "sing_msg {a: 1, b: -2}\n"), "got %s", out);

	/* Braces indent the whole body one level, so everything shifts. */
	zassert_equal(pb_tf_print_buf_multiline_braces(&fx_shapes_t_tf, &m, out, sizeof(out)),
		      PB_TF_OK);
	zassert_not_null(strstr(out, "  sing_msg {a: 1, b: -2}\n"), "got %s", out);
}

/*
 * A chain expands exactly as far as it actually nests, and no further.
 *
 * fx_deep1 and fx_deep2 each declare a submessage field, so both expand
 * whatever their width -- that clause is what keeps the measuring pass flat.
 * fx_deep3 declares only a scalar, so it is a leaf, gets measured, and
 * "d3 {v: 9}" folds. The result is one indent level rather than three, which
 * is the whole difference between this and a rule that propagated downwards.
 */
ZTEST(pb_tf_print, test_chain_expands_only_where_it_nests)
{
	fx_deep1_t m;
	char out[192];

	if (CONFIG_NANOPB_TEXTFORMAT_MAX_DEPTH < 2) {
		ztest_test_skip();
	}

	zassert_equal(pb_tf_parse(&fx_deep1_t_tf, &m, "d2 { d3 { v: 9 } }", NULL), PB_TF_OK);

	zassert_equal(pb_tf_print_buf_multiline(&fx_deep1_t_tf, &m, out, sizeof(out)), PB_TF_OK);
	zassert_not_null(strstr(out, "d2 {\n  d3 {v: 9}\n}\n"), "got %s", out);

	/* Braces shift everything a level, and the indent counts toward the fit. */
	zassert_equal(pb_tf_print_buf_multiline_braces(&fx_deep1_t_tf, &m, out, sizeof(out)),
		      PB_TF_OK);
	zassert_not_null(strstr(out, "  d2 {\n    d3 {v: 9}\n  }\n"), "got %s", out);
}

/* An absent optional prints nothing; a present zero prints even under SKIP. */
ZTEST(pb_tf_print, test_presence_governs_optional)
{
	fx_shapes_t m;
	char out[192];
	const char *s;

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "plain_scalar: 1", NULL), PB_TF_OK);
	s = PRINT_INTO(out, &fx_shapes_t_tf, &m);
	zassert_is_null(strstr(s, "opt_scalar"), "an unset optional must not print: %s", s);

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "opt_scalar: 0", NULL), PB_TF_OK);
	s = PRINT_INTO(out, &fx_shapes_t_tf, &m);
	zassert_not_null(strstr(s, "opt_scalar: 0"),
			 "presence is tracked separately from value, so a set zero prints "
			 "even under SKIP_DEFAULTS: %s", s);
}

/* SKIP_DEFAULTS governs implicit-presence zeros, and only those. */
ZTEST(pb_tf_print, test_skip_defaults_follows_configuration)
{
	fx_shapes_t m;
	char out[192];
	const char *s;

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "opt_scalar: 0", NULL), PB_TF_OK);
	s = PRINT_INTO(out, &fx_shapes_t_tf, &m);

	if (STYLE_SKIP) {
		zassert_is_null(strstr(s, "plain_scalar"), "a default zero must be dropped: %s", s);
	} else {
		zassert_not_null(strstr(s, "plain_scalar: 0"), "print-all keeps it: %s", s);
	}
}

/*
 * A repeated field prints as one bracket list rather than once per element.
 *
 * Both spellings are valid text format and both re-parse, but the list does not
 * repeat the field name, so which element you are reading is visible instead of
 * merely countable -- and the line is shorter.
 */
ZTEST(pb_tf_print, test_repeated_prints_as_list)
{
	fx_shapes_t m;
	char out[192];
	const char *s;

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "rep_scalar: [1,2,3]", NULL),
		      PB_TF_OK);

	s = PRINT_INTO(out, &fx_shapes_t_tf, &m);
	zassert_not_null(strstr(s, "rep_scalar: [1, 2, 3]"), "got %s", s);
	zassert_is_null(strstr(s, "rep_scalar: 1"), "the name must not repeat: %s", s);

	zassert_equal(pb_tf_print_buf_compact(&fx_shapes_t_tf, &m, out, sizeof(out)), PB_TF_OK);
	zassert_not_null(strstr(out, "rep_scalar: [1, 2, 3]"), "got %s", out);
}

/* A repeated field with no elements prints nothing at all, never "rep: []". */
ZTEST(pb_tf_print, test_empty_repeated_prints_nothing)
{
	fx_shapes_t m;
	char out[192];
	const char *s;

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "rep_scalar: []", NULL), PB_TF_OK);

	s = PRINT_INTO(out, &fx_shapes_t_tf, &m);
	zassert_is_null(strstr(s, "rep_scalar"), "an empty list is absent, not empty: %s", s);
}

ZTEST(pb_tf_print, test_bool_is_canonical)
{
	fx_scalars_t m;
	char out[1024];

	if (!SCALARS_PRINTABLE) {
		ztest_test_skip();
	}

	/* Parsed as 't', printed as "true". */
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_bool: t", NULL), PB_TF_OK);
	zassert_not_null(strstr(PRINT_INTO(out, &fx_scalars_t_tf, &m), "f_bool: true"));
}

/*
 * Escapes on output are always three octal digits. A \xHH escape would be
 * ambiguous before a hex digit: "\x0" then 'a' reads back as one byte 0x0A.
 */
ZTEST(pb_tf_print, test_escapes_are_unambiguous)
{
	fx_scalars_t m;
	char out[1024];
	const char *s;

	if (!SCALARS_PRINTABLE) {
		ztest_test_skip();
	}

	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_bytes: \"\\x0a\"", NULL),
		      PB_TF_OK);
	s = PRINT_INTO(out, &fx_scalars_t_tf, &m);
	zassert_not_null(strstr(s, "f_bytes: \"\\012\""), "got %s", s);

	/* A byte needing an escape immediately before a digit still round-trips. */
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_bytes: \"\\x0a\\x31\"", NULL),
		      PB_TF_OK);
	s = PRINT_INTO(out, &fx_scalars_t_tf, &m);
	zassert_not_null(strstr(s, "f_bytes: \"\\0121\""), "got %s", s);

	/* Backslash and the quote character are escaped; the other quote is not. */
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_string: \"a\\\\b\\\"c'd\"", NULL),
		      PB_TF_OK);
	s = PRINT_INTO(out, &fx_scalars_t_tf, &m);
	zassert_not_null(strstr(s, "f_string: \"a\\\\b\\\"c'd\""), "got %s", s);
}

/* Output is pure ASCII, so a UTF-8 payload survives without encoding logic. */
ZTEST(pb_tf_print, test_utf8_payload_is_escaped)
{
	fx_scalars_t m;
	char out[1024];

	if (!SCALARS_PRINTABLE) {
		ztest_test_skip();
	}

	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_string: \"\\u20ac\"", NULL),
		      PB_TF_OK);
	zassert_not_null(strstr(PRINT_INTO(out, &fx_scalars_t_tf, &m),
				"f_string: \"\\342\\202\\254\""));
}

ZTEST(pb_tf_print, test_out_buf_overflow_is_reported)
{
	fx_shapes_t m;
	char tiny[8];
	struct pb_tf_buf b = {.buf = tiny, .size = sizeof(tiny)};

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "plain_scalar: 123456789", NULL),
		      PB_TF_OK);

	/* Truncation is reported on the sink, not as a print error. */
	zassert_equal(pb_tf_print_multiline(&fx_shapes_t_tf, &m, pb_tf_out_buf, &b), PB_TF_OK);
	zassert_true(b.overflow);
	zassert_true(strlen(tiny) < sizeof(tiny));
}

ZTEST(pb_tf_print, test_null_arguments)
{
	/* Zeroed because its address is taken while the call is expected to
	 * reject the arguments and never read it. */
	fx_shapes_t m = {0};
	char out[32];
	struct pb_tf_buf b = {.buf = out, .size = sizeof(out)};

	zassert_equal(pb_tf_print_multiline(NULL, &m, pb_tf_out_buf, &b), -PB_TF_ERR_INTERNAL);
	zassert_equal(pb_tf_print_multiline(&fx_shapes_t_tf, NULL, pb_tf_out_buf, &b), -PB_TF_ERR_INTERNAL);
	zassert_equal(pb_tf_print_multiline(&fx_shapes_t_tf, &m, NULL, &b), -PB_TF_ERR_INTERNAL);
}

/*
 * An empty message prints nothing at all, braces aside -- and that is exactly
 * the case pb_tf_out_buf() alone cannot terminate, since it terminates as it
 * writes and here it is never called. pb_tf_print_buf_multiline() exists for this.
 */
ZTEST(pb_tf_print, test_empty_message)
{
	fx_empty_t m;
	char out[32];

	zassert_equal(pb_tf_parse(&fx_empty_t_tf, &m, "", NULL), PB_TF_OK);

	(void)memset(out, 'X', sizeof(out));
	zassert_equal(pb_tf_print_buf_multiline(&fx_empty_t_tf, &m, out, sizeof(out)), PB_TF_OK);
	zassert_str_equal(out, "", "a message that prints nothing must still terminate");

	(void)memset(out, 'X', sizeof(out));
	zassert_equal(pb_tf_print_buf_compact_braces(&fx_empty_t_tf, &m, out, sizeof(out)),
		      PB_TF_OK);
	zassert_str_equal(out, "{}");

	(void)memset(out, 'X', sizeof(out));
	zassert_equal(pb_tf_print_buf_multiline_braces(&fx_empty_t_tf, &m, out, sizeof(out)),
		      PB_TF_OK);
	zassert_str_equal(out, "{\n}\n");
}

/* The same trap via SKIP_DEFAULTS: every field skipped, so nothing is written. */
ZTEST(pb_tf_print, test_all_defaults_skipped_still_terminates)
{
	fx_shapes_t m;
	char out[64];

	if (!STYLE_SKIP) {
		ztest_test_skip();
	}

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "", NULL), PB_TF_OK);

	(void)memset(out, 'X', sizeof(out));
	zassert_equal(pb_tf_print_buf_multiline(&fx_shapes_t_tf, &m, out, sizeof(out)), PB_TF_OK);
	zassert_str_equal(out, "");
}

/* Truncation comes back as an error rather than a flag to remember to check. */
ZTEST(pb_tf_print, test_print_buf_reports_truncation)
{
	fx_shapes_t m;
	char tiny[8];

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "plain_scalar: 123456789", NULL),
		      PB_TF_OK);

	zassert_equal(pb_tf_print_buf_multiline(&fx_shapes_t_tf, &m, tiny, sizeof(tiny)),
		      -PB_TF_ERR_NO_SPACE);
	zassert_true(strlen(tiny) < sizeof(tiny), "must stay terminated when truncated");

	/* Degenerate sizes are rejected rather than writing anything. */
	zassert_equal(pb_tf_print_buf_multiline(&fx_shapes_t_tf, &m, tiny, 0U), -PB_TF_ERR_INTERNAL);
	zassert_equal(pb_tf_print_buf_multiline(&fx_shapes_t_tf, &m, NULL, 8U), -PB_TF_ERR_INTERNAL);
}

/*
 * The fit threshold, to the byte, either side of the boundary.
 *
 * fx_fit holds one fx_wide, which declares only scalars -- so it is measured
 * rather than expanded outright. Printed it is
 *
 *     leaf {n: 1, pad: "<pad>"}
 *
 * which is 20 columns plus the pad, at no indentation. The expected pad lengths
 * are therefore derived from the configured width rather than hardcoded, so
 * this holds whatever FIT_WIDTH a build picks.
 */
ZTEST(pb_tf_print, test_fit_boundary_exactly)
{
	const size_t frame = sizeof("leaf {n: 1, pad: \"\"}") - 1U;
	fx_fit_t m;
	char out[512];
	char want[512];
	size_t pad;

	if ((size_t)CONFIG_NANOPB_TEXTFORMAT_PRINT_FIT_WIDTH <= frame) {
		/* No pad length can reach the boundary from below. */
		ztest_test_skip();
	}

	pad = (size_t)CONFIG_NANOPB_TEXTFORMAT_PRINT_FIT_WIDTH - frame;
	zassert_true(pad + 1U < sizeof(m.leaf.pad), "pad field too small for this width");

	/* Exactly at the width: folds. */
	pb_tf_clear(&fx_fit_t_tf, &m);
	m.has_leaf = true;
	m.leaf.n = 1U;
	(void)memset(m.leaf.pad, 'x', pad);
	m.leaf.pad[pad] = '\0';

	zassert_equal(pb_tf_print_buf_multiline(&fx_fit_t_tf, &m, out, sizeof(out)), PB_TF_OK);
	(void)snprintk(want, sizeof(want), "leaf {n: 1, pad: \"%s\"}\n", m.leaf.pad);
	zassert_str_equal(out, want, "at exactly %d columns it must fold",
			  CONFIG_NANOPB_TEXTFORMAT_PRINT_FIT_WIDTH);

	/* One column over: breaks apart. */
	m.leaf.pad[pad] = 'x';
	m.leaf.pad[pad + 1U] = '\0';

	zassert_equal(pb_tf_print_buf_multiline(&fx_fit_t_tf, &m, out, sizeof(out)), PB_TF_OK);
	(void)snprintk(want, sizeof(want), "leaf {\n  n: 1\n  pad: \"%s\"\n}\n", m.leaf.pad);
	zassert_str_equal(out, want, "one column over the width it must expand");
}

/*
 * A body expands because its *type* declares a submessage field, not because
 * one is set.
 *
 * That is what keeps the measuring pass flat -- anything measured is a leaf, so
 * there is no recursion and no second frame stack -- and it is visible here:
 * fx_deep2 prints an empty body and still breaks across lines.
 */
ZTEST(pb_tf_print, test_expands_on_descriptor_not_value)
{
	fx_deep1_t m;
	char out[192];

	if (CONFIG_NANOPB_TEXTFORMAT_MAX_DEPTH < 2) {
		ztest_test_skip();
	}

	pb_tf_clear(&fx_deep1_t_tf, &m);
	m.has_d2 = true;

	zassert_equal(pb_tf_print_buf_multiline(&fx_deep1_t_tf, &m, out, sizeof(out)), PB_TF_OK);
	zassert_str_equal(out, "d2 {\n}\n");
}

/* A list of submessages puts one element per line, with each element folded. */
ZTEST(pb_tf_print, test_message_list_expands_with_folded_elements)
{
	fx_shapes_t m;
	char out[256];

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "rep_msg: [{a: 9, b: 1}, {a: 10, b: 2}]",
				  NULL),
		      PB_TF_OK);

	zassert_equal(pb_tf_print_buf_multiline(&fx_shapes_t_tf, &m, out, sizeof(out)), PB_TF_OK);
	zassert_not_null(strstr(out, "rep_msg: [\n  {a: 9, b: 1},\n  {a: 10, b: 2}\n]\n"),
			 "got %s", out);

	/* Compact folds the same list onto one line. */
	zassert_equal(pb_tf_print_buf_compact(&fx_shapes_t_tf, &m, out, sizeof(out)), PB_TF_OK);
	zassert_not_null(strstr(out, "rep_msg: [{a: 9, b: 1}, {a: 10, b: 2}]"), "got %s", out);
}

/* Repeated bytes print as a list, and each element is located by data_size. */
ZTEST(pb_tf_print, test_repeated_bytes_prints_as_list)
{
	fx_shapes_t m;
	char out[192];

	zassert_equal(pb_tf_parse(&fx_shapes_t_tf, &m, "rep_bytes: [\"\\000\\377\", \"ab\"]", NULL),
		      PB_TF_OK);

	zassert_equal(pb_tf_print_buf_compact(&fx_shapes_t_tf, &m, out, sizeof(out)), PB_TF_OK);
	zassert_not_null(strstr(out, "rep_bytes: [\"\\000\\377\", \"ab\"]"), "got %s", out);
}

/*
 * ===========================================================================
 * Round-trip: parse -> print -> parse, compared as structs and as text.
 *
 * The strongest check the suite has. It exercises the vtype table in both
 * directions at once: nanopb's runtime pb_type_t cannot tell a float from a
 * fixed32, so a printer that had lost that distinction would emit a value the
 * parser could not read back.
 *
 * It runs in whatever style the build selected, and testcase.yaml covers the
 * four combinations, so between them every style is round-tripped.
 * ===========================================================================
 */

ZTEST_SUITE(pb_tf_roundtrip, NULL, NULL, NULL, NULL, NULL);

/** parse(text) -> print -> parse -> assert both structs and both texts agree. */
static void roundtrip(const struct pb_tf_msg *tf, void *a, void *b, size_t size, const char *text)
{
	char first[1024];
	char second[1024];
	struct pb_tf_status st;
	int err;

	err = pb_tf_parse(tf, a, text, &st);
	zassert_equal(err, PB_TF_OK, "parse '%s': %s at %u", text, pb_tf_strerror(st.code),
		      st.offset);

	err = pb_tf_print_buf_multiline(tf, a, first, sizeof(first));
	zassert_equal(err, PB_TF_OK, "print: %s", pb_tf_strerror(err));

	err = pb_tf_parse(tf, b, first, &st);
	zassert_equal(err, PB_TF_OK, "reparse '%s': %s at %u", first, pb_tf_strerror(st.code),
		      st.offset);

	zassert_mem_equal(a, b, size, "structs differ after round-trip of '%s' via '%s'", text,
			  first);

	err = pb_tf_print_buf_multiline(tf, b, second, sizeof(second));
	zassert_equal(err, PB_TF_OK, "reprint: %s", pb_tf_strerror(err));
	zassert_str_equal(first, second, "printing is not idempotent");
}

ZTEST(pb_tf_roundtrip, test_every_field_shape)
{
	static const char *const inputs[] = {
		"plain_scalar: 42",
		"opt_scalar: 0",
		"rep_scalar: [1, 2, 3, 4]",
		"rep_scalar: []",
		"sing_msg { a: 1, b: -2 }",
		"opt_msg { a: 9 }",
		"rep_msg: [{a: 1}, {a: 2, b: 3}]",
		"rep_string: [\"ab\", \"cd\"]",
		"c_num: 77",
		"c_str: \"pick\"",
		"c_msg { a: 4 }",
		"plain_scalar: 1 opt_scalar: 2 rep_scalar: [3] sing_msg { a: 4 } c_num: 5",
		/* Bytes that need escaping, adjacent to digits. */
		"rep_string: [\"\\012\\061\"]",
		/*
		 * Repeated bytes: the one repeated shape whose element stride is
		 * a padded pb_bytes_array_t, so locating element 1 exercises
		 * nanopb's data_size rather than the declared max_size.
		 */
		"rep_bytes: [\"\\000\\377\", \"ab\"]",
		"rep_bytes: [\"\"]",
	};

	ARRAY_FOR_EACH(inputs, i) {
		fx_shapes_t a;
		fx_shapes_t b;

		roundtrip(&fx_shapes_t_tf, &a, &b, sizeof(a), inputs[i]);
	}
}

/* proto2 shapes: required, and a submessage behind a callback member. */
ZTEST(pb_tf_roundtrip, test_proto2_shapes)
{
	static const char *const inputs[] = {
		"req_num: 5",
		"req_num: 1 opt_num: 2 rep_num: [3, 4]",
		"req_num: 1 sub { v: 2 }",
	};

	ARRAY_FOR_EACH(inputs, i) {
		fx_p2_t a;
		fx_p2_t b;

		roundtrip(&fx_p2_t_tf, &a, &b, sizeof(a), inputs[i]);
	}
}

ZTEST(pb_tf_roundtrip, test_anonymous_oneof)
{
	fx_anon_t a;
	fx_anon_t b;

	roundtrip(&fx_anon_t_tf, &a, &b, sizeof(a), "keep: 1 p_num: 2");
	roundtrip(&fx_anon_t_tf, &a, &b, sizeof(a), "keep: 1 p_msg { a: 3 }");
}

/* The escaping rule has to survive every byte value, not just the easy ones. */
ZTEST(pb_tf_roundtrip, test_every_byte_value)
{
	fx_shapes_t a;
	fx_shapes_t b;
	char text[64];
	unsigned int v;

	/* v == 0 is skipped: a NUL truncates a char-array string. */
	for (v = 1U; v < 256U; v++) {
		/* Two copies, so an escape running into the next byte would show. */
		(void)snprintf(text, sizeof(text), "rep_string: [\"\\%03o\\%03o\"]", v, v);
		roundtrip(&fx_shapes_t_tf, &a, &b, sizeof(a), text);
	}
}
/*
 * ===========================================================================
 * Milestone 6: floats behind CONFIG_NANOPB_TEXTFORMAT_FLOAT.
 * ===========================================================================
 */

ZTEST_SUITE(pb_tf_float, NULL, NULL, NULL, NULL, NULL);

#if defined(CONFIG_NANOPB_TEXTFORMAT_FLOAT)

ZTEST(pb_tf_float, test_literal_forms)
{
	static const struct {
		const char *text;
		float want;
	} cases[] = {
		{"f_float: 0.0", 0.0f},
		{"f_float: 1.0", 1.0f},
		{"f_float: -1.5", -1.5f},
		{"f_float: .5", 0.5f},
		{"f_float: 1.", 1.0f},
		{"f_float: -2.5e3", -2500.0f},
		{"f_float: 2.5E-3", 0.0025f},
		{"f_float: 1e3", 1000.0f},
		/* The f suffix, which strtod() does not consume itself. */
		{"f_float: 1.5f", 1.5f},
		{"f_float: 1.5F", 1.5f},
		/* A bare integer is a valid float value. */
		{"f_float: 7", 7.0f},
		{"f_float: -7", -7.0f},
	};

	ARRAY_FOR_EACH(cases, i) {
		fx_scalars_t m;
		struct pb_tf_status st;
		int err = pb_tf_parse(&fx_scalars_t_tf, &m, cases[i].text, &st);

		zassert_equal(err, PB_TF_OK, "'%s': %s", cases[i].text, pb_tf_strerror(st.code));
		zassert_equal(m.f_float, cases[i].want, "'%s'", cases[i].text);
	}
}

ZTEST(pb_tf_float, test_double_keeps_full_precision)
{
	fx_scalars_t m;
	struct pb_tf_status st;
	int err = pb_tf_parse(&fx_scalars_t_tf, &m,
				      "f_double: 0.1234567890123456789", &st);

	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_true(m.f_double > 0.1234567890123 && m.f_double < 0.1234567890124,
		     "a double must not be narrowed on the way in");
}

ZTEST(pb_tf_float, test_inf_and_nan_words)
{
	static const char *const infs[] = {
		"f_float: inf", "f_float: Inf", "f_float: INF",
		"f_float: infinity", "f_float: Infinity",
	};
	static const char *const nans[] = {
		"f_float: nan", "f_float: NaN", "f_float: NAN",
	};

	ARRAY_FOR_EACH(infs, i) {
		fx_scalars_t m;

		zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, infs[i], NULL), PB_TF_OK,
			      "'%s'", infs[i]);
		zassert_true(isinf(m.f_float) && (m.f_float > 0.0f), "'%s'", infs[i]);
	}

	ARRAY_FOR_EACH(nans, i) {
		fx_scalars_t m;

		zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, nans[i], NULL), PB_TF_OK,
			      "'%s'", nans[i]);
		zassert_true(isnan(m.f_float), "'%s'", nans[i]);
	}

	fx_scalars_t neg;

	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &neg, "f_float: -inf", NULL), PB_TF_OK);
	zassert_true(isinf(neg.f_float) && (neg.f_float < 0.0f));
}

/*
 * strtod() accepts more than text format does. The grammar is validated
 * independently and strtod()'s endptr required to agree, so these are refused
 * however willing the C library is.
 */
ZTEST(pb_tf_float, test_strtod_is_not_trusted)
{
	static const struct {
		const char *text;
		int want;
	} cases[] = {
		/* Hex float: strtod() would take it, text format has no such literal. */
		{"f_float: 0x1p3", -PB_TF_ERR_TYPE},
		{"f_float: +1.0", -PB_TF_ERR_SYNTAX},
		{"f_float: nan(x)", -PB_TF_ERR_SYNTAX},
		{"f_float: .", -PB_TF_ERR_SYNTAX},
		{"f_float: -", -PB_TF_ERR_SYNTAX},
		{"f_float: hello", -PB_TF_ERR_TYPE},
		{"f_float: \"1.0\"", -PB_TF_ERR_SYNTAX},
	};

	ARRAY_FOR_EACH(cases, i) {
		fx_scalars_t m;

		zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, cases[i].text, NULL),
			      cases[i].want, "'%s'", cases[i].text);
	}
}

/* A typo must not become an infinity. Only the inf/nan words may produce one. */
ZTEST(pb_tf_float, test_overflow_is_refused)
{
	fx_scalars_t m;
	struct pb_tf_status st;

	/* Exceeds FLT_MAX but is a perfectly good double. */
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_float: 1e39", &st),
		      -PB_TF_ERR_RANGE, "narrowing to float must not yield inf");
	zassert_str_equal(st.field, "f_float");

	/* Overflows the double itself: strtod returns inf with ERANGE. */
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_double: 1e309", &st),
		      -PB_TF_ERR_RANGE);
	zassert_str_equal(st.field, "f_double");

	/* At the boundary it still fits. */
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_float: 3.4e38", NULL), PB_TF_OK);

	/* Underflow to zero is left alone, as protoc leaves it. */
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_float: 1e-60", NULL), PB_TF_OK);
	zassert_equal(m.f_float, 0.0f);
}

/* %.9g and %.17g are the shortest precisions that round-trip. */
ZTEST(pb_tf_float, test_print_precision_round_trips)
{
	static const char *const inputs[] = {
		"f_float: 0.1",   "f_float: 1e-30", "f_float: 3.4e38",
		/* 15 significant digits: within what cbprintf can emit. DBL_MAX
		 * needs 17 and is covered separately, below. */
		"f_double: 0.1",  "f_double: 1e-300", "f_double: 0.123456789012345",
		"f_float: inf",   "f_float: -inf",
	};

	ARRAY_FOR_EACH(inputs, i) {
		fx_scalars_t a;
		fx_scalars_t b;
		char text[1024];
		struct pb_tf_status st;
		int err;

		err = pb_tf_parse(&fx_scalars_t_tf, &a, inputs[i], &st);
		zassert_equal(err, PB_TF_OK, "'%s': %s", inputs[i], pb_tf_strerror(st.code));

		err = pb_tf_print_buf_multiline(&fx_scalars_t_tf, &a, text, sizeof(text));
		if (err == -PB_TF_ERR_UNSUPPORTED) {
			/* 64-bit fields under PB_WITHOUT_64BIT; not this test's subject. */
			ztest_test_skip();
		}
		zassert_equal(err, PB_TF_OK, "print '%s': %s", inputs[i], pb_tf_strerror(err));

		err = pb_tf_parse(&fx_scalars_t_tf, &b, text, &st);
		zassert_equal(err, PB_TF_OK, "reparse '%s': %s", text, pb_tf_strerror(st.code));

		/* Compared by value, since NaN never equals itself. */
		zassert_equal(a.f_float, b.f_float, "float lost precision via '%s'", text);
		zassert_equal(a.f_double, b.f_double, "double lost precision via '%s'", text);
	}
}

/*
 * Doubles round-trip to 16 significant digits, not the 17 that IEEE-754 needs in
 * the worst case.
 *
 * The limit is Zephyr's, not this library's: cbprintf_complete.c caps floating
 * point at 16 extracted digits (see its digit_count and the `decimals > 16`
 * clamp), so a %.17g request cannot produce 17. Asking for %.17g is still right
 * -- it is what the value deserves, and it becomes exact if cbprintf ever gains
 * the digit -- but the promise has to be stated as 16.
 *
 * At DBL_MAX the 16-digit form rounds *up* past the maximum, so reparsing it
 * overflows. glibc's strtod reports that as an infinity, which this library
 * refuses (-PB_TF_ERR_RANGE); picolibc's lands back on DBL_MAX and succeeds. Both
 * are accepted here -- what must never happen is a silent infinity.
 */
ZTEST(pb_tf_float, test_double_precision_is_capped_at_16_digits)
{
	fx_scalars_t a;
	fx_scalars_t b;
	char text[1024];
	int err;

	/* 15 digits: comfortably inside the cap, so exact. */
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &a, "f_double: 0.123456789012345", NULL),
		      PB_TF_OK);
	err = pb_tf_print_buf_multiline(&fx_scalars_t_tf, &a, text, sizeof(text));
	if (err == -PB_TF_ERR_UNSUPPORTED) {
		ztest_test_skip();
	}
	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(err));
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &b, text, NULL), PB_TF_OK);
	zassert_equal(a.f_double, b.f_double, "15 digits must survive: '%s'", text);

	/* DBL_MAX needs 17 and therefore does not. */
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &a, "f_double: 1.7976931348623157e308",
				  NULL),
		      PB_TF_OK);
	err = pb_tf_print_buf_multiline(&fx_scalars_t_tf, &a, text, sizeof(text));
	zassert_equal(err, PB_TF_OK, "printing DBL_MAX must still succeed: %s",
		      pb_tf_strerror(err));

	err = pb_tf_parse(&fx_scalars_t_tf, &b, text, NULL);
	zassert_true((err == PB_TF_OK) || (err == -PB_TF_ERR_RANGE), "got %s for '%s'",
		     pb_tf_strerror(err), text);
	if (err == PB_TF_OK) {
		zassert_true(isfinite(b.f_double), "a rounded DBL_MAX must not become inf");
	}
}

/* NaN needs its own check: it is never equal to itself, so memcmp is the test. */
ZTEST(pb_tf_float, test_nan_round_trips)
{
	fx_scalars_t a;
	fx_scalars_t b;
	char text[1024];
	int err;

	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &a, "f_float: nan", NULL), PB_TF_OK);

	err = pb_tf_print_buf_multiline(&fx_scalars_t_tf, &a, text, sizeof(text));
	if (err == -PB_TF_ERR_UNSUPPORTED) {
		ztest_test_skip();
	}
	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(err));

	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &b, text, NULL), PB_TF_OK,
		      "printed NaN must read back: '%s'", text);
	zassert_true(isnan(b.f_float), "got '%s'", text);
}

/*
 * The whole of fx_scalars: a field of every ltype in one message, both
 * directions. It lives in the float suite because the float fields are what
 * make it need a build that can represent everything -- see SCALARS_PRINTABLE.
 */
ZTEST(pb_tf_float, test_full_scalars_round_trip)
{
	fx_scalars_t a;
	fx_scalars_t b;

	if (IS_ENABLED(CONFIG_NANOPB_WITHOUT_64BIT) ||
	    !IS_ENABLED(CONFIG_CBPRINTF_FULL_INTEGRAL)) {
		/*
		 * Two independent reasons an 8-byte field is unrepresentable
		 * here: PB_WITHOUT_64BIT narrows the codec, and reduced integral
		 * conversion cannot print wider than 32 bits. Narrower fields
		 * are unaffected either way, which is the point of using
		 * width-correct conversions.
		 */
		ztest_test_skip();
	}

	roundtrip(&fx_scalars_t_tf, &a, &b, sizeof(a),
		  "f_int32: -1 f_int64: -2 f_uint32: 3 f_uint64: 4 f_sint32: -5 f_sint64: -6 "
		  "f_fixed32: 7 f_fixed64: 8 f_sfixed32: -9 f_sfixed64: -10 f_bool: true "
		  "f_float: 1.5 f_double: 2.25 f_uenum: 2 f_enum: -1 f_string: \"hi\" "
		  "f_bytes: \"\\001\\002\" f_fixed_bytes: \"abcd\" f_narrow: 127 "
		  "f_narrow32: 2147483647");

	roundtrip(&fx_scalars_t_tf, &a, &b, sizeof(a), "f_float: 0.1 f_double: 0.1");
}

#else /* !CONFIG_NANOPB_TEXTFORMAT_FLOAT */

/* With float support off, both directions report it rather than failing to build. */
ZTEST(pb_tf_float, test_degrades_cleanly)
{
	fx_scalars_t m;
	struct pb_tf_status st;
	char out[1024];

	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_float: 1.5", &st),
		      -PB_TF_ERR_NO_FLOAT);
	zassert_str_equal(st.field, "f_float");

	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_double: 1.5", &st),
		      -PB_TF_ERR_NO_FLOAT);

	/* Integer fields in the same message are unaffected. */
	zassert_equal(pb_tf_parse(&fx_scalars_t_tf, &m, "f_int32: 5", &st), PB_TF_OK);
	zassert_equal(m.f_int32, 5);

	/* Printing a message that has a float field reports it too. */
	zassert_equal(pb_tf_print_buf_multiline(&fx_scalars_t_tf, &m, out, sizeof(out)),
		      -PB_TF_ERR_NO_FLOAT);
}

#endif /* CONFIG_NANOPB_TEXTFORMAT_FLOAT */

/*
 * ===========================================================================
 * Naming styles: the same shapes under nanopb's default naming.
 * ===========================================================================
 */

/*
 * Under --c-style the FIELDLIST prefix and the struct type differ; under the
 * default naming they are one token. That this compiles at all is the substance
 * of the test -- the descriptor, its forward declarations and its submessage
 * _MSGTYPE lookups all have to resolve from the same pair of macro arguments.
 */
ZTEST(pb_tf_macro, test_default_naming_style)
{
	const struct pb_tf_field *f;

	zassert_equal_ptr(FxDef_tf.desc, &FxDef_msg);
	zassert_equal(FxDef_tf.count, 7, "got %u", FxDef_tf.count);
	zassert_equal(FxDef_tf.struct_size, sizeof(FxDef));

	/* Field names are unaffected by the naming style. */
	f = pb_tf_field_find(&FxDef_tf, "plain", 5);
	zassert_not_null(f);
	zassert_equal(f->tag, FxDef_plain_tag);

	/* The submessage descriptor resolved through <struct>_<field>_MSGTYPE. */
	f = pb_tf_field_find(&FxDef_tf, "sub", 3);
	zassert_not_null(f);
	zassert_equal_ptr(f->sub, &FxDefInner_tf, "submessage lookup must be naming-agnostic");

	/* Including for a oneof member, whose symbol adds the union name. */
	f = pb_tf_field_find(&FxDef_tf, "p_msg", 5);
	zassert_not_null(f);
	zassert_equal_ptr(f->sub, &FxDefInner_tf);
}

ZTEST(pb_tf_parse, test_default_naming_style_parses)
{
	FxDef m;
	struct pb_tf_status st;
	int err = pb_tf_parse(&FxDef_tf, &m,
				      "plain: 1 opt: 2 rep: [3, 4] sub { a: 5 } text: \"hi\"",
				      &st);

	zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));
	zassert_equal(m.plain, 1);
	zassert_true(m.has_opt);
	zassert_equal(m.rep_count, 2);
	zassert_equal(m.sub.a, 5);
	zassert_str_equal(m.text, "hi");
}

ZTEST(pb_tf_roundtrip, test_default_naming_style)
{
	FxDef a;
	FxDef b;

	roundtrip(&FxDef_tf, &a, &b, sizeof(a), "plain: 1 opt: 2 rep: [3] sub { a: 4, b: -5 } "
					        "text: \"x\" p_num: 6");
	roundtrip(&FxDef_tf, &a, &b, sizeof(a), "plain: 9 p_msg { a: 1 }");
}
