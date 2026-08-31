/*
 * Copyright (c) 2026 Rodrigo Peixoto
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Text-format parser: lexer, recursive descent, and value binding.
 *
 * Single pass, no heap and no scratch buffers -- values are written straight
 * into the destination struct through nanopb's field iterator.
 */

#include <string.h>

#include <pb_common.h>

#include "nanopb_textformat_priv.h"

/** Parser state: the input, a cursor into it, and where to report errors. */
struct pb_tf_parser {
	/** Start of the input, so a cursor can be turned into an offset. */
	const char *text;
	/** Current position. */
	const char *p;
	/** Caller's status block, or NULL. */
	pb_tf_status_t *status;
	/** Submessages currently open, bounded by PB_TF_MAX_DEPTH. */
	unsigned int depth;
};

/* Recursion: a submessage value is itself a field body. */
static pb_tf_err_t pb_tf_parse_fields(struct pb_tf_parser *ps, const pb_tf_msg_t *tf, void *msg,
				      bool braced);

/** A scanned integer literal: magnitude, sign and the base it was written in. */
struct pb_tf_num {
	pb_tf_uint64_t mag;
	bool neg;
	/** 8, 10 or 16. Only a decimal literal can be the head of a float. */
	unsigned int base;
};

/*
 * ---------------------------------------------------------------------------
 * Lexer
 * ---------------------------------------------------------------------------
 */

/**
 * Record an error and return its code.
 *
 * @p at points into the input; the reported offset is relative to its start,
 * clamped because pb_tf_status_t carries a uint16_t (a shell line is capped far
 * below that by CONFIG_SHELL_CMD_BUFF_SIZE anyway).
 */
static pb_tf_err_t pb_tf_fail(struct pb_tf_parser *ps, pb_tf_err_t code, const char *at,
			      const char *field)
{
	if (ps->status != NULL) {
		size_t off = (size_t)(at - ps->text);

		ps->status->code = code;
		ps->status->offset = (uint16_t)MIN(off, (size_t)UINT16_MAX);
		ps->status->field = field;
	}

	return code;
}

/*
 * Text format's whitespace. '#' line comments are deliberately not accepted:
 * the dialect is a strict subset, so an unsupported construct must be an error
 * rather than silently skipped.
 */
static bool pb_tf_is_space(char c)
{
	return (c == ' ') || (c == '\t') || (c == '\n') || (c == '\r') || (c == '\v') || (c == '\f');
}

static void pb_tf_skip_ws(struct pb_tf_parser *ps)
{
	while (pb_tf_is_space(*ps->p)) {
		ps->p++;
	}
}

static bool pb_tf_is_ident_start(char c)
{
	return (((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || (c == '_'));
}

static bool pb_tf_is_ident_char(char c)
{
	return pb_tf_is_ident_start(c) || ((c >= '0') && (c <= '9'));
}

/** Consume an identifier and return its length, or 0 if none starts here. */
static size_t pb_tf_scan_ident(struct pb_tf_parser *ps)
{
	const char *start = ps->p;

	if (!pb_tf_is_ident_start(*ps->p)) {
		return 0U;
	}

	while (pb_tf_is_ident_char(*ps->p)) {
		ps->p++;
	}

	return (size_t)(ps->p - start);
}

/** Exact comparison of a non-NUL-terminated identifier against a literal. */
static bool pb_tf_ident_is(const char *s, size_t len, const char *lit)
{
	return (strlen(lit) == len) && (strncmp(s, lit, len) == 0);
}

/** Digit value of @p c in @p base, or false if it is not one. */
static bool pb_tf_digit_val(char c, unsigned int base, unsigned int *out)
{
	unsigned int v;

	if ((c >= '0') && (c <= '9')) {
		v = (unsigned int)(c - '0');
	} else if ((c >= 'a') && (c <= 'f')) {
		v = (unsigned int)(c - 'a') + 10U;
	} else if ((c >= 'A') && (c <= 'F')) {
		v = (unsigned int)(c - 'A') + 10U;
	} else {
		return false;
	}

	if (v >= base) {
		return false;
	}

	*out = v;

	return true;
}

/**
 * Scan an integer literal: optional '-', then decimal, 0x hex or leading-zero
 * octal. A leading '+' is not accepted, keeping the dialect a strict subset.
 *
 * The magnitude accumulates into pb_tf_uint64_t, so what fits here is exactly
 * what this build's codec could hold -- the range check against the field's own
 * width happens later, in pb_tf_write_int().
 */
static pb_tf_err_t pb_tf_scan_num(struct pb_tf_parser *ps, const char *field,
				  struct pb_tf_num *out)
{
	const char *start = ps->p;
	bool any = false;
	unsigned int d;

	out->mag = 0U;
	out->neg = false;
	out->base = 10U;

	if (*ps->p == '-') {
		out->neg = true;
		ps->p++;
	}

	if (*ps->p == '0') {
		if ((ps->p[1] == 'x') || (ps->p[1] == 'X')) {
			out->base = 16U;
			ps->p += 2;
		} else if ((ps->p[1] >= '0') && (ps->p[1] <= '9')) {
			/*
			 * Leading zero means octal. Consuming only the '0' leaves
			 * an 8 or 9 to fail as a non-octal digit below, which is
			 * what protoc does with e.g. 09.
			 */
			out->base = 8U;
			ps->p += 1;
		}
	}

	while (pb_tf_digit_val(*ps->p, out->base, &d)) {
		if (out->mag > ((PB_TF_UINT64_MAX - d) / out->base)) {
			return pb_tf_fail(ps, PB_TF_ERR_RANGE, start, field);
		}
		out->mag = (out->mag * out->base) + d;
		any = true;
		ps->p++;
	}

	if (!any) {
		return pb_tf_fail(ps, PB_TF_ERR_SYNTAX, start, field);
	}

	return PB_TF_OK;
}

/*
 * ---------------------------------------------------------------------------
 * Value binding
 * ---------------------------------------------------------------------------
 */

/**
 * True for the vtypes whose C storage is signed.
 *
 * This is exactly why the row carries a vtype: nanopb's runtime pb_type_t
 * collapses INT32/INT64/ENUM into PB_LTYPE_VARINT and FLOAT/FIXED32/SFIXED32
 * into PB_LTYPE_FIXED32, so signedness is not recoverable from the iterator.
 */
static bool pb_tf_vtype_is_signed(uint8_t vtype)
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
 * Write an integer into @p dest, dispatching on the field's own data_size.
 *
 * The width chain mirrors pb_decode.c:1469-1478,1513-1522 exactly, including
 * its assign-then-compare range check. Dispatching rather than assuming 4 bytes
 * is what makes nanopb's int_size option, an implementation-defined enum width
 * (-fshort-enums), and PB_CONVERT_DOUBLE_FLOAT all come out correct.
 */
static pb_tf_err_t pb_tf_write_int(struct pb_tf_parser *ps, const pb_tf_field_t *row,
				   const pb_field_iter_t *it, void *dest,
				   const struct pb_tf_num *n, const char *at)
{
	/*
	 * A field wider than this build's accumulator, i.e. an int64 field under
	 * PB_WITHOUT_64BIT. That is a configuration limit rather than a
	 * descriptor bug, so it earns the same class of answer as a float field
	 * under CONFIG_NANOPB_TEXTFORMAT_FLOAT=n. The chains below keep
	 * PB_TF_ERR_INTERNAL for a data_size that matches no integer width at
	 * all, which really would mean a descriptor mismatch.
	 */
	if (it->data_size > sizeof(pb_tf_uint64_t)) {
		return pb_tf_fail(ps, PB_TF_ERR_UNSUPPORTED, at, row->name);
	}

	if (pb_tf_vtype_is_signed(row->vtype)) {
		pb_tf_int64_t svalue;
		pb_tf_int64_t clamped;

		if (n->neg) {
			if (n->mag > ((pb_tf_uint64_t)PB_TF_INT64_MAX + 1U)) {
				return pb_tf_fail(ps, PB_TF_ERR_RANGE, at, row->name);
			}
			if (n->mag == ((pb_tf_uint64_t)PB_TF_INT64_MAX + 1U)) {
				/* Negating this magnitude would overflow. */
				svalue = PB_TF_INT64_MIN;
			} else {
				svalue = -(pb_tf_int64_t)n->mag;
			}
		} else {
			if (n->mag > (pb_tf_uint64_t)PB_TF_INT64_MAX) {
				return pb_tf_fail(ps, PB_TF_ERR_RANGE, at, row->name);
			}
			svalue = (pb_tf_int64_t)n->mag;
		}

		if (it->data_size == sizeof(pb_tf_int64_t)) {
			clamped = *(pb_tf_int64_t *)dest = svalue;
		} else if (it->data_size == sizeof(int32_t)) {
			clamped = *(int32_t *)dest = (int32_t)svalue;
		} else if (it->data_size == sizeof(int_least16_t)) {
			clamped = *(int_least16_t *)dest = (int_least16_t)svalue;
		} else if (it->data_size == sizeof(int_least8_t)) {
			clamped = *(int_least8_t *)dest = (int_least8_t)svalue;
		} else {
			return pb_tf_fail(ps, PB_TF_ERR_INTERNAL, at, row->name);
		}

		if (clamped != svalue) {
			return pb_tf_fail(ps, PB_TF_ERR_RANGE, at, row->name);
		}

		return PB_TF_OK;
	}

	/*
	 * Unsigned target. A negative literal is refused outright rather than
	 * wrapping, including -0: protoc rejects it too, and silently storing a
	 * huge positive value would be the worst outcome for hand-typed input.
	 */
	if (n->neg) {
		return pb_tf_fail(ps, PB_TF_ERR_RANGE, at, row->name);
	}

	pb_tf_uint64_t value = n->mag;
	pb_tf_uint64_t clamped;

	if (it->data_size == sizeof(pb_tf_uint64_t)) {
		clamped = *(pb_tf_uint64_t *)dest = value;
	} else if (it->data_size == sizeof(uint32_t)) {
		clamped = *(uint32_t *)dest = (uint32_t)value;
	} else if (it->data_size == sizeof(uint_least16_t)) {
		clamped = *(uint_least16_t *)dest = (uint_least16_t)value;
	} else if (it->data_size == sizeof(uint_least8_t)) {
		clamped = *(uint_least8_t *)dest = (uint_least8_t)value;
	} else {
		return pb_tf_fail(ps, PB_TF_ERR_INTERNAL, at, row->name);
	}

	if (clamped != value) {
		return pb_tf_fail(ps, PB_TF_ERR_RANGE, at, row->name);
	}

	return PB_TF_OK;
}

/** Parse a bool: true/false/True/False/t/f, or the integers 1 and 0. */
static pb_tf_err_t pb_tf_write_bool(struct pb_tf_parser *ps, const pb_tf_field_t *row,
				    const pb_field_iter_t *it, void *dest)
{
	const char *start = ps->p;
	bool value;

	if (pb_tf_is_ident_start(*ps->p)) {
		size_t len = pb_tf_scan_ident(ps);

		if (pb_tf_ident_is(start, len, "true") || pb_tf_ident_is(start, len, "True") ||
		    pb_tf_ident_is(start, len, "t")) {
			value = true;
		} else if (pb_tf_ident_is(start, len, "false") ||
			   pb_tf_ident_is(start, len, "False") ||
			   pb_tf_ident_is(start, len, "f")) {
			value = false;
		} else {
			return pb_tf_fail(ps, PB_TF_ERR_TYPE, start, row->name);
		}
	} else {
		struct pb_tf_num n;
		pb_tf_err_t err = pb_tf_scan_num(ps, row->name, &n);

		if (err != PB_TF_OK) {
			return err;
		}
		if (n.neg || (n.mag > 1U)) {
			return pb_tf_fail(ps, PB_TF_ERR_RANGE, start, row->name);
		}
		value = (n.mag == 1U);
	}

	if (it->data_size != sizeof(bool)) {
		return pb_tf_fail(ps, PB_TF_ERR_INTERNAL, start, row->name);
	}

	*(bool *)dest = value;

	return PB_TF_OK;
}

/**
 * Parse a "{ ... }" submessage body into @p dest.
 *
 * The braces are required here even though the colon before them is optional:
 * text format's only other Message delimiters are angle brackets, which are out
 * of scope. @p dest is the submessage struct, so the recursion needs no offset
 * arithmetic of its own.
 *
 * Note what is deliberately absent: @p dest is not cleared. A singular or
 * optional submessage mentioned twice therefore merges, which is what text
 * format asks for. A repeated one is cleared per element by its caller.
 */
static pb_tf_err_t pb_tf_parse_submsg(struct pb_tf_parser *ps, const pb_tf_field_t *row, void *dest)
{
	pb_tf_err_t err;

	if (row->sub == NULL) {
		/* PB_TF_DEFINE fills this in for every MESSAGE/MSG_W_CB row. */
		return pb_tf_fail(ps, PB_TF_ERR_INTERNAL, ps->p, row->name);
	}

	/*
	 * Check before descending, not after: each level costs a stack frame, so
	 * the limit has to stop the call rather than report it afterwards.
	 */
	if (ps->depth >= (unsigned int)PB_TF_MAX_DEPTH) {
		return pb_tf_fail(ps, PB_TF_ERR_DEPTH, ps->p, row->name);
	}

	if (*ps->p != '{') {
		return pb_tf_fail(ps, PB_TF_ERR_SYNTAX, ps->p, row->name);
	}
	ps->p++;

	ps->depth++;
	err = pb_tf_parse_fields(ps, row->sub, dest, true);
	ps->depth--;

	return err;
}

/**
 * Parse one value of @p row's type into @p dest.
 *
 * @p dest is passed in rather than taken from the iterator because a repeated
 * field writes into element n, not the head of the array.
 */
static pb_tf_err_t pb_tf_parse_value(struct pb_tf_parser *ps, const pb_tf_field_t *row,
				     const pb_field_iter_t *it, void *dest)
{
	const char *start = ps->p;
	struct pb_tf_num n;
	pb_tf_err_t err;

	switch (row->vtype) {
	case PB_TF_VT_BOOL:
		return pb_tf_write_bool(ps, row, it, dest);

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
		err = pb_tf_scan_num(ps, row->name, &n);
		if (err != PB_TF_OK) {
			return err;
		}
		/*
		 * A decimal literal that continues into '.', an exponent or a
		 * float suffix is a float, not an integer that happens to stop
		 * early. Report the type mismatch here rather than leaving the
		 * tail to surface as a confusing syntax error. Hex needs no
		 * such check, since 'e' and 'f' are digits there.
		 */
		if ((n.base == 10U) &&
		    ((*ps->p == '.') || (*ps->p == 'e') || (*ps->p == 'E') || (*ps->p == 'f') ||
		     (*ps->p == 'F'))) {
			return pb_tf_fail(ps, PB_TF_ERR_TYPE, start, row->name);
		}
		return pb_tf_write_int(ps, row, it, dest, &n, start);

	case PB_TF_VT_FLOAT:
	case PB_TF_VT_DOUBLE:
		if (!IS_ENABLED(CONFIG_NANOPB_TEXTFORMAT_FLOAT)) {
			return pb_tf_fail(ps, PB_TF_ERR_NO_FLOAT, start, row->name);
		}
		/* TODO(milestone 6): strtod() and the float suffixes. */
		return pb_tf_fail(ps, PB_TF_ERR_UNSUPPORTED, start, row->name);

	case PB_TF_VT_STRING:
	case PB_TF_VT_BYTES:
	case PB_TF_VT_FIXED_LENGTH_BYTES:
		/* TODO(milestone 4): quoting and escape sequences. */
		return pb_tf_fail(ps, PB_TF_ERR_UNSUPPORTED, start, row->name);

	case PB_TF_VT_MESSAGE:
	case PB_TF_VT_MSG_W_CB:
		/*
		 * MSG_W_CB is just MESSAGE with a pb_callback_t ahead of it in the
		 * struct. nanopb emits that member before the has_ bit and derives
		 * size_offset from the latter, so the callback sits strictly below
		 * pSize and writing through pData can never disturb it.
		 */
		return pb_tf_parse_submsg(ps, row, dest);

	default:
		return pb_tf_fail(ps, PB_TF_ERR_INTERNAL, start, row->name);
	}
}

/**
 * Parse one value and store it according to @p row's repetition rule.
 *
 * The iterator is resolved per occurrence: pb_field_iter_find() is cheap, and
 * re-resolving keeps this correct for a field mentioned more than once.
 */
static pb_tf_err_t pb_tf_store_value(struct pb_tf_parser *ps, const pb_tf_msg_t *tf, void *msg,
				     const pb_tf_field_t *row)
{
	pb_field_iter_t it;
	pb_tf_err_t err;

	if (!pb_field_iter_begin(&it, tf->desc, msg) || !pb_field_iter_find(&it, row->tag)) {
		return pb_tf_fail(ps, PB_TF_ERR_INTERNAL, ps->p, row->name);
	}

	switch (PB_TF_HT(row->rules)) {
	case PB_TF_HT_REQUIRED:
	case PB_TF_HT_SINGULAR:
		/*
		 * pSize is NULL for these (pb_common.c:74-117 leaves it unset
		 * when size_offset is 0), so it must not be touched. Repeating
		 * such a field simply overwrites, which for a submessage is the
		 * merge semantics text format asks for.
		 */
		return pb_tf_parse_value(ps, row, &it, it.pData);

	case PB_TF_HT_OPTIONAL:
		if (it.pSize == NULL) {
			return pb_tf_fail(ps, PB_TF_ERR_INTERNAL, ps->p, row->name);
		}
		err = pb_tf_parse_value(ps, row, &it, it.pData);
		if (err == PB_TF_OK) {
			*(bool *)it.pSize = true;
		}
		return err;

	case PB_TF_HT_REPEATED: {
		pb_size_t n;
		void *slot;

		if (it.pSize == NULL) {
			return pb_tf_fail(ps, PB_TF_ERR_INTERNAL, ps->p, row->name);
		}

		n = *(pb_size_t *)it.pSize;
		if (n >= it.array_size) {
			return pb_tf_fail(ps, PB_TF_ERR_TOO_MANY, ps->p, row->name);
		}

		/*
		 * pData is the head of the array and data_size is one element.
		 * Clear the slot first so an appended element cannot inherit
		 * bytes from a previous, longer occupant.
		 */
		slot = (char *)it.pData + ((size_t)n * (size_t)it.data_size);
		(void)memset(slot, 0, it.data_size);

		err = pb_tf_parse_value(ps, row, &it, slot);
		if (err == PB_TF_OK) {
			/* Commit only on success, so a failed value is not counted. */
			*(pb_size_t *)it.pSize = (pb_size_t)(n + 1U);
		}
		return err;
	}

	case PB_TF_HT_ONEOF:
		if (it.pSize == NULL) {
			return pb_tf_fail(ps, PB_TF_ERR_INTERNAL, ps->p, row->name);
		}
		/*
		 * Switching members must clear the union first, or the new
		 * member inherits the previous one's bytes.
		 */
		if (*(pb_size_t *)it.pSize != (pb_size_t)row->tag) {
			(void)memset(it.pData, 0, it.data_size);
		}
		err = pb_tf_parse_value(ps, row, &it, it.pData);
		if (err == PB_TF_OK) {
			*(pb_size_t *)it.pSize = (pb_size_t)row->tag;
		}
		return err;

	default:
		/* FIXARRAY; pb_tf_shape_supported() rejects it before we get here. */
		return pb_tf_fail(ps, PB_TF_ERR_UNSUPPORTED, ps->p, row->name);
	}
}

/*
 * ---------------------------------------------------------------------------
 * Message body
 * ---------------------------------------------------------------------------
 */

/**
 * Parse a sequence of fields.
 *
 * When @p braced, a '}' ends the body and is consumed; otherwise end of input
 * does. Runs to the terminator, so the caller only has to check for trailing
 * junk once.
 */
static pb_tf_err_t pb_tf_parse_fields(struct pb_tf_parser *ps, const pb_tf_msg_t *tf, void *msg,
				      bool braced)
{
	while (true) {
		const char *fstart;
		size_t nlen;
		const pb_tf_field_t *row;
		pb_tf_err_t err;

		pb_tf_skip_ws(ps);

		if (braced && (*ps->p == '}')) {
			ps->p++;
			return PB_TF_OK;
		}

		if (*ps->p == '\0') {
			if (braced) {
				/* Unterminated body. */
				return pb_tf_fail(ps, PB_TF_ERR_SYNTAX, ps->p, NULL);
			}
			return PB_TF_OK;
		}

		fstart = ps->p;
		nlen = pb_tf_scan_ident(ps);
		if (nlen == 0U) {
			return pb_tf_fail(ps, PB_TF_ERR_SYNTAX, fstart, NULL);
		}

		row = pb_tf_field_find(tf, fstart, nlen);
		if (row == NULL) {
			return pb_tf_fail(ps, PB_TF_ERR_UNKNOWN_FIELD, fstart, NULL);
		}

		/* POINTER, CALLBACK, FIXARRAY and EXTENSION shapes, as a runtime error. */
		if (!pb_tf_shape_supported(row)) {
			return pb_tf_fail(ps, PB_TF_ERR_UNSUPPORTED, fstart, row->name);
		}

		pb_tf_skip_ws(ps);

		/* The colon is optional before a submessage, required otherwise. */
		if (*ps->p == ':') {
			ps->p++;
		} else if (!pb_tf_vtype_is_msg(row->vtype)) {
			return pb_tf_fail(ps, PB_TF_ERR_SYNTAX, ps->p, row->name);
		}

		pb_tf_skip_ws(ps);

		if (*ps->p == '[') {
			if (PB_TF_HT(row->rules) != PB_TF_HT_REPEATED) {
				return pb_tf_fail(ps, PB_TF_ERR_SYNTAX, ps->p, row->name);
			}

			ps->p++;
			pb_tf_skip_ws(ps);

			if (*ps->p == ']') {
				/* "x: []" is legal and means nothing to append. */
				ps->p++;
			} else {
				while (true) {
					err = pb_tf_store_value(ps, tf, msg, row);
					if (err != PB_TF_OK) {
						return err;
					}

					pb_tf_skip_ws(ps);

					if (*ps->p == ',') {
						ps->p++;
						pb_tf_skip_ws(ps);
						continue;
					}
					if (*ps->p == ']') {
						ps->p++;
						break;
					}

					return pb_tf_fail(ps, PB_TF_ERR_SYNTAX, ps->p, row->name);
				}
			}
		} else {
			err = pb_tf_store_value(ps, tf, msg, row);
			if (err != PB_TF_OK) {
				return err;
			}
		}

		/* Field separators are optional in text format. */
		pb_tf_skip_ws(ps);
		if ((*ps->p == ',') || (*ps->p == ';')) {
			ps->p++;
		}
	}
}

pb_tf_err_t pb_tf_parse(const pb_tf_msg_t *tf, void *msg, const char *text, unsigned int flags,
			pb_tf_status_t *status)
{
	struct pb_tf_parser ps;
	pb_tf_err_t err;
	bool braced = false;

	if (status != NULL) {
		status->code = PB_TF_OK;
		status->offset = 0U;
		status->field = NULL;
	}

	if ((tf == NULL) || (msg == NULL) || (text == NULL)) {
		if (status != NULL) {
			status->code = PB_TF_ERR_INTERNAL;
		}
		return PB_TF_ERR_INTERNAL;
	}

	ps.text = text;
	ps.p = text;
	ps.status = status;
	ps.depth = 0U;

	if ((flags & PB_TF_NOINIT) == 0U) {
		pb_tf_clear(tf, msg);
	}

	pb_tf_skip_ws(&ps);

	/*
	 * Outer braces are accepted but not required. '{' can never legally start
	 * a top-level text-format Message -- a FieldName is an identifier or '['
	 * -- so this is unambiguous, provided the matching '}' has to be the last
	 * token. That is what the trailing-junk check below enforces.
	 */
	if (*ps.p == '{') {
		braced = true;
		ps.p++;
	}

	err = pb_tf_parse_fields(&ps, tf, msg, braced);
	if (err != PB_TF_OK) {
		return err;
	}

	pb_tf_skip_ws(&ps);
	if (*ps.p != '\0') {
		return pb_tf_fail(&ps, PB_TF_ERR_SYNTAX, ps.p, NULL);
	}

	return PB_TF_OK;
}
