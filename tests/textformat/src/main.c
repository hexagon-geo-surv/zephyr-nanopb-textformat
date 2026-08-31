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

/* The tables must agree with nanopb's own field counts. */
BUILD_ASSERT(PB_TF_FIELD_COUNT(FX_SCALARS_FIELDLIST) == 20);
BUILD_ASSERT(PB_TF_FIELD_COUNT(FX_SHAPES_FIELDLIST) == 11);
BUILD_ASSERT(PB_TF_FIELD_COUNT(FX_INNER_FIELDLIST) == 2);
BUILD_ASSERT(PB_TF_FIELD_COUNT(FX_EMPTY_FIELDLIST) == 0);

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
