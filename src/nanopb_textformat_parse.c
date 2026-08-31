/*
 * Copyright (c) 2026 Rodrigo Peixoto
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Text-format parser: lexer, state machine, and value binding.
 *
 * Single pass, no heap and no scratch buffers -- values are written straight
 * into the destination struct through nanopb's field iterator.
 *
 * Submessages are walked with an explicit frame stack rather than by recursion.
 * The obvious implementation is mutually recursive -- a submessage value is
 * itself a field body -- but that makes stack use a function of the input:
 * measured at -Os on Cortex-M3 the cycle cost 144 bytes per level, so the
 * default depth of 8 wanted ~1.2 KB and the Kconfig maximum of 32 wanted
 * ~4.6 KB, against a CONFIG_SHELL_STACK_SIZE that defaults to 2048. A depth
 * cap bounds the level count, but nothing there converts levels into bytes,
 * and raising the cap silently spends stack.
 *
 * Here the frames are an array inside struct pb_tf_parser, so the whole parse
 * costs one frame whose size is a compile-time constant visible in a single
 * -fstack-usage line. It also keeps the library clear of MISRA C:2025
 * Rule 17.2, which forbids recursion direct or indirect.
 */

#include <string.h>

#if defined(CONFIG_NANOPB_TEXTFORMAT_FLOAT)
#include <float.h>
#include <math.h>
#include <stdlib.h>
#endif

#include <pb_common.h>

#include "nanopb_textformat_priv.h"

/** @name What to write once a field's value is known to be complete. */
/**@{*/
#define PB_TF_CM_NONE    0U
/** Set the has_ bit of an optional field. */
#define PB_TF_CM_PRESENT 1U
/** Store a pb_size_t: a repeated count, or a oneof's which_ tag. */
#define PB_TF_CM_SIZE    2U
/**@}*/

/** @name Frame flags. */
/**@{*/
/** This body is terminated by '}' rather than by end of input. */
#define PB_TF_FR_BRACED  0x01U
/** A '[' list is open, so a value is followed by ',' or ']'. */
#define PB_TF_FR_IN_LIST 0x02U
/**@}*/

/**
 * One open message body.
 *
 * The commit fields describe what the *parent* owes once this body closes.
 * Deferring matters for a repeated submessage: the element count must not
 * include an element whose body failed halfway through, so the increment waits
 * for the closing brace. A scalar needs no deferral -- success is known
 * immediately -- but travels the same path for uniformity.
 */
struct pb_tf_frame {
	/** Descriptor of the message this body fills. */
	const pb_tf_msg_t *tf;
	/** The struct being filled. */
	void *msg;
	/** Field currently being parsed, for list continuation and diagnostics. */
	const pb_tf_field_t *row;
	/** Where to write on completion: an iterator's pSize, or NULL. */
	void *commit_ptr;
	/** Value for PB_TF_CM_SIZE. */
	pb_size_t commit_val;
	/** One of PB_TF_CM_*. */
	uint8_t commit_kind;
	/** PB_TF_FR_* */
	uint8_t flags;
};

/** Parser state: the input, a cursor into it, and where to report errors. */
struct pb_tf_parser {
	/** Start of the input, so a cursor can be turned into an offset. */
	const char *text;
	/** Current position. */
	const char *p;
	/** Caller's status block, or NULL. */
	pb_tf_status_t *status;
	/** Index of the innermost open body in @ref stack. */
	unsigned int top;
	/**
	 * Slot 0 is the outermost message, so PB_TF_MAX_DEPTH submessages fit
	 * above it and the cap is reached when @ref top would exceed it.
	 */
	struct pb_tf_frame stack[PB_TF_MAX_DEPTH + 1U];
};

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
				   pb_size_t data_size, void *dest, const struct pb_tf_num *n,
				   const char *at)
{
	/*
	 * A field wider than this build's accumulator, i.e. an int64 field under
	 * PB_WITHOUT_64BIT. That is a configuration limit rather than a
	 * descriptor bug, so it earns the same class of answer as a float field
	 * under CONFIG_NANOPB_TEXTFORMAT_FLOAT=n. The chains below keep
	 * PB_TF_ERR_INTERNAL for a data_size that matches no integer width at
	 * all, which really would mean a descriptor mismatch.
	 */
	if (data_size > sizeof(pb_tf_uint64_t)) {
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

		if (data_size == sizeof(pb_tf_int64_t)) {
			clamped = *(pb_tf_int64_t *)dest = svalue;
		} else if (data_size == sizeof(int32_t)) {
			clamped = *(int32_t *)dest = (int32_t)svalue;
		} else if (data_size == sizeof(int_least16_t)) {
			clamped = *(int_least16_t *)dest = (int_least16_t)svalue;
		} else if (data_size == sizeof(int_least8_t)) {
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

	if (data_size == sizeof(pb_tf_uint64_t)) {
		clamped = *(pb_tf_uint64_t *)dest = value;
	} else if (data_size == sizeof(uint32_t)) {
		clamped = *(uint32_t *)dest = (uint32_t)value;
	} else if (data_size == sizeof(uint_least16_t)) {
		clamped = *(uint_least16_t *)dest = (uint_least16_t)value;
	} else if (data_size == sizeof(uint_least8_t)) {
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
				    pb_size_t data_size, void *dest)
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

	if (data_size != sizeof(bool)) {
		return pb_tf_fail(ps, PB_TF_ERR_INTERNAL, start, row->name);
	}

	*(bool *)dest = value;

	return PB_TF_OK;
}


/*
 * ---------------------------------------------------------------------------
 * Strings and bytes
 *
 * Unescaped straight into the destination field: there is no scratch buffer, so
 * a value that will not fit is refused at the byte that overflows rather than
 * after building it somewhere else first.
 * ---------------------------------------------------------------------------
 */

/** Where decoded bytes go, and how many the field can hold. */
struct pb_tf_sink {
	pb_byte_t *buf;
	size_t cap;
	size_t len;
};

static pb_tf_err_t pb_tf_sink_put(struct pb_tf_parser *ps, const pb_tf_field_t *row,
				  struct pb_tf_sink *sk, pb_byte_t b, const char *at)
{
	if (sk->len >= sk->cap) {
		return pb_tf_fail(ps, PB_TF_ERR_NO_SPACE, at, row->name);
	}

	sk->buf[sk->len] = b;
	sk->len++;

	return PB_TF_OK;
}

/**
 * Append a code point as UTF-8.
 *
 * \uXXXX tops out at U+FFFF, so three bytes is the most this can produce.
 * Surrogates are rejected by the caller, which keeps this from ever emitting
 * ill-formed UTF-8 -- and keeps a CONFIG_NANOPB_VALIDATE_UTF8 build from later
 * rejecting a struct this parser filled.
 */
static pb_tf_err_t pb_tf_put_utf8(struct pb_tf_parser *ps, const pb_tf_field_t *row,
				  struct pb_tf_sink *sk, uint32_t cp, const char *at)
{
	pb_tf_err_t err;

	if (cp < 0x80U) {
		return pb_tf_sink_put(ps, row, sk, (pb_byte_t)cp, at);
	}

	if (cp < 0x800U) {
		err = pb_tf_sink_put(ps, row, sk, (pb_byte_t)(0xC0U | (cp >> 6)), at);
		if (err != PB_TF_OK) {
			return err;
		}
		return pb_tf_sink_put(ps, row, sk, (pb_byte_t)(0x80U | (cp & 0x3FU)), at);
	}

	err = pb_tf_sink_put(ps, row, sk, (pb_byte_t)(0xE0U | (cp >> 12)), at);
	if (err != PB_TF_OK) {
		return err;
	}
	err = pb_tf_sink_put(ps, row, sk, (pb_byte_t)(0x80U | ((cp >> 6) & 0x3FU)), at);
	if (err != PB_TF_OK) {
		return err;
	}

	return pb_tf_sink_put(ps, row, sk, (pb_byte_t)(0x80U | (cp & 0x3FU)), at);
}

/**
 * Decode one escape sequence, the backslash already consumed.
 *
 * Handles \a \b \f \n \r \t \v \? \\ \' \" plus \xH[H], \NNN octal and
 * \uXXXX. Anything else -- including \U, which is out of scope -- is a syntax
 * error rather than a silently dropped backslash.
 */
static pb_tf_err_t pb_tf_scan_escape(struct pb_tf_parser *ps, const pb_tf_field_t *row,
				     struct pb_tf_sink *sk)
{
	const char *at = ps->p - 1;
	pb_byte_t b;
	uint8_t d;

	switch (*ps->p) {
	case 'a':
		b = (pb_byte_t)'\a';
		ps->p++;
		break;
	case 'b':
		b = (pb_byte_t)'\b';
		ps->p++;
		break;
	case 'f':
		b = (pb_byte_t)'\f';
		ps->p++;
		break;
	case 'n':
		b = (pb_byte_t)'\n';
		ps->p++;
		break;
	case 'r':
		b = (pb_byte_t)'\r';
		ps->p++;
		break;
	case 't':
		b = (pb_byte_t)'\t';
		ps->p++;
		break;
	case 'v':
		b = (pb_byte_t)'\v';
		ps->p++;
		break;
	case '?':
	case '\\':
	case '\'':
	case '"':
		b = (pb_byte_t)*ps->p;
		ps->p++;
		break;

	case 'x':
	case 'X': {
		/* One or two hex digits, per the spec's HEX_ESCAPE. */
		uint8_t v;

		ps->p++;
		if (char2hex(*ps->p, &d) != 0) {
			return pb_tf_fail(ps, PB_TF_ERR_SYNTAX, at, row->name);
		}
		v = d;
		ps->p++;

		if (char2hex(*ps->p, &d) == 0) {
			v = (uint8_t)((v << 4) | d);
			ps->p++;
		}

		b = (pb_byte_t)v;
		break;
	}

	case 'u': {
		uint32_t cp = 0U;
		unsigned int i;

		ps->p++;
		for (i = 0U; i < 4U; i++) {
			if (char2hex(*ps->p, &d) != 0) {
				return pb_tf_fail(ps, PB_TF_ERR_SYNTAX, at, row->name);
			}
			cp = (cp << 4) | (uint32_t)d;
			ps->p++;
		}

		/*
		 * A lone surrogate is rejected rather than encoded. Surrogate
		 * pairs are out of scope, so there is no way to reach a code
		 * point above U+FFFF and nothing here can produce ill-formed
		 * UTF-8.
		 */
		if ((cp >= 0xD800U) && (cp <= 0xDFFFU)) {
			return pb_tf_fail(ps, PB_TF_ERR_SYNTAX, at, row->name);
		}

		return pb_tf_put_utf8(ps, row, sk, cp, at);
	}

	default:
		if ((*ps->p >= '0') && (*ps->p <= '7')) {
			/* One to three octal digits. */
			unsigned int v = 0U;
			unsigned int i;

			for (i = 0U; (i < 3U) && (*ps->p >= '0') && (*ps->p <= '7'); i++) {
				v = (v * 8U) + (unsigned int)(*ps->p - '0');
				ps->p++;
			}

			if (v > 0xFFU) {
				return pb_tf_fail(ps, PB_TF_ERR_RANGE, at, row->name);
			}

			b = (pb_byte_t)v;
			break;
		}

		return pb_tf_fail(ps, PB_TF_ERR_SYNTAX, at, row->name);
	}

	return pb_tf_sink_put(ps, row, sk, b, at);
}

/**
 * Scan a quoted literal into @p sk.
 *
 * Either quote character opens a literal and the matching one closes it, so the
 * other appears inside as an ordinary byte.
 *
 * Every raw byte below 0x20 is refused. protoc certainly rejects an unescaped
 * newline; for the others -- a literal tab, say -- this is deliberately stricter
 * than protoc. Narrowing is always safe for a subset dialect, and it costs
 * nothing real here: the printer escapes control characters, so round-trip is
 * unaffected, and shell input never carries a raw tab past line editing.
 *
 * Adjacent string concatenation ("a" "b") is out of scope and needs no check
 * here -- the second literal simply is not a field name, so the field loop
 * reports it.
 */
static pb_tf_err_t pb_tf_scan_quoted(struct pb_tf_parser *ps, const pb_tf_field_t *row,
				     struct pb_tf_sink *sk)
{
	const char *start = ps->p;
	char quote;

	if ((*ps->p != '"') && (*ps->p != '\'')) {
		return pb_tf_fail(ps, PB_TF_ERR_TYPE, start, row->name);
	}

	quote = *ps->p;
	ps->p++;

	while (*ps->p != quote) {
		unsigned char c = (unsigned char)*ps->p;
		pb_tf_err_t err;

		if (c == 0U) {
			/* Unterminated literal. */
			return pb_tf_fail(ps, PB_TF_ERR_SYNTAX, ps->p, row->name);
		}

		if (c < 0x20U) {
			return pb_tf_fail(ps, PB_TF_ERR_SYNTAX, ps->p, row->name);
		}

		if (c == (unsigned char)'\\') {
			ps->p++;
			err = pb_tf_scan_escape(ps, row, sk);
		} else {
			err = pb_tf_sink_put(ps, row, sk, (pb_byte_t)c, ps->p);
			ps->p++;
		}

		if (err != PB_TF_OK) {
			return err;
		}
	}

	ps->p++;

	return PB_TF_OK;
}

/** A char array. nanopb's bound is size + 1 <= data_size, NUL always written. */
static pb_tf_err_t pb_tf_write_string(struct pb_tf_parser *ps, const pb_tf_field_t *row,
				      pb_size_t data_size, void *dest)
{
	struct pb_tf_sink sk;
	pb_tf_err_t err;

	if (data_size < 1U) {
		return pb_tf_fail(ps, PB_TF_ERR_INTERNAL, ps->p, row->name);
	}

	sk.buf = dest;
	sk.cap = (size_t)data_size - 1U;
	sk.len = 0U;

	err = pb_tf_scan_quoted(ps, row, &sk);
	if (err != PB_TF_OK) {
		return err;
	}

	sk.buf[sk.len] = 0U;

	return PB_TF_OK;
}

/**
 * A pb_bytes_array_t: a pb_size_t count followed by the payload.
 *
 * The capacity is data_size minus the header, exactly as nanopb computes it.
 * Note that this can exceed the .options max_size by whatever tail padding the
 * struct carries -- true of nanopb itself, so it is not advertised as exact.
 */
static pb_tf_err_t pb_tf_write_bytes(struct pb_tf_parser *ps, const pb_tf_field_t *row,
				     pb_size_t data_size, void *dest)
{
	pb_bytes_array_t *arr = dest;
	struct pb_tf_sink sk;
	pb_tf_err_t err;

	if ((size_t)data_size <= offsetof(pb_bytes_array_t, bytes)) {
		return pb_tf_fail(ps, PB_TF_ERR_INTERNAL, ps->p, row->name);
	}

	sk.buf = arr->bytes;
	sk.cap = (size_t)data_size - offsetof(pb_bytes_array_t, bytes);
	sk.len = 0U;

	err = pb_tf_scan_quoted(ps, row, &sk);
	if (err != PB_TF_OK) {
		return err;
	}

	arr->size = (pb_size_t)sk.len;

	return PB_TF_OK;
}

/**
 * A fixed_length_bytes field: a bare array with no count.
 *
 * nanopb accepts either exactly data_size bytes or an empty value, which it
 * treats as all zeros (pb_decode.c:1684-1692). Anything in between is refused
 * as PB_TF_ERR_TYPE -- the literal is the wrong shape for the field, and
 * PB_TF_ERR_NO_SPACE would claim it was too long when it is too short.
 */
static pb_tf_err_t pb_tf_write_fixed_bytes(struct pb_tf_parser *ps, const pb_tf_field_t *row,
					   pb_size_t data_size, void *dest)
{
	const char *start = ps->p;
	struct pb_tf_sink sk;
	pb_tf_err_t err;

	sk.buf = dest;
	sk.cap = (size_t)data_size;
	sk.len = 0U;

	err = pb_tf_scan_quoted(ps, row, &sk);
	if (err != PB_TF_OK) {
		return err;
	}

	if (sk.len == 0U) {
		(void)memset(dest, 0, data_size);
		return PB_TF_OK;
	}

	if (sk.len != (size_t)data_size) {
		return pb_tf_fail(ps, PB_TF_ERR_TYPE, start, row->name);
	}

	return PB_TF_OK;
}

/*
 * ---------------------------------------------------------------------------
 * Floats
 * ---------------------------------------------------------------------------
 */

#if defined(CONFIG_NANOPB_TEXTFORMAT_FLOAT)

/** Case-insensitive compare of a non-NUL-terminated ident against a lowercase literal. */
static bool pb_tf_ident_ieq(const char *s, size_t len, const char *lit)
{
	size_t i;

	if (strlen(lit) != len) {
		return false;
	}

	for (i = 0U; i < len; i++) {
		char a = s[i];

		if ((a >= 'A') && (a <= 'Z')) {
			a = (char)(a + ('a' - 'A'));
		}
		if (a != lit[i]) {
			return false;
		}
	}

	return true;
}

static bool pb_tf_is_digit(char c)
{
	return (c >= '0') && (c <= '9');
}

/**
 * End of the decimal float literal starting at @p p, or NULL if there is none.
 *
 * This exists because strtod() accepts a good deal more than text format does --
 * hex floats such as 0x1p3, a leading '+', its own inf/nan spellings including
 * nan(chars). Rather than trust it, the shape is validated here and strtod()'s
 * endptr is required to agree, which keeps the dialect a strict subset no matter
 * what the C library is willing to swallow.
 *
 * The 'f' suffix is deliberately not consumed: strtod() would not accept it, so
 * it is handled by the caller after the endptr check.
 */
static const char *pb_tf_float_extent(const char *p)
{
	const char *q = p;
	bool digits = false;

	if (*q == '-') {
		q++;
	}

	while (pb_tf_is_digit(*q)) {
		q++;
		digits = true;
	}

	if (*q == '.') {
		q++;
		while (pb_tf_is_digit(*q)) {
			q++;
			digits = true;
		}
	}

	if (!digits) {
		return NULL;
	}

	if ((*q == 'e') || (*q == 'E')) {
		const char *save = q;

		q++;
		if ((*q == '+') || (*q == '-')) {
			q++;
		}

		if (!pb_tf_is_digit(*q)) {
			/* "1e" is the literal 1 followed by junk, not a float. */
			q = save;
		} else {
			while (pb_tf_is_digit(*q)) {
				q++;
			}
		}
	}

	return q;
}

/**
 * Scan a float literal.
 *
 * Accepts the decimal forms, an optional f/F suffix, a bare integer, and
 * inf / infinity / nan case-insensitively with an optional leading sign --
 * matching what protoc accepts and what this library's own printer emits.
 *
 * @param special set when the value is an infinity or NaN, so the caller knows
 *                to skip the finite range check.
 */
static pb_tf_err_t pb_tf_scan_float(struct pb_tf_parser *ps, const pb_tf_field_t *row, double *out,
				    bool *special)
{
	const char *start = ps->p;
	const char *end;
	const char *q = ps->p;
	char *ep = NULL;
	bool neg = false;
	double v;

	*special = false;

	if (*q == '-') {
		neg = true;
		q++;
	}

	if (pb_tf_is_ident_start(*q)) {
		const char *w = q;
		size_t len;

		while (pb_tf_is_ident_char(*q)) {
			q++;
		}
		len = (size_t)(q - w);

		if (pb_tf_ident_ieq(w, len, "inf") || pb_tf_ident_ieq(w, len, "infinity")) {
			*out = neg ? -INFINITY : INFINITY;
			*special = true;
			ps->p = q;
			return PB_TF_OK;
		}

		if (pb_tf_ident_ieq(w, len, "nan")) {
			/* A signed NaN is still a NaN; the sign is not meaningful. */
			*out = (double)NAN;
			*special = true;
			ps->p = q;
			return PB_TF_OK;
		}

		return pb_tf_fail(ps, PB_TF_ERR_TYPE, start, row->name);
	}

	end = pb_tf_float_extent(ps->p);
	if (end == NULL) {
		return pb_tf_fail(ps, PB_TF_ERR_SYNTAX, start, row->name);
	}

	v = strtod(ps->p, &ep);

	/*
	 * If strtod() consumed a different span than the grammar allows, the input
	 * was something it accepts and text format does not -- a hex float, say.
	 */
	if (ep != end) {
		return pb_tf_fail(ps, PB_TF_ERR_TYPE, start, row->name);
	}

	ps->p = end;

	/* The optional float suffix, which strtod() leaves behind. */
	if ((*ps->p == 'f') || (*ps->p == 'F')) {
		ps->p++;
	}

	*out = v;

	return PB_TF_OK;
}

static pb_tf_err_t pb_tf_write_float(struct pb_tf_parser *ps, const pb_tf_field_t *row,
				     pb_size_t data_size, void *dest)
{
	const char *start = ps->p;
	bool special;
	double v;
	pb_tf_err_t err = pb_tf_scan_float(ps, row, &v, &special);

	if (err != PB_TF_OK) {
		return err;
	}

	/*
	 * A literal that overflowed the double itself -- 1e309, say -- comes back
	 * from strtod() as an infinity with ERANGE. Storing that would silently
	 * turn a typo into an infinity, so only the inf/nan *words* may produce
	 * one.
	 */
	if (!special && isinf(v)) {
		return pb_tf_fail(ps, PB_TF_ERR_RANGE, start, row->name);
	}

	/*
	 * A proto double is stored as a C float when PB_CONVERT_DOUBLE_FLOAT is
	 * set, so the width decides here, not the vtype.
	 */
	if (data_size == sizeof(float)) {
		/*
		 * Narrowing a finite value that exceeds FLT_MAX would quietly
		 * become an infinity, so it is refused instead. Underflow to zero
		 * or a subnormal is left alone, as protoc leaves it.
		 */
		if (!special && ((v > (double)FLT_MAX) || (v < -(double)FLT_MAX))) {
			return pb_tf_fail(ps, PB_TF_ERR_RANGE, start, row->name);
		}
		*(float *)dest = (float)v;
		return PB_TF_OK;
	}

	if (data_size == sizeof(double)) {
		*(double *)dest = v;
		return PB_TF_OK;
	}

	return pb_tf_fail(ps, PB_TF_ERR_INTERNAL, start, row->name);
}

#else /* !CONFIG_NANOPB_TEXTFORMAT_FLOAT */

/*
 * With float support off, neither strtod() nor any floating-point formatting is
 * referenced, so neither is linked in. A float field is a clean runtime error.
 */
static pb_tf_err_t pb_tf_write_float(struct pb_tf_parser *ps, const pb_tf_field_t *row,
				     pb_size_t data_size, void *dest)
{
	ARG_UNUSED(data_size);
	ARG_UNUSED(dest);

	return pb_tf_fail(ps, PB_TF_ERR_NO_FLOAT, ps->p, row->name);
}

#endif /* CONFIG_NANOPB_TEXTFORMAT_FLOAT */

/** Parse one non-submessage value of @p row's type into @p dest. */
static pb_tf_err_t pb_tf_parse_scalar(struct pb_tf_parser *ps, const pb_tf_field_t *row,
				      pb_size_t data_size, void *dest)
{
	const char *start = ps->p;
	struct pb_tf_num n;
	pb_tf_err_t err;

	switch (row->vtype) {
	case PB_TF_VT_BOOL:
		return pb_tf_write_bool(ps, row, data_size, dest);

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
		return pb_tf_write_int(ps, row, data_size, dest, &n, start);

	case PB_TF_VT_FLOAT:
	case PB_TF_VT_DOUBLE:
		return pb_tf_write_float(ps, row, data_size, dest);

	case PB_TF_VT_STRING:
		return pb_tf_write_string(ps, row, data_size, dest);

	case PB_TF_VT_BYTES:
		return pb_tf_write_bytes(ps, row, data_size, dest);

	case PB_TF_VT_FIXED_LENGTH_BYTES:
		return pb_tf_write_fixed_bytes(ps, row, data_size, dest);

	default:
		/* MESSAGE and MSG_W_CB never reach here; EXTENSION is unreachable. */
		return pb_tf_fail(ps, PB_TF_ERR_INTERNAL, start, row->name);
	}
}

/*
 * ---------------------------------------------------------------------------
 * Field storage
 * ---------------------------------------------------------------------------
 */

/** Where one occurrence of a field is to be written, and what it owes on success. */
struct pb_tf_slot {
	void *dest;
	pb_size_t data_size;
	void *commit_ptr;
	pb_size_t commit_val;
	uint8_t commit_kind;
};

/**
 * Resolve @p row against @p msg: find the destination for one occurrence and
 * work out what completing it must record.
 *
 * The iterator is resolved per occurrence rather than cached, which keeps a
 * field mentioned several times correct and costs only a short scan.
 */
static pb_tf_err_t pb_tf_resolve_slot(struct pb_tf_parser *ps, const pb_tf_msg_t *tf, void *msg,
				      const pb_tf_field_t *row, struct pb_tf_slot *out)
{
	pb_field_iter_t it;

	if (!pb_field_iter_begin(&it, tf->desc, msg) || !pb_field_iter_find(&it, row->tag)) {
		return pb_tf_fail(ps, PB_TF_ERR_INTERNAL, ps->p, row->name);
	}

	out->data_size = it.data_size;
	out->commit_ptr = NULL;
	out->commit_val = 0U;
	out->commit_kind = PB_TF_CM_NONE;

	switch (PB_TF_HT(row->rules)) {
	case PB_TF_HT_REQUIRED:
	case PB_TF_HT_SINGULAR:
		/*
		 * pSize is NULL for these -- pb_common.c leaves it unset when
		 * size_offset is 0 -- so it must not be touched. Repeating such
		 * a field overwrites, which for a submessage is the merge
		 * semantics text format asks for.
		 */
		out->dest = it.pData;
		break;

	case PB_TF_HT_OPTIONAL:
		if (it.pSize == NULL) {
			return pb_tf_fail(ps, PB_TF_ERR_INTERNAL, ps->p, row->name);
		}
		out->dest = it.pData;
		out->commit_ptr = it.pSize;
		out->commit_kind = PB_TF_CM_PRESENT;
		break;

	case PB_TF_HT_REPEATED: {
		pb_size_t n;

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
		out->dest = (char *)it.pData + ((size_t)n * (size_t)it.data_size);
		(void)memset(out->dest, 0, it.data_size);
		out->commit_ptr = it.pSize;
		out->commit_val = (pb_size_t)(n + 1U);
		out->commit_kind = PB_TF_CM_SIZE;
		break;
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
		out->dest = it.pData;
		out->commit_ptr = it.pSize;
		out->commit_val = (pb_size_t)row->tag;
		out->commit_kind = PB_TF_CM_SIZE;
		break;

	default:
		/* FIXARRAY; pb_tf_shape_supported() rejects it before we get here. */
		return pb_tf_fail(ps, PB_TF_ERR_UNSUPPORTED, ps->p, row->name);
	}

	return PB_TF_OK;
}

/** Record a completed field. A no-op for SINGULAR and REQUIRED. */
static void pb_tf_commit(void *ptr, pb_size_t val, uint8_t kind)
{
	switch (kind) {
	case PB_TF_CM_PRESENT:
		*(bool *)ptr = true;
		break;
	case PB_TF_CM_SIZE:
		*(pb_size_t *)ptr = val;
		break;
	default:
		break;
	}
}

/**
 * Open a submessage body as a new frame.
 *
 * The depth check comes before the brace check so that an over-deep input
 * reports PB_TF_ERR_DEPTH rather than a syntax error, and before any state
 * changes so the frame stack cannot be left half-pushed.
 */
static pb_tf_err_t pb_tf_push(struct pb_tf_parser *ps, const pb_tf_field_t *row,
			      const struct pb_tf_slot *slot)
{
	struct pb_tf_frame *fr;

	if (row->sub == NULL) {
		/* PB_TF_DEFINE fills this in for every MESSAGE/MSG_W_CB row. */
		return pb_tf_fail(ps, PB_TF_ERR_INTERNAL, ps->p, row->name);
	}

	if (ps->top >= (unsigned int)PB_TF_MAX_DEPTH) {
		return pb_tf_fail(ps, PB_TF_ERR_DEPTH, ps->p, row->name);
	}

	/* The colon is optional before a submessage; the braces are not. */
	if (*ps->p != '{') {
		return pb_tf_fail(ps, PB_TF_ERR_SYNTAX, ps->p, row->name);
	}
	ps->p++;

	ps->top++;
	fr = &ps->stack[ps->top];
	fr->tf = row->sub;
	fr->msg = slot->dest;
	fr->row = NULL;
	fr->commit_ptr = slot->commit_ptr;
	fr->commit_val = slot->commit_val;
	fr->commit_kind = slot->commit_kind;
	fr->flags = PB_TF_FR_BRACED;

	/*
	 * Note what is deliberately absent: the body is not cleared. A singular
	 * or optional submessage mentioned twice therefore merges, which is what
	 * text format asks for; a repeated one was cleared per element by
	 * pb_tf_resolve_slot().
	 */

	return PB_TF_OK;
}

/*
 * ---------------------------------------------------------------------------
 * The state machine
 *
 * Three states cycle per field: FIELD reads a name and decides whether a list
 * follows, VALUE stores one value or opens a submessage, and AFTER handles the
 * list and field separators. A submessage pushes a frame and re-enters FIELD;
 * its closing brace commits, pops, and resumes the parent in AFTER -- which is
 * exactly where a returning recursive call would have landed.
 * ---------------------------------------------------------------------------
 */

#define PB_TF_ST_FIELD 0U
#define PB_TF_ST_VALUE 1U
#define PB_TF_ST_AFTER 2U

static pb_tf_err_t pb_tf_run(struct pb_tf_parser *ps)
{
	unsigned int state = PB_TF_ST_FIELD;

	while (true) {
		struct pb_tf_frame *fr = &ps->stack[ps->top];
		pb_tf_err_t err;

		switch (state) {
		case PB_TF_ST_FIELD: {
			const char *fstart;
			size_t nlen;
			const pb_tf_field_t *row;

			pb_tf_skip_ws(ps);

			if (*ps->p == '}') {
				if ((fr->flags & PB_TF_FR_BRACED) == 0U) {
					return pb_tf_fail(ps, PB_TF_ERR_SYNTAX, ps->p, NULL);
				}
				ps->p++;

				if (ps->top == 0U) {
					/* Outermost body; the caller checks for trailing junk. */
					return PB_TF_OK;
				}

				pb_tf_commit(fr->commit_ptr, fr->commit_val, fr->commit_kind);
				ps->top--;
				state = PB_TF_ST_AFTER;
				break;
			}

			if (*ps->p == '\0') {
				if ((fr->flags & PB_TF_FR_BRACED) != 0U) {
					/* Unterminated body. */
					return pb_tf_fail(ps, PB_TF_ERR_SYNTAX, ps->p, NULL);
				}
				/* Only the outermost body may be unbraced. */
				return PB_TF_OK;
			}

			fstart = ps->p;
			nlen = pb_tf_scan_ident(ps);
			if (nlen == 0U) {
				return pb_tf_fail(ps, PB_TF_ERR_SYNTAX, fstart, NULL);
			}

			row = pb_tf_field_find(fr->tf, fstart, nlen);
			if (row == NULL) {
				return pb_tf_fail(ps, PB_TF_ERR_UNKNOWN_FIELD, fstart, NULL);
			}

			/* POINTER, CALLBACK, FIXARRAY and EXTENSION, as a runtime error. */
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

			fr->row = row;
			fr->flags &= (uint8_t)~PB_TF_FR_IN_LIST;
			state = PB_TF_ST_VALUE;

			if (*ps->p == '[') {
				if (PB_TF_HT(row->rules) != PB_TF_HT_REPEATED) {
					return pb_tf_fail(ps, PB_TF_ERR_SYNTAX, ps->p, row->name);
				}

				ps->p++;
				pb_tf_skip_ws(ps);

				if (*ps->p == ']') {
					/* "x: []" is legal and appends nothing. */
					ps->p++;
					state = PB_TF_ST_AFTER;
				} else {
					fr->flags |= PB_TF_FR_IN_LIST;
				}
			}
			break;
		}

		case PB_TF_ST_VALUE: {
			struct pb_tf_slot slot;

			err = pb_tf_resolve_slot(ps, fr->tf, fr->msg, fr->row, &slot);
			if (err != PB_TF_OK) {
				return err;
			}

			if (pb_tf_vtype_is_msg(fr->row->vtype)) {
				/*
				 * MSG_W_CB is MESSAGE with a pb_callback_t ahead
				 * of it in the struct. nanopb emits that member
				 * before the has_ bit and derives size_offset
				 * from the latter, so the callback sits strictly
				 * below pSize and writing through pData can
				 * never disturb it.
				 */
				err = pb_tf_push(ps, fr->row, &slot);
				if (err != PB_TF_OK) {
					return err;
				}
				state = PB_TF_ST_FIELD;
				break;
			}

			err = pb_tf_parse_scalar(ps, fr->row, slot.data_size, slot.dest);
			if (err != PB_TF_OK) {
				return err;
			}

			pb_tf_commit(slot.commit_ptr, slot.commit_val, slot.commit_kind);
			state = PB_TF_ST_AFTER;
			break;
		}

		case PB_TF_ST_AFTER:
		default:
			if ((fr->flags & PB_TF_FR_IN_LIST) != 0U) {
				pb_tf_skip_ws(ps);

				if (*ps->p == ',') {
					ps->p++;
					pb_tf_skip_ws(ps);
					state = PB_TF_ST_VALUE;
					break;
				}

				if (*ps->p != ']') {
					return pb_tf_fail(ps, PB_TF_ERR_SYNTAX, ps->p,
							  fr->row->name);
				}

				ps->p++;
				fr->flags &= (uint8_t)~PB_TF_FR_IN_LIST;
			}

			/* Field separators are optional in text format. */
			pb_tf_skip_ws(ps);
			if ((*ps->p == ',') || (*ps->p == ';')) {
				ps->p++;
			}

			state = PB_TF_ST_FIELD;
			break;
		}
	}
}

pb_tf_err_t pb_tf_parse(const pb_tf_msg_t *tf, void *msg, const char *text, unsigned int flags,
			pb_tf_status_t *status)
{
	struct pb_tf_parser ps;
	struct pb_tf_frame *base;
	pb_tf_err_t err;

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
	ps.top = 0U;

	if ((flags & PB_TF_NOINIT) == 0U) {
		pb_tf_clear(tf, msg);
	}

	base = &ps.stack[0];
	base->tf = tf;
	base->msg = msg;
	base->row = NULL;
	base->commit_ptr = NULL;
	base->commit_val = 0U;
	base->commit_kind = PB_TF_CM_NONE;
	base->flags = 0U;

	pb_tf_skip_ws(&ps);

	/*
	 * Outer braces are accepted but not required. '{' can never legally start
	 * a top-level text-format Message -- a FieldName is an identifier or '['
	 * -- so this is unambiguous, provided the matching '}' has to be the last
	 * token. That is what the trailing-junk check below enforces.
	 */
	if (*ps.p == '{') {
		base->flags = PB_TF_FR_BRACED;
		ps.p++;
	}

	err = pb_tf_run(&ps);
	if (err != PB_TF_OK) {
		return err;
	}

	pb_tf_skip_ws(&ps);
	if (*ps.p != '\0') {
		return pb_tf_fail(&ps, PB_TF_ERR_SYNTAX, ps.p, NULL);
	}

	return PB_TF_OK;
}
