/*
 * Copyright (c) 2026 Rodrigo Peixoto
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Text-format printer, driven by cbprintf().
 *
 * Output goes out character by character through the caller's cbprintf_cb, so
 * there is no length limit and no temporary buffer anywhere in the path.
 *
 * Like the parser, submessages are walked with an explicit frame stack rather
 * than by recursion -- see nanopb_textformat_parse.c for the measurements that
 * motivated it. The printer is the less exposed of the two, since its nesting
 * depth is fixed by the .proto at build time rather than by input, but a second
 * stack-growth mechanism is not worth keeping for symmetry's sake.
 */

#include <stdarg.h>
#include <string.h>

#include <pb_common.h>

#include "nanopb_textformat_priv.h"

/** Nothing has been emitted in this body yet, so no separator is due. */
#define PB_TF_PF_FIRST 0x01U

/** One open message body. */
struct pb_tf_pframe {
	const pb_tf_msg_t *tf;
	const void *msg;
	/** Next row to visit. */
	uint16_t index;
	/** Next element to visit, for a repeated row. */
	pb_size_t elem;
	/** PB_TF_PF_* */
	uint8_t flags;
};

struct pb_tf_printer {
	cbprintf_cb out;
	void *ctx;
	unsigned int flags;
	/** Indent applied to the outermost body: 1 when wrapped in braces. */
	unsigned int base;
	unsigned int top;
	struct pb_tf_pframe stack[PB_TF_MAX_DEPTH + 1U];
};

/*
 * ---------------------------------------------------------------------------
 * Output
 *
 * A cbprintf_cb signals nothing useful on failure -- pb_tf_out_buf() reports
 * truncation by setting a flag on its own context, which is the documented
 * contract -- so these ignore the callback's return and let the sink decide what
 * overflow means. A negative return from cbvprintf() is a real formatting
 * failure and does propagate.
 * ---------------------------------------------------------------------------
 */

static void pb_tf_put(struct pb_tf_printer *pr, char c)
{
	(void)pr->out((int)(unsigned char)c, pr->ctx);
}

static pb_tf_err_t pb_tf_emit(struct pb_tf_printer *pr, const char *fmt, ...)
{
	va_list ap;
	int rc;

	va_start(ap, fmt);
	rc = cbvprintf(pr->out, pr->ctx, fmt, ap);
	va_end(ap);

	return (rc < 0) ? PB_TF_ERR_INTERNAL : PB_TF_OK;
}

static bool pb_tf_compact(const struct pb_tf_printer *pr)
{
	return (pr->flags & PB_TF_COMPACT) != 0U;
}

static void pb_tf_indent(struct pb_tf_printer *pr)
{
	unsigned int n = (pr->base + pr->top) * 2U;
	unsigned int i;

	for (i = 0U; i < n; i++) {
		pb_tf_put(pr, ' ');
	}
}

/**
 * Emit whatever must precede the next field of the current body.
 *
 * Multi-line terminates every field with a newline, so a field only needs its
 * indent. Compact joins fields with ", " and therefore needs to know whether
 * anything came before it.
 */
static void pb_tf_field_prefix(struct pb_tf_printer *pr, struct pb_tf_pframe *fr)
{
	if (pb_tf_compact(pr)) {
		if ((fr->flags & PB_TF_PF_FIRST) == 0U) {
			pb_tf_put(pr, ',');
			pb_tf_put(pr, ' ');
		}
	} else {
		pb_tf_indent(pr);
	}

	fr->flags &= (uint8_t)~PB_TF_PF_FIRST;
}

static void pb_tf_field_suffix(struct pb_tf_printer *pr)
{
	if (!pb_tf_compact(pr)) {
		pb_tf_put(pr, '\n');
	}
}

/*
 * ---------------------------------------------------------------------------
 * Values
 * ---------------------------------------------------------------------------
 */

/**
 * Emit a quoted literal.
 *
 * Anything outside printable ASCII becomes a **three-digit** octal escape. The
 * fixed width is the point: \\xHH would be ambiguous before a hex digit, since
 * "\\x0" followed by 'a' reads back as one byte 0x0A rather than two. Three octal
 * digits cannot run on, so the output re-parses byte for byte -- by this library
 * and by protoc. It also keeps output pure ASCII, so a UTF-8 payload survives
 * round-trip without the printer needing to understand encodings.
 */
static void pb_tf_quoted(struct pb_tf_printer *pr, const pb_byte_t *b, size_t len)
{
	size_t i;

	pb_tf_put(pr, '"');

	for (i = 0U; i < len; i++) {
		unsigned char c = (unsigned char)b[i];

		if ((c == (unsigned char)'\\') || (c == (unsigned char)'"')) {
			pb_tf_put(pr, '\\');
			pb_tf_put(pr, (char)c);
		} else if ((c >= 0x20U) && (c < 0x7FU)) {
			pb_tf_put(pr, (char)c);
		} else {
			(void)pb_tf_emit(pr, "\\%03o", (unsigned int)c);
		}
	}

	pb_tf_put(pr, '"');
}

/** True for the vtypes whose C storage is signed. Mirrors the parser's table. */
static bool pb_tf_print_is_signed(uint8_t vtype)
{
	switch (vtype) {
	case PB_TF_VT_INT32:
	case PB_TF_VT_INT64:
	case PB_TF_VT_SINT32:
	case PB_TF_VT_SINT64:
	case PB_TF_VT_SFIXED32:
	case PB_TF_VT_SFIXED64:
	case PB_TF_VT_ENUM:
		return true;
	default:
		return false;
	}
}

/**
 * Read an integer of @p data_size and print it.
 *
 * The width chain mirrors the parser's, which mirrors nanopb's. Note this is
 * where the row's vtype earns its keep on the way out too: nanopb's runtime
 * pb_type_t cannot distinguish a float from a fixed32, so a printer driven by
 * the iterator alone could not tell whether to write 1.0 or 1.
 */
static pb_tf_err_t pb_tf_print_int(struct pb_tf_printer *pr, const pb_tf_field_t *row,
				   pb_size_t data_size, const void *src)
{
	if (data_size > sizeof(pb_tf_uint64_t)) {
		/* An int64 field under PB_WITHOUT_64BIT; see the parser. */
		return PB_TF_ERR_UNSUPPORTED;
	}

	if (pb_tf_print_is_signed(row->vtype)) {
		long long v;

		if (data_size == sizeof(pb_tf_int64_t)) {
			v = (long long)*(const pb_tf_int64_t *)src;
		} else if (data_size == sizeof(int32_t)) {
			v = (long long)*(const int32_t *)src;
		} else if (data_size == sizeof(int_least16_t)) {
			v = (long long)*(const int_least16_t *)src;
		} else if (data_size == sizeof(int_least8_t)) {
			v = (long long)*(const int_least8_t *)src;
		} else {
			return PB_TF_ERR_INTERNAL;
		}

		return pb_tf_emit(pr, "%lld", v);
	}

	unsigned long long v;

	if (data_size == sizeof(pb_tf_uint64_t)) {
		v = (unsigned long long)*(const pb_tf_uint64_t *)src;
	} else if (data_size == sizeof(uint32_t)) {
		v = (unsigned long long)*(const uint32_t *)src;
	} else if (data_size == sizeof(uint_least16_t)) {
		v = (unsigned long long)*(const uint_least16_t *)src;
	} else if (data_size == sizeof(uint_least8_t)) {
		v = (unsigned long long)*(const uint_least8_t *)src;
	} else {
		return PB_TF_ERR_INTERNAL;
	}

	return pb_tf_emit(pr, "%llu", v);
}

static pb_tf_err_t pb_tf_print_scalar(struct pb_tf_printer *pr, const pb_tf_field_t *row,
				      pb_size_t data_size, const void *src)
{
	switch (row->vtype) {
	case PB_TF_VT_BOOL:
		if (data_size != sizeof(bool)) {
			return PB_TF_ERR_INTERNAL;
		}
		/* Output is always the canonical spelling, whatever was parsed. */
		return pb_tf_emit(pr, "%s", *(const bool *)src ? "true" : "false");

	case PB_TF_VT_ENUM:
	case PB_TF_VT_UENUM:
	case PB_TF_VT_INT32:
	case PB_TF_VT_INT64:
	case PB_TF_VT_UINT32:
	case PB_TF_VT_UINT64:
	case PB_TF_VT_SINT32:
	case PB_TF_VT_SINT64:
	case PB_TF_VT_FIXED32:
	case PB_TF_VT_FIXED64:
	case PB_TF_VT_SFIXED32:
	case PB_TF_VT_SFIXED64:
		return pb_tf_print_int(pr, row, data_size, src);

	case PB_TF_VT_FLOAT:
		if (!IS_ENABLED(CONFIG_NANOPB_TEXTFORMAT_FLOAT)) {
			return PB_TF_ERR_NO_FLOAT;
		}
		/*
		 * The precision is not cosmetic: %.9g is the shortest that round
		 * trips every float, and %.17g every double. Too few digits
		 * silently loses a value that then fails to compare equal.
		 */
		return pb_tf_emit(pr, "%.9g", (double)*(const float *)src);

	case PB_TF_VT_DOUBLE:
		if (!IS_ENABLED(CONFIG_NANOPB_TEXTFORMAT_FLOAT)) {
			return PB_TF_ERR_NO_FLOAT;
		}
		if (data_size == sizeof(float)) {
			/* PB_CONVERT_DOUBLE_FLOAT stores a proto double as a float. */
			return pb_tf_emit(pr, "%.9g", (double)*(const float *)src);
		}
		return pb_tf_emit(pr, "%.17g", *(const double *)src);

	case PB_TF_VT_STRING:
		pb_tf_quoted(pr, (const pb_byte_t *)src, strnlen((const char *)src, data_size));
		return PB_TF_OK;

	case PB_TF_VT_BYTES: {
		const pb_bytes_array_t *arr = src;

		pb_tf_quoted(pr, arr->bytes, (size_t)arr->size);
		return PB_TF_OK;
	}

	case PB_TF_VT_FIXED_LENGTH_BYTES:
		pb_tf_quoted(pr, src, (size_t)data_size);
		return PB_TF_OK;

	default:
		return PB_TF_ERR_INTERNAL;
	}
}

/**
 * True if this occurrence is at its type's default and PB_TF_SKIP_DEFAULTS
 * says to omit it.
 *
 * Only implicit-presence fields are eligible: an optional field is already
 * governed by its has_ bit, and a repeated one by its count.
 */
static bool pb_tf_skip_default(const struct pb_tf_printer *pr, const pb_tf_field_t *row,
			       pb_size_t data_size, const void *src)
{
	if ((pr->flags & PB_TF_SKIP_DEFAULTS) == 0U) {
		return false;
	}

	if ((PB_TF_HT(row->rules) != PB_TF_HT_SINGULAR) &&
	    (PB_TF_HT(row->rules) != PB_TF_HT_REQUIRED)) {
		return false;
	}

	switch (row->vtype) {
	case PB_TF_VT_STRING:
		return *(const char *)src == '\0';

	case PB_TF_VT_BYTES:
		return ((const pb_bytes_array_t *)src)->size == 0U;

	default: {
		/*
		 * All-zero bytes covers every numeric, bool, fixed-length bytes,
		 * and a submessage all of whose fields are themselves default.
		 */
		const uint8_t *p = src;
		pb_size_t i;

		for (i = 0U; i < data_size; i++) {
			if (p[i] != 0U) {
				return false;
			}
		}
		return true;
	}
	}
}

/*
 * ---------------------------------------------------------------------------
 * Walk
 * ---------------------------------------------------------------------------
 */

/**
 * Locate occurrence @p elem of @p row and decide whether it is to be printed.
 *
 * Returns the source address in @p src, or NULL in @p src when the occurrence
 * is absent -- an unset optional, an unselected oneof member, or past the end
 * of a repeated field.
 *
 * The iterator is resolved per occurrence rather than advanced in lockstep with
 * the row table. Lockstep would save a short scan, but it would also mean one
 * pb_field_iter_t (~40 bytes) per open frame, which is 360 bytes at the default
 * depth and 1.4 KB at the maximum -- the wrong trade for messages that have a
 * couple of dozen fields at most.
 */
static pb_tf_err_t pb_tf_locate(const struct pb_tf_pframe *fr, const pb_tf_field_t *row,
				pb_size_t elem, const void **src, pb_size_t *data_size,
				pb_size_t *count)
{
	pb_field_iter_t it;

	*src = NULL;
	*count = 1U;

	/* pb_field_iter_begin() reports false for a message with no fields. */
	if (!pb_field_iter_begin(&it, fr->tf->desc, (void *)fr->msg) ||
	    !pb_field_iter_find(&it, row->tag)) {
		return PB_TF_ERR_INTERNAL;
	}

	*data_size = it.data_size;

	switch (PB_TF_HT(row->rules)) {
	case PB_TF_HT_REQUIRED:
	case PB_TF_HT_SINGULAR:
		*src = it.pData;
		break;

	case PB_TF_HT_OPTIONAL:
		if ((it.pSize != NULL) && *(const bool *)it.pSize) {
			*src = it.pData;
		}
		break;

	case PB_TF_HT_REPEATED:
		if (it.pSize == NULL) {
			return PB_TF_ERR_INTERNAL;
		}
		*count = *(const pb_size_t *)it.pSize;
		if (elem < *count) {
			*src = (const char *)it.pData + ((size_t)elem * (size_t)it.data_size);
		}
		break;

	case PB_TF_HT_ONEOF:
		if ((it.pSize != NULL) && (*(const pb_size_t *)it.pSize == (pb_size_t)row->tag)) {
			*src = it.pData;
		}
		break;

	default:
		/* FIXARRAY: skipped rather than printed, see pb_tf_run_print(). */
		break;
	}

	return PB_TF_OK;
}

/** Advance a frame's cursor past the occurrence just handled. */
static void pb_tf_advance(struct pb_tf_pframe *fr, const pb_tf_field_t *row, pb_size_t count)
{
	if (PB_TF_HT(row->rules) == PB_TF_HT_REPEATED) {
		fr->elem++;
		if (fr->elem >= count) {
			fr->index++;
			fr->elem = 0U;
		}
		return;
	}

	fr->index++;
	fr->elem = 0U;
}

static pb_tf_err_t pb_tf_run_print(struct pb_tf_printer *pr)
{
	while (true) {
		struct pb_tf_pframe *fr = &pr->stack[pr->top];
		const pb_tf_field_t *row;
		const void *src;
		pb_size_t data_size = 0U;
		pb_size_t count = 1U;
		pb_tf_err_t err;

		if (fr->index >= fr->tf->count) {
			if (pr->top == 0U) {
				return PB_TF_OK;
			}

			/* Close this body; the parent's cursor already moved on. */
			pr->top--;
			if (!pb_tf_compact(pr)) {
				pb_tf_indent(pr);
			} else {
				pb_tf_put(pr, ' ');
			}
			pb_tf_put(pr, '}');
			pb_tf_field_suffix(pr);
			continue;
		}

		row = &fr->tf->fields[fr->index];

		/*
		 * Shapes this library cannot represent are skipped rather than
		 * failed. Refusing would make printing a message useless because
		 * one field is a pointer, which is worse than an incomplete view;
		 * the omission is documented.
		 */
		if (!pb_tf_shape_supported(row)) {
			fr->index++;
			fr->elem = 0U;
			continue;
		}

		err = pb_tf_locate(fr, row, fr->elem, &src, &data_size, &count);
		if (err != PB_TF_OK) {
			return err;
		}

		if ((src == NULL) || pb_tf_skip_default(pr, row, data_size, src)) {
			/* Absent, or a default being skipped. */
			fr->index++;
			fr->elem = 0U;
			continue;
		}

		if (pb_tf_vtype_is_msg(row->vtype)) {
			if (row->sub == NULL) {
				return PB_TF_ERR_INTERNAL;
			}
			if (pr->top >= (unsigned int)PB_TF_MAX_DEPTH) {
				return PB_TF_ERR_DEPTH;
			}

			pb_tf_field_prefix(pr, fr);
			err = pb_tf_emit(pr, "%s {", row->name);
			if (err != PB_TF_OK) {
				return err;
			}
			if (!pb_tf_compact(pr)) {
				pb_tf_put(pr, '\n');
			}

			/* Advance before descending, so the pop needs no fixup. */
			pb_tf_advance(fr, row, count);

			pr->top++;
			pr->stack[pr->top].tf = row->sub;
			pr->stack[pr->top].msg = src;
			pr->stack[pr->top].index = 0U;
			pr->stack[pr->top].elem = 0U;
			pr->stack[pr->top].flags = PB_TF_PF_FIRST;
			continue;
		}

		pb_tf_field_prefix(pr, fr);
		err = pb_tf_emit(pr, "%s: ", row->name);
		if (err != PB_TF_OK) {
			return err;
		}

		err = pb_tf_print_scalar(pr, row, data_size, src);
		if (err != PB_TF_OK) {
			return err;
		}

		pb_tf_field_suffix(pr);
		pb_tf_advance(fr, row, count);
	}
}

pb_tf_err_t pb_tf_print(const pb_tf_msg_t *tf, const void *msg, cbprintf_cb out, void *ctx,
			unsigned int flags)
{
	struct pb_tf_printer pr;
	pb_tf_err_t err;

	if ((tf == NULL) || (msg == NULL) || (out == NULL)) {
		return PB_TF_ERR_INTERNAL;
	}

	pr.out = out;
	pr.ctx = ctx;
	pr.flags = flags;
	pr.base = ((flags & PB_TF_BRACES) != 0U) ? 1U : 0U;
	pr.top = 0U;

	pr.stack[0].tf = tf;
	pr.stack[0].msg = msg;
	pr.stack[0].index = 0U;
	pr.stack[0].elem = 0U;
	pr.stack[0].flags = PB_TF_PF_FIRST;

	if ((flags & PB_TF_BRACES) != 0U) {
		pb_tf_put(&pr, '{');
		if (!pb_tf_compact(&pr)) {
			pb_tf_put(&pr, '\n');
		}
	}

	err = pb_tf_run_print(&pr);
	if (err != PB_TF_OK) {
		return err;
	}

	if ((flags & PB_TF_BRACES) != 0U) {
		pb_tf_put(&pr, '}');
		if (!pb_tf_compact(&pr)) {
			pb_tf_put(&pr, '\n');
		}
	}

	return PB_TF_OK;
}

pb_tf_err_t pb_tf_print_buf(const pb_tf_msg_t *tf, const void *msg, char *buf, size_t size,
			    unsigned int flags)
{
	pb_tf_buf_t sink;
	pb_tf_err_t err;

	if ((buf == NULL) || (size == 0U)) {
		return PB_TF_ERR_INTERNAL;
	}

	/*
	 * Terminate up front. pb_tf_out_buf() terminates as it writes, so a
	 * message that prints nothing would otherwise leave buf untouched.
	 */
	buf[0] = '\0';

	sink.buf = buf;
	sink.size = size;
	sink.used = 0U;
	sink.overflow = false;

	err = pb_tf_print(tf, msg, pb_tf_out_buf, &sink, flags);
	if (err != PB_TF_OK) {
		return err;
	}

	return sink.overflow ? PB_TF_ERR_NO_SPACE : PB_TF_OK;
}
