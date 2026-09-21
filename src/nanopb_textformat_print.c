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

#include <inttypes.h>
#include <stdarg.h>
#include <string.h>

#include <pb_common.h>

#include "nanopb_textformat_priv.h"

/** Nothing has been emitted in this body yet, so no separator is due. */
#define PB_TF_PF_FIRST 0x01U

/** This body renders on one line, even though the enclosing style is not. */
#define PB_TF_PF_INLINE 0x02U

/** A "name: [" is open on this frame and still collecting elements. */
#define PB_TF_PF_LIST_OPEN 0x04U

/** That open list puts each element on its own line. */
#define PB_TF_PF_LIST_EXPAND 0x08U

/** One open message body. */
struct pb_tf_pframe {
	const struct pb_tf_msg *tf;
	const void *msg;
	/** Next row to visit. */
	uint16_t index;
	/** Next element to visit, for a repeated row. */
	pb_size_t elem;
	/**
	 * Indentation level of this body's fields.
	 *
	 * Not derived from the stack depth, which would be one short inside an
	 * expanded list: the list's extra level lives on the *parent* frame, so
	 * an element that breaks across lines has to carry it down. Fits in the
	 * padding the struct already had, so a level still costs 16 bytes.
	 */
	uint8_t level;
	/** PB_TF_PF_* */
	uint8_t flags;
};

struct pb_tf_printer {
	cbprintf_cb out;
	void *ctx;
	/**
	 * Single line rather than one field per line.
	 *
	 * Carried here rather than read from Kconfig at each use, because an
	 * application may want both: multi-line for a person reading a
	 * configuration, compact for a line it means to paste or log, and it may
	 * not know which until run time.
	 */
	bool compact;
	/** Wrap the whole message in braces. */
	bool braces;
	/** Indent applied to the outermost body: 1 when wrapped in braces. */
	unsigned int base;
	unsigned int top;
	struct pb_tf_pframe stack[PB_TF_MAX_DEPTH + 1U];
};

/*
 * The line style and the braces are named by the caller, so they are not
 * Kconfig. Skipping defaults still is: unlike the style, it is not something
 * one firmware plausibly wants both ways, and making it per-call would mean a
 * third boolean on every entry point.
 */
#define PB_TF_SKIPS_DEFAULTS IS_ENABLED(CONFIG_NANOPB_TEXTFORMAT_PRINT_SKIP_DEFAULTS)

/*
 * Width below which a submessage or a list stays on one line. Not in
 * nanopb_textformat_priv.h: that header is shared with the parser, and neither
 * symbol exists in a build with the printer, or just the fit pass, left out.
 */
#if defined(CONFIG_NANOPB_TEXTFORMAT_PRINT_FIT)
#define PB_TF_FIT_WIDTH CONFIG_NANOPB_TEXTFORMAT_PRINT_FIT_WIDTH
#endif

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

static int pb_tf_emit(struct pb_tf_printer *pr, const char *fmt, ...)
{
	va_list ap;
	int rc;

	va_start(ap, fmt);
	rc = cbvprintf(pr->out, pr->ctx, fmt, ap);
	va_end(ap);

	return (rc < 0) ? -PB_TF_ERR_INTERNAL : 0;
}

/**
 * True if the body now open is being written on a single line.
 *
 * Either because the caller asked for the compact style, or because this one
 * body was narrow enough to fold into its parent's line. An inlined body cannot
 * contain a submessage -- that is what pb_tf_msg_inlines() checks -- so the flag
 * never has to propagate to a child.
 */
static bool pb_tf_compact(const struct pb_tf_printer *pr)
{
	if (pr->compact) {
		return true;
	}

	return (pr->stack[pr->top].flags & PB_TF_PF_INLINE) != 0U;
}

/**
 * Columns of indentation the current body's fields sit at.
 *
 * One extra level while an expanded list is open, since its elements are nested
 * inside the "name: [" line without a frame of their own.
 */
static unsigned int pb_tf_indent_cols(const struct pb_tf_printer *pr)
{
	const struct pb_tf_pframe *fr = &pr->stack[pr->top];
	unsigned int extra = ((fr->flags & PB_TF_PF_LIST_EXPAND) != 0U) ? 1U : 0U;

	return (pr->base + (unsigned int)fr->level + extra) * 2U;
}

static void pb_tf_indent(struct pb_tf_printer *pr)
{
	unsigned int n = pb_tf_indent_cols(pr);
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

/**
 * Emit whatever must follow the occurrence just written.
 *
 * Inside an open list that is a separator, and only if another element follows:
 * pb_tf_advance() has already run, so a non-zero elem means the run continues.
 * The closing ']' is not emitted here but by pb_tf_run_print(), because the last
 * element of a message list finishes in the pop branch rather than here.
 */
static void pb_tf_occ_end(struct pb_tf_printer *pr, struct pb_tf_pframe *fr)
{
	if ((fr->flags & PB_TF_PF_LIST_OPEN) != 0U) {
		if (fr->elem != 0U) {
			pb_tf_put(pr, ',');
			if ((fr->flags & PB_TF_PF_LIST_EXPAND) != 0U) {
				pb_tf_put(pr, '\n');
			} else {
				pb_tf_put(pr, ' ');
			}
		}
		return;
	}

	pb_tf_field_suffix(pr);
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

/**
 * Length of @p s, stopping at @p n.
 *
 * Not strnlen(): that is POSIX-2008, not ISO C, so glibc hides it behind
 * __STRICT_ANSI__ -- and Zephyr compiles module sources at -std=c17, which sets
 * it. On a cross target the SDK's picolibc declares it anyway and on macOS Apple
 * libc does too, so the omission only surfaces as an error on a glibc host,
 * i.e. native_sim on Linux. An implicitly declared function is assumed to return
 * int, so the length would have been truncated through int before widening back
 * to size_t.
 *
 * Six lines here avoids both a _POSIX_C_SOURCE dance and any further libc
 * variation.
 */
static size_t pb_tf_strnlen(const char *s, size_t n)
{
	size_t i = 0U;

	while ((i < n) && (s[i] != '\0')) {
		i++;
	}

	return i;
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
 * The specifier matches the width rather than promoting everything to
 * long long, which matters for more than tidiness: CBPRINTF_REDUCED_INTEGRAL
 * drops conversions wider than 32 bits, and under CBPRINTF_COMPLETE such a
 * conversion emits the *specification* ("%jd") instead of a number. Promoting a
 * uint32 field to %llu therefore made every field depend on full integral
 * conversion. Printing it as PRIu32 confines that requirement to the fields that
 * genuinely need it, and a message with no 64-bit field works either way.
 *
 * Note this is also where the row's vtype earns its keep on the way out:
 * nanopb's runtime pb_type_t cannot distinguish a float from a fixed32, so a
 * printer driven by the iterator alone could not tell whether to write 1.0 or 1.
 */
static int pb_tf_print_int(struct pb_tf_printer *pr, const struct pb_tf_field *row,
				   pb_size_t data_size, const void *src)
{
	bool sign = pb_tf_print_is_signed(row->vtype);

	if (data_size > sizeof(pb_tf_uint64_t)) {
		/* An int64 field under PB_WITHOUT_64BIT; see the parser. */
		return -PB_TF_ERR_UNSUPPORTED;
	}

	if (data_size > sizeof(uint32_t)) {
		/*
		 * Genuinely wider than 32 bits, so this is the one path that
		 * needs CONFIG_CBPRINTF_FULL_INTEGRAL. Without it the conversion
		 * would emit "%jd" rather than a number, so refuse instead --
		 * the same runtime degradation PB_WITHOUT_64BIT gets, for the
		 * same reason.
		 */
		if (!IS_ENABLED(CONFIG_CBPRINTF_FULL_INTEGRAL)) {
			return -PB_TF_ERR_UNSUPPORTED;
		}

		if (sign) {
			return pb_tf_emit(pr, "%" PRId64, (int64_t)*(const pb_tf_int64_t *)src);
		}
		return pb_tf_emit(pr, "%" PRIu64, (uint64_t)*(const pb_tf_uint64_t *)src);
	}

	/* 1, 2 or 4 bytes: a 32-bit conversion, which every cbprintf provides. */
	if (sign) {
		int32_t v;

		if (data_size == sizeof(int32_t)) {
			v = *(const int32_t *)src;
		} else if (data_size == sizeof(int_least16_t)) {
			v = *(const int_least16_t *)src;
		} else if (data_size == sizeof(int_least8_t)) {
			v = *(const int_least8_t *)src;
		} else {
			return -PB_TF_ERR_INTERNAL;
		}

		return pb_tf_emit(pr, "%" PRId32, v);
	}

	uint32_t v;

	if (data_size == sizeof(uint32_t)) {
		v = *(const uint32_t *)src;
	} else if (data_size == sizeof(uint_least16_t)) {
		v = *(const uint_least16_t *)src;
	} else if (data_size == sizeof(uint_least8_t)) {
		v = *(const uint_least8_t *)src;
	} else {
		return -PB_TF_ERR_INTERNAL;
	}

	return pb_tf_emit(pr, "%" PRIu32, v);
}

static int pb_tf_print_scalar(struct pb_tf_printer *pr, const struct pb_tf_field *row,
				      pb_size_t data_size, const void *src)
{
	switch (row->vtype) {
	case PB_TF_VT_BOOL:
		if (data_size != sizeof(bool)) {
			return -PB_TF_ERR_INTERNAL;
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
	case PB_TF_VT_DOUBLE:
#if defined(CONFIG_NANOPB_TEXTFORMAT_FLOAT)
		/*
		 * The precision is not cosmetic: %.9g is the shortest that round
		 * trips every float, and %.17g every double. Too few digits
		 * silently loses a value that then fails to compare equal.
		 *
		 * Note a Zephyr limit that %.17g cannot escape: cbprintf caps
		 * floating point at 16 extracted digits, so a double needing 17
		 * -- DBL_MAX, for one -- does not round-trip exactly. Asking for
		 * 17 is still correct, and becomes exact if cbprintf gains the
		 * digit; see docs/dialect.md.
		 *
		 * The width decides which to use, not the vtype: with
		 * PB_CONVERT_DOUBLE_FLOAT a proto double is stored as a C float,
		 * and printing 17 digits of a float's worth of information would
		 * emit noise.
		 */
		if (data_size == sizeof(float)) {
			return pb_tf_emit(pr, "%.9g", (double)*(const float *)src);
		}
		if (data_size == sizeof(double)) {
			return pb_tf_emit(pr, "%.17g", *(const double *)src);
		}
		return -PB_TF_ERR_INTERNAL;
#else
		/* No floating-point formatting is referenced, so none is linked. */
		return -PB_TF_ERR_NO_FLOAT;
#endif

	case PB_TF_VT_STRING:
		pb_tf_quoted(pr, (const pb_byte_t *)src, pb_tf_strnlen((const char *)src, data_size));
		return 0;

	case PB_TF_VT_BYTES: {
		const pb_bytes_array_t *arr = src;

		pb_tf_quoted(pr, arr->bytes, (size_t)arr->size);
		return 0;
	}

	case PB_TF_VT_FIXED_LENGTH_BYTES:
		pb_tf_quoted(pr, src, (size_t)data_size);
		return 0;

	default:
		return -PB_TF_ERR_INTERNAL;
	}
}

/**
 * True if this occurrence is at its type's default and
 * CONFIG_NANOPB_TEXTFORMAT_PRINT_SKIP_DEFAULTS says to omit it.
 *
 * Only implicit-presence fields are eligible: an optional field is already
 * governed by its has_ bit, and a repeated one by its count.
 */
static bool pb_tf_skip_default(const struct pb_tf_printer *pr, const struct pb_tf_field *row,
			       pb_size_t data_size, const void *src)
{
	ARG_UNUSED(pr);

	if (!PB_TF_SKIPS_DEFAULTS) {
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
static int pb_tf_locate(const struct pb_tf_pframe *fr, const struct pb_tf_field *row,
				pb_size_t elem, const void **src, pb_size_t *data_size,
				pb_size_t *count)
{
	pb_field_iter_t it;

	*src = NULL;
	*count = 1U;

	/* pb_field_iter_begin() reports false for a message with no fields. */
	if (!pb_field_iter_begin(&it, fr->tf->desc, (void *)fr->msg) ||
	    !pb_field_iter_find(&it, row->tag)) {
		return -PB_TF_ERR_INTERNAL;
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
			return -PB_TF_ERR_INTERNAL;
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

	return 0;
}

/** Advance a frame's cursor past the occurrence just handled. */
static void pb_tf_advance(struct pb_tf_pframe *fr, const struct pb_tf_field *row, pb_size_t count)
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

/*
 * ---------------------------------------------------------------------------
 * Fit-or-expand
 *
 * The multi-line styles keep a submessage or a list on one line when it is
 * narrow enough to read there. Deciding that needs the width the node *would*
 * have, which means rendering it twice -- so the measuring pass must cost
 * nothing structural.
 *
 * It costs nothing because of one rule: a body whose descriptor contains a
 * submessage row always expands, whatever its width. So anything ever measured
 * is a leaf, the measuring pass is a flat loop over one row table, and there is
 * no second frame stack and no recursion. The printer's own sink is borrowed
 * and put back rather than building a second struct pb_tf_printer, which would
 * carry another stack[PB_TF_MAX_DEPTH + 1] -- 144 bytes at the default depth.
 * That is safe because pb_tf_print_scalar() reads only out/ctx: no frame, no
 * style, no printer state of its own.
 *
 * The whole pass is optional. CONFIG_NANOPB_TEXTFORMAT_PRINT_FIT=n replaces the
 * two decision helpers with the constant answers below, which is not the same
 * as PRINT_FIT_WIDTH=0: the output is identical, but the code is gone. It is
 * worth more than its own size, because pb_tf_measure() is the only other
 * caller of pb_tf_locate() and pb_tf_print_scalar() -- the two largest helpers
 * here -- and a second call site is what stops the compiler inlining them into
 * the walk. Measured, the pass costs ~520 B of its own and ~1050 B in total.
 *
 * This is #if and not IS_ENABLED for the same reason the float paths are: the
 * point is that nothing references the code, so none of it is emitted.
 * ---------------------------------------------------------------------------
 */

#if defined(CONFIG_NANOPB_TEXTFORMAT_PRINT_FIT)

/** A sink that counts bytes and keeps none of them. */
struct pb_tf_ctr {
	size_t used;
};

static int pb_tf_out_ctr(int c, void *ctx)
{
	struct pb_tf_ctr *ctr = ctx;

	ctr->used++;

	return c;
}

/**
 * True if @p tf declares a field that is itself a message.
 *
 * A property of the descriptor, not of the value: a message whose only
 * submessage field happens to be unset still expands. Reading the value would
 * mean descending to find out how wide the nested part is, which is exactly the
 * recursion this design does not have. Unrepresentable shapes do not count,
 * since the printer skips them rather than emitting them.
 */
static bool pb_tf_has_msg_row(const struct pb_tf_msg *tf)
{
	uint16_t i;

	for (i = 0U; i < tf->count; i++) {
		const struct pb_tf_field *row = &tf->fields[i];

		if (pb_tf_shape_supported(row) && pb_tf_vtype_is_msg(row->vtype)) {
			return true;
		}
	}

	return false;
}

/**
 * Compact width of rows [@p first, @p last) of one message body.
 *
 * Serves both questions the fit rule asks. Whether a whole body folds is the
 * full row range; whether one repeated row's bracket list folds is a range of
 * one, which comes out as "name: [a, b]" because a repeated row is framed that
 * way here exactly as it is in pb_tf_run_print().
 *
 * Everything is counted through the sink rather than added up by hand, so the
 * measurement cannot drift from what the printer emits -- which matters most for
 * floating point, whose width comes from cbprintf and not from any rule this
 * file could restate.
 *
 * @return the width, or SIZE_MAX if some field in the range cannot be printed
 *         at all. The caller then expands, and the real pass reports the error.
 */
static size_t pb_tf_measure(struct pb_tf_printer *pr, const struct pb_tf_msg *tf, const void *msg,
			    uint16_t first, uint16_t last)
{
	struct pb_tf_pframe scratch;
	struct pb_tf_ctr ctr;
	cbprintf_cb save_out = pr->out;
	void *save_ctx = pr->ctx;
	bool leading = true;
	bool bad = false;
	uint16_t i;

	ctr.used = 0U;
	pr->out = pb_tf_out_ctr;
	pr->ctx = &ctr;

	scratch.tf = tf;
	scratch.msg = msg;
	scratch.index = 0U;
	scratch.elem = 0U;
	scratch.level = 0U;
	scratch.flags = 0U;

	for (i = first; (i < last) && !bad; i++) {
		const struct pb_tf_field *row = &tf->fields[i];
		bool repeated = (PB_TF_HT(row->rules) == PB_TF_HT_REPEATED);
		bool opened = false;
		pb_size_t elem = 0U;

		if (!pb_tf_shape_supported(row)) {
			continue;
		}

		while (!bad) {
			const void *src;
			pb_size_t data_size = 0U;
			pb_size_t count = 1U;

			if (pb_tf_locate(&scratch, row, elem, &src, &data_size, &count) != 0) {
				bad = true;
				break;
			}
			if ((src == NULL) || pb_tf_skip_default(pr, row, data_size, src)) {
				break;
			}
			if (pb_tf_vtype_is_msg(row->vtype)) {
				/* Never asked for: the caller scanned the rows. */
				bad = true;
				break;
			}

			if (opened) {
				(void)pb_tf_emit(pr, ", ");
			} else {
				if (!leading) {
					(void)pb_tf_emit(pr, ", ");
				}
				leading = false;
				(void)pb_tf_emit(pr, repeated ? "%s: [" : "%s: ", row->name);
				opened = true;
			}

			if (pb_tf_print_scalar(pr, row, data_size, src) != 0) {
				bad = true;
				break;
			}

			if (!repeated) {
				break;
			}

			elem++;
			if (elem >= count) {
				break;
			}
		}

		if (opened && repeated) {
			pb_tf_put(pr, ']');
		}
	}

	pr->out = save_out;
	pr->ctx = save_ctx;

	return bad ? SIZE_MAX : ctr.used;
}

/**
 * True if @p width more columns fit on the line now being written.
 *
 * The indentation already spent counts against the budget, so the same message
 * breaks apart once it sits deep enough -- which is the point, the limit being a
 * line width and not a message size. PB_TF_FIT_WIDTH of 0 never fits, and is the
 * documented way to ask for one field per line.
 */
static bool pb_tf_fits(const struct pb_tf_printer *pr, size_t width)
{
	size_t cols = (size_t)pb_tf_indent_cols(pr);

	if (width == SIZE_MAX) {
		return false;
	}

	return (cols + width) <= (size_t)PB_TF_FIT_WIDTH;
}

/**
 * True if this submessage occurrence is to be written on one line.
 *
 * @p frame is what the occurrence costs around its body: the name, " {" and "}"
 * for a named field, or just the braces for an unnamed element of a list.
 */
static bool pb_tf_msg_inlines(struct pb_tf_printer *pr, const struct pb_tf_msg *sub,
			      const void *src, size_t frame)
{
	size_t width;

	if (pb_tf_compact(pr)) {
		return true;
	}

	if (pb_tf_has_msg_row(sub)) {
		return false;
	}

	width = pb_tf_measure(pr, sub, src, 0U, sub->count);
	if (width == SIZE_MAX) {
		return false;
	}

	return pb_tf_fits(pr, frame + width);
}

/** True if an opening list is to put each element on its own line. */
static bool pb_tf_list_expands(struct pb_tf_printer *pr, const struct pb_tf_pframe *fr,
			       const struct pb_tf_field *row)
{
	if (pb_tf_compact(pr)) {
		return false;
	}

	if (pb_tf_vtype_is_msg(row->vtype)) {
		/*
		 * Elements are messages, so the same rule as a named submessage
		 * applies to the list: it always breaks. Which element you are
		 * reading is then visible rather than merely countable.
		 */
		return true;
	}

	/* A range of one row, which frames itself as "name: [...]". */
	return !pb_tf_fits(pr,
			   pb_tf_measure(pr, fr->tf, fr->msg, fr->index,
					 (uint16_t)(fr->index + 1U)));
}

#else /* !CONFIG_NANOPB_TEXTFORMAT_PRINT_FIT */

/*
 * Nothing is measured, so nothing folds: a multi-line style gives every
 * submessage and every list element a line of its own, which is what
 * PRINT_FIT_WIDTH=0 also produces. Compact is unaffected -- it folds
 * everything by definition, and asks no question that needs an answer.
 */

static bool pb_tf_msg_inlines(struct pb_tf_printer *pr, const struct pb_tf_msg *sub,
			      const void *src, size_t frame)
{
	ARG_UNUSED(sub);
	ARG_UNUSED(src);
	ARG_UNUSED(frame);

	return pb_tf_compact(pr);
}

static bool pb_tf_list_expands(struct pb_tf_printer *pr, const struct pb_tf_pframe *fr,
			       const struct pb_tf_field *row)
{
	ARG_UNUSED(fr);
	ARG_UNUSED(row);

	return !pb_tf_compact(pr);
}

#endif /* CONFIG_NANOPB_TEXTFORMAT_PRINT_FIT */

static int pb_tf_run_print(struct pb_tf_printer *pr)
{
	while (true) {
		struct pb_tf_pframe *fr = &pr->stack[pr->top];
		const struct pb_tf_field *row;
		const void *src;
		pb_size_t data_size = 0U;
		pb_size_t count = 1U;
		bool repeated;
		int err;

		/*
		 * An open list whose run has ended. elem is only zero again once
		 * pb_tf_advance() has moved past the last element, and the skip
		 * paths below cannot fire mid-list -- pb_tf_locate() yields a
		 * source for every elem below count, and pb_tf_skip_default()
		 * never applies to a repeated row -- so this is an exact test.
		 */
		if (((fr->flags & PB_TF_PF_LIST_OPEN) != 0U) && (fr->elem == 0U)) {
			bool expand = (fr->flags & PB_TF_PF_LIST_EXPAND) != 0U;

			/* Cleared first, so the ']' indents to the field, not the
			 * elements.
			 */
			fr->flags &= (uint8_t)~(PB_TF_PF_LIST_OPEN | PB_TF_PF_LIST_EXPAND);

			if (expand) {
				pb_tf_put(pr, '\n');
				pb_tf_indent(pr);
			}
			pb_tf_put(pr, ']');
			pb_tf_field_suffix(pr);
			continue;
		}

		if (fr->index >= fr->tf->count) {
			bool inl;

			if (pr->top == 0U) {
				return 0;
			}

			/*
			 * Close this body; the parent's cursor already moved on.
			 * Whether the body was inlined is the child's property, and
			 * pb_tf_compact() reads the parent once top has been
			 * decremented -- so read it while the child is still
			 * current.
			 */
			inl = (fr->flags & PB_TF_PF_INLINE) != 0U;

			pr->top--;
			fr = &pr->stack[pr->top];

			/* Compact stays tight and symmetric: "origin {x: 1, y: 2}". */
			if (!pr->compact && !inl) {
				pb_tf_indent(pr);
			}
			pb_tf_put(pr, '}');
			pb_tf_occ_end(pr, fr);
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
		if (err != 0) {
			return err;
		}

		if ((src == NULL) || pb_tf_skip_default(pr, row, data_size, src)) {
			/* Absent, or a default being skipped. */
			fr->index++;
			fr->elem = 0U;
			continue;
		}

		/* Checked before anything is emitted, so a refusal emits nothing. */
		if (pb_tf_vtype_is_msg(row->vtype)) {
			if (row->sub == NULL) {
				return -PB_TF_ERR_INTERNAL;
			}
			if (pr->top >= (unsigned int)PB_TF_MAX_DEPTH) {
				return -PB_TF_ERR_DEPTH;
			}
		}

		repeated = (PB_TF_HT(row->rules) == PB_TF_HT_REPEATED);

		/*
		 * A repeated field is one bracket list, not one field per element:
		 * "tags: [1, 2]" rather than "tags: 1 tags: 2". Both are valid
		 * text format and both re-parse, but the list groups the elements
		 * and does not repeat the name, so which element you are reading
		 * is visible. The walk itself is unchanged -- each element still
		 * re-enters this loop through fr->elem -- only the framing is new.
		 */
		if (repeated && ((fr->flags & PB_TF_PF_LIST_OPEN) == 0U)) {
			bool expand = pb_tf_list_expands(pr, fr, row);

			pb_tf_field_prefix(pr, fr);
			err = pb_tf_emit(pr, "%s: [", row->name);
			if (err != 0) {
				return err;
			}

			fr->flags |= PB_TF_PF_LIST_OPEN;
			if (expand) {
				fr->flags |= PB_TF_PF_LIST_EXPAND;
				pb_tf_put(pr, '\n');
			}
		}

		/* An element of an expanded list sits on a line of its own. */
		if (repeated && ((fr->flags & PB_TF_PF_LIST_EXPAND) != 0U)) {
			pb_tf_indent(pr);
		}

		if (pb_tf_vtype_is_msg(row->vtype)) {
			unsigned int level;
			bool inl;

			if (repeated) {
				/* List elements carry no name: "rep: [{a: 1}]". */
				inl = pb_tf_msg_inlines(pr, row->sub, src, 2U);
				pb_tf_put(pr, '{');
			} else {
				inl = pb_tf_msg_inlines(pr, row->sub, src,
							strlen(row->name) + 3U);
				pb_tf_field_prefix(pr, fr);
				err = pb_tf_emit(pr, "%s {", row->name);
				if (err != 0) {
					return err;
				}
			}

			if (!pr->compact && !inl) {
				pb_tf_put(pr, '\n');
			}

			/* Advance before descending, so the pop needs no fixup. */
			pb_tf_advance(fr, row, count);

			/*
			 * One level in from this body, and one more again when an
			 * expanded list put the element on its own line.
			 */
			level = (unsigned int)fr->level + 1U +
				(((fr->flags & PB_TF_PF_LIST_EXPAND) != 0U) ? 1U : 0U);

			pr->top++;
			pr->stack[pr->top].tf = row->sub;
			pr->stack[pr->top].msg = src;
			pr->stack[pr->top].index = 0U;
			pr->stack[pr->top].elem = 0U;
			pr->stack[pr->top].level = (uint8_t)level;
			pr->stack[pr->top].flags = PB_TF_PF_FIRST;
			if (inl) {
				pr->stack[pr->top].flags |= PB_TF_PF_INLINE;
			}
			continue;
		}

		if (!repeated) {
			pb_tf_field_prefix(pr, fr);
			err = pb_tf_emit(pr, "%s: ", row->name);
			if (err != 0) {
				return err;
			}
		}

		err = pb_tf_print_scalar(pr, row, data_size, src);
		if (err != 0) {
			return err;
		}

		/* Advance first: pb_tf_occ_end() reads the cursor to tell whether
		 * another element follows.
		 */
		pb_tf_advance(fr, row, count);
		pb_tf_occ_end(pr, fr);
	}
}

int pb_tf_print_styled(const struct pb_tf_msg *tf, const void *msg, cbprintf_cb out, void *ctx,
		       bool compact, bool braces)
{
	struct pb_tf_printer pr;
	int err;

	if ((tf == NULL) || (msg == NULL) || (out == NULL)) {
		return -PB_TF_ERR_INTERNAL;
	}

	pr.out = out;
	pr.ctx = ctx;
	pr.compact = compact;
	pr.braces = braces;
	pr.base = braces ? 1U : 0U;
	pr.top = 0U;

	pr.stack[0].tf = tf;
	pr.stack[0].msg = msg;
	pr.stack[0].index = 0U;
	pr.stack[0].elem = 0U;
	pr.stack[0].level = 0U;
	pr.stack[0].flags = PB_TF_PF_FIRST;

	if (pr.braces) {
		pb_tf_put(&pr, '{');
		if (!pb_tf_compact(&pr)) {
			pb_tf_put(&pr, '\n');
		}
	}

	err = pb_tf_run_print(&pr);
	if (err != 0) {
		return err;
	}

	if (pr.braces) {
		pb_tf_put(&pr, '}');
		if (!pb_tf_compact(&pr)) {
			pb_tf_put(&pr, '\n');
		}
	}

	return 0;
}

int pb_tf_print_buf_styled(const struct pb_tf_msg *tf, const void *msg, char *buf, size_t size,
			   bool compact, bool braces)
{
	struct pb_tf_buf sink;
	int err;

	if ((buf == NULL) || (size == 0U)) {
		return -PB_TF_ERR_INTERNAL;
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

	err = pb_tf_print_styled(tf, msg, pb_tf_out_buf, &sink, compact, braces);
	if (err != 0) {
		return err;
	}

	return sink.overflow ? -PB_TF_ERR_NO_SPACE : 0;
}
