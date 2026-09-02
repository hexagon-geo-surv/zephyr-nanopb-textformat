/*
 * Copyright (c) 2026 Rodrigo Peixoto
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "nanopb_textformat_priv.h"

const struct pb_tf_field *pb_tf_field_find(const struct pb_tf_msg *tf, const char *name, size_t len)
{
	if ((tf == NULL) || (name == NULL) || (len == 0U)) {
		return NULL;
	}

	for (uint16_t i = 0U; i < tf->count; i++) {
		const struct pb_tf_field *f = &tf->fields[i];

		if ((f->name == NULL) || (f->vtype == PB_TF_VT_EXTENSION)) {
			continue;
		}

		if ((strncmp(f->name, name, len) == 0) && (f->name[len] == '\0')) {
			return f;
		}
	}

	return NULL;
}

bool pb_tf_vtype_is_msg(uint8_t vtype)
{
	return (vtype == PB_TF_VT_MESSAGE) || (vtype == PB_TF_VT_MSG_W_CB);
}

bool pb_tf_shape_supported(const struct pb_tf_field *f)
{
	if (PB_TF_AT(f->rules) != PB_TF_AT_STATIC) {
		return false;
	}

	if (PB_TF_HT(f->rules) == PB_TF_HT_FIXARRAY) {
		return false;
	}

	return f->vtype != PB_TF_VT_EXTENSION;
}

void pb_tf_clear(const struct pb_tf_msg *tf, void *msg)
{
	if ((tf != NULL) && (msg != NULL)) {
		(void)memset(msg, 0, tf->struct_size);
	}
}

const char *pb_tf_strerror(int err)
{
	/*
	 * Accepts either sign, since the functions return a negated code but the
	 * enumerators themselves are positive. A caller can pass whichever it
	 * has -- a return value, or status.code -- without thinking about it.
	 */
	if (err < 0) {
		err = -err;
	}

	switch ((enum pb_tf_err)err) {
	case PB_TF_OK:
		return "ok";
	case PB_TF_ERR_SYNTAX:
		return "syntax error";
	case PB_TF_ERR_UNKNOWN_FIELD:
		return "no such field";
	case PB_TF_ERR_TYPE:
		return "wrong value type";
	case PB_TF_ERR_RANGE:
		return "value out of range";
	case PB_TF_ERR_TOO_MANY:
		return "too many values";
	case PB_TF_ERR_NO_SPACE:
		return "value too long";
	case PB_TF_ERR_DEPTH:
		return "nesting too deep";
	case PB_TF_ERR_UNSUPPORTED:
		return "unsupported field shape";
	case PB_TF_ERR_NO_FLOAT:
		return "float support disabled";
	case PB_TF_ERR_INTERNAL:
		return "internal error";
	default:
		return "unknown error";
	}
}

int pb_tf_out_buf(int c, void *ctx)
{
	struct pb_tf_buf *b = ctx;

	/* Keep one byte for the terminator at all times. */
	if ((b->buf == NULL) || (b->size == 0U)) {
		b->overflow = true;
		return c;
	}

	if (b->used + 1U >= b->size) {
		b->overflow = true;
		b->buf[b->size - 1U] = '\0';
		return c;
	}

	b->buf[b->used++] = (char)c;
	b->buf[b->used] = '\0';

	return c;
}
