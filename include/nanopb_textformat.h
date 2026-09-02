/*
 * Copyright (c) 2026 Rodrigo Peixoto
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Protobuf text format for nanopb messages.
 *
 * Parses a subset of the protobuf text format
 * (https://protobuf.dev/reference/protobuf/textformat-spec/) directly into a
 * nanopb-generated struct, and prints such a struct back out. No heap, no
 * scratch buffers: strings and bytes are unescaped straight into the
 * destination field.
 *
 * nanopb descriptors carry no field names (see @c pb_msgdesc_t), so this file
 * builds a companion descriptor from nanopb's own generated
 * <tt><MSG>_FIELDLIST</tt> X-macro. One @ref PB_TF_DEFINE per message, at file
 * scope, next to its @c PB_BIND:
 *
 * @code
 * PB_TF_DEFINE(UI_CONFIG, ui_config_t);   // --c-style naming
 * PB_TF_DEFINE(UiConfig, UiConfig);       // default nanopb naming
 * @endcode
 *
 * See docs/dialect.md for exactly which parts of the spec are supported.
 */

#ifndef NANOPB_TEXTFORMAT_H_
#define NANOPB_TEXTFORMAT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pb.h>
#include <zephyr/sys/cbprintf.h>
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @name Version
 *
 * Semantic versioning (https://semver.org). While the major version is 0 the
 * API may still change; see the Versioning section of README.md for what counts
 * as a breaking change here.
 */
/**@{*/
#define NANOPB_TEXTFORMAT_VERSION_MAJOR 0
#define NANOPB_TEXTFORMAT_VERSION_MINOR 7
#define NANOPB_TEXTFORMAT_VERSION_PATCH 0

#define NANOPB_TEXTFORMAT_VERSION_STRING "0.7.0"

/**
 * @brief Version as @c 0xMMmmpp, for comparison.
 *
 * @code
 * #if NANOPB_TEXTFORMAT_VERSION >= 0x000200
 * @endcode
 */
#define NANOPB_TEXTFORMAT_VERSION                                                                  \
	(((NANOPB_TEXTFORMAT_VERSION_MAJOR) << 16) | ((NANOPB_TEXTFORMAT_VERSION_MINOR) << 8) |     \
	 (NANOPB_TEXTFORMAT_VERSION_PATCH))
/**@}*/

/**
 * @brief Value type of a field.
 *
 * These mirror the ltype tokens in nanopb's generated @c FIELDLIST rows, and
 * deliberately keep distinctions that nanopb's runtime @c pb_type_t throws
 * away: @c PB_LTYPE_FIXED32 covers @c float, @c fixed32 and @c sfixed32
 * alike, so a runtime-only design cannot tell whether to print @c 1.0 or
 * @c 1. The enumerator names must match the FIELDLIST tokens exactly, because
 * PB_TF_GEN_ROW pastes them.
 */
enum pb_tf_vtype {
	PB_TF_VT_BOOL = 0,
	PB_TF_VT_BYTES,
	PB_TF_VT_DOUBLE,
	PB_TF_VT_ENUM,
	PB_TF_VT_UENUM,
	PB_TF_VT_FIXED32,
	PB_TF_VT_FIXED64,
	PB_TF_VT_FLOAT,
	PB_TF_VT_INT32,
	PB_TF_VT_INT64,
	PB_TF_VT_MESSAGE,
	PB_TF_VT_MSG_W_CB,
	PB_TF_VT_SFIXED32,
	PB_TF_VT_SFIXED64,
	PB_TF_VT_SINT32,
	PB_TF_VT_SINT64,
	PB_TF_VT_STRING,
	PB_TF_VT_UINT32,
	PB_TF_VT_UINT64,
	PB_TF_VT_EXTENSION,
	PB_TF_VT_FIXED_LENGTH_BYTES,
};

/** @name Allocation type, from the FIELDLIST atype token. */
/**@{*/
#define PB_TF_AT_STATIC   0x00U
#define PB_TF_AT_POINTER  0x40U
#define PB_TF_AT_CALLBACK 0x80U
#define PB_TF_AT_MASK     0xC0U
/**@}*/

/**
 * @name Repetition rule, from the FIELDLIST htype token.
 *
 * Kept separate from nanopb's @c PB_HTYPE_* because those alias:
 * @c PB_HTYPE_OPTIONAL == @c PB_HTYPE_SINGULAR and
 * @c PB_HTYPE_REPEATED == @c PB_HTYPE_FIXARRAY, so the distinction survives
 * only in the macro token.
 */
/**@{*/
#define PB_TF_HT_REQUIRED 0x00U
#define PB_TF_HT_SINGULAR 0x01U
#define PB_TF_HT_OPTIONAL 0x02U
#define PB_TF_HT_REPEATED 0x03U
#define PB_TF_HT_FIXARRAY 0x04U
#define PB_TF_HT_ONEOF    0x05U
#define PB_TF_HT_MASK     0x07U
/**@}*/

#define PB_TF_AT(rules) ((rules) & PB_TF_AT_MASK)
#define PB_TF_HT(rules) ((rules) & PB_TF_HT_MASK)

struct pb_tf_msg;

/** @brief One field of a message: its text-format name plus what nanopb omits. */
struct pb_tf_field {
	/** Field name as written in the .proto (from @c #fieldname). */
	const char *name;
	/** Protobuf field number. */
	uint16_t tag;
	/** One of @ref pb_tf_vtype. */
	uint8_t vtype;
	/** @c PB_TF_AT_* | @c PB_TF_HT_* */
	uint8_t rules;
	/** Submessage descriptor for MESSAGE/MSG_W_CB fields, else NULL. */
	const struct pb_tf_msg *sub;
};

/** @brief Text-format descriptor for one message type. */
struct pb_tf_msg {
	/** nanopb's own descriptor, for @c pb_field_iter_begin(). */
	const pb_msgdesc_t *desc;
	/** @ref count entries, in tag order, matching @c desc->field_info. */
	const struct pb_tf_field *fields;
	/** Number of real fields (the terminator row is not counted). */
	uint16_t count;
	/** @c sizeof() the C struct, so pb_tf_clear() can zero it. */
	uint16_t struct_size;
};


/**
 * @brief Outcome of a parse or print.
 *
 * Every function here returns @c int, following Zephyr's convention: **0 on
 * success, or one of these negated on failure**. The enumerators themselves are
 * positive, so a caller writes
 *
 * @code
 * int err = pb_tf_parse(&cfg_tf, &cfg, text, &st);
 *
 * if (err == -PB_TF_ERR_RANGE) {
 *         ...
 * }
 * @endcode
 *
 * and @c pb_tf_strerror() accepts either sign, so a return value or a
 * @c status.code can be passed to it directly.
 *
 * Append new codes at the end. Renumbering these changes the meaning of a value
 * a consumer may have stored.
 */
enum pb_tf_err {
	PB_TF_OK = 0,
	/** Unexpected token, or unterminated brace/string/list. */
	PB_TF_ERR_SYNTAX,
	/** No field of that name in this message. */
	PB_TF_ERR_UNKNOWN_FIELD,
	/** Literal is the wrong shape for the field's type. */
	PB_TF_ERR_TYPE,
	/** Numeric literal does not fit the field's width. */
	PB_TF_ERR_RANGE,
	/** Repeated field already holds @c array_size elements. */
	PB_TF_ERR_TOO_MANY,
	/** String or bytes value longer than the destination buffer. */
	PB_TF_ERR_NO_SPACE,
	/** Nesting deeper than @c CONFIG_NANOPB_TEXTFORMAT_MAX_DEPTH. */
	PB_TF_ERR_DEPTH,
	/** Field shape this library does not handle (POINTER, CALLBACK, ...). */
	PB_TF_ERR_UNSUPPORTED,
	/** Float field, but CONFIG_NANOPB_TEXTFORMAT_FLOAT is disabled. */
	PB_TF_ERR_NO_FLOAT,
	/** Descriptor inconsistency; a bug here or a nanopb mismatch. */
	PB_TF_ERR_INTERNAL,
};


/** @brief Where and in which field an error happened. */
struct pb_tf_status {
	/** 0 on success, or a negative @ref pb_tf_err. */
	int code;
	/** Byte offset into the input text. */
	uint16_t offset;
	/** Field name if one was in scope, else NULL. */
	const char *field;
};


/*
 * There are no runtime flags. Output style and the merge-versus-replace choice
 * used to be a `flags` argument; they are Kconfig options and a second function
 * now, so a build states its intent once instead of at every call site:
 *
 *   CONFIG_NANOPB_TEXTFORMAT_PRINT_SKIP_DEFAULTS
 *
 * pb_tf_parse() versus pb_tf_merge(), and the line style and braces by function
 * name -- pb_tf_print_compact(), pb_tf_print_multiline_braces() and so on.
 *
 * Either direction can also be left out entirely:
 *
 *   CONFIG_NANOPB_TEXTFORMAT_PARSE   pb_tf_parse(), pb_tf_merge()
 *   CONFIG_NANOPB_TEXTFORMAT_PRINT   pb_tf_print(), pb_tf_print_buf()
 *
 * Both default to y. The declarations below are guarded on them, so calling
 * into a half that was configured out is a compile error naming the function
 * rather than a link failure much later.
 */

/**
 * @brief Parse text format into a nanopb struct.
 *
 * Clears @p msg first. Note that clearing means zeroing, not applying proto2
 * defaults; use pb_tf_merge() over an already-initialised struct for those.
 *
 * @param tf     descriptor from PB_TF_DEFINE, e.g. @c &ui_config_t_tf
 * @param msg    the struct to fill
 * @param text   NUL-terminated text format; outer braces are optional
 * @param status filled in on both success and failure; may be NULL
 *
 * @return PB_TF_OK, or the first error encountered.
 */
#if defined(CONFIG_NANOPB_TEXTFORMAT_PARSE)
int pb_tf_parse(const struct pb_tf_msg *tf, void *msg, const char *text,
			struct pb_tf_status *status);

/**
 * @brief Parse text format into a nanopb struct without clearing it first.
 *
 * As pb_tf_parse(), except that @p msg keeps what it already held: scalars
 * mentioned in @p text are overwritten, repeated fields are appended to, and
 * everything else is left alone.
 *
 * This is also how to get proto2 defaults: initialise @p msg with
 * @c <MSG>_init_default yourself, then merge over it.
 */
int pb_tf_merge(const struct pb_tf_msg *tf, void *msg, const char *text,
			struct pb_tf_status *status);
#endif /* CONFIG_NANOPB_TEXTFORMAT_PARSE */

#if defined(CONFIG_NANOPB_TEXTFORMAT_PRINT)
/**
 * @brief Print a nanopb struct as text format, in an explicitly given style.
 *
 * Output is produced character by character through @p out, so there is no
 * length limit and no temporary buffer. @c cbprintf_cb is Zephyr's own callback
 * type; pb_tf_out_buf() is a ready-made one that fills a buffer.
 *
 * Prefer one of the four style-named wrappers below: two bare booleans at a
 * call site say nothing about what comes out. Reach for this form when the
 * style is chosen at run time -- a shell command offering
 * @c "get [compact] [braces]", say -- where the alternative is a four-way
 * branch picking among the wrappers.
 *
 * @param tf       descriptor from PB_TF_DEFINE
 * @param msg      the struct to print
 * @param out      character sink
 * @param ctx      passed to @p out
 * @param compact  one line, rather than one field per line
 * @param braces   wrap the whole message in @c { }
 *
 * @return 0, or a negated @ref pb_tf_err -- the first error encountered.
 */
int pb_tf_print_styled(const struct pb_tf_msg *tf, const void *msg, cbprintf_cb out, void *ctx,
		       bool compact, bool braces);

/**
 * @name Printing in a style named at the call site
 *
 * Each names the output it produces, so a firmware can use more than one shape
 * -- multi-line for a person reading a configuration, compact for a line it
 * means to paste back in, log, or send somewhere -- and a call site says which
 * it is. There is deliberately no style-agnostic pb_tf_print(): its output
 * would depend on a Kconfig the reader has to go and look up.
 *
 * These are @c static @c inline and so generate no function of their own: the
 * call site materialises the two style arguments and calls
 * pb_tf_print_styled() directly, which is why having four of them costs
 * nothing.
 *
 * @c CONFIG_NANOPB_TEXTFORMAT_PRINT_SKIP_DEFAULTS remains Kconfig-only: unlike
 * the line style it is not something one firmware plausibly wants both ways.
 */
/**@{*/
static inline int pb_tf_print_compact(const struct pb_tf_msg *tf, const void *msg, cbprintf_cb out,
				      void *ctx)
{
	return pb_tf_print_styled(tf, msg, out, ctx, true, false);
}

static inline int pb_tf_print_multiline(const struct pb_tf_msg *tf, const void *msg,
					cbprintf_cb out, void *ctx)
{
	return pb_tf_print_styled(tf, msg, out, ctx, false, false);
}

static inline int pb_tf_print_compact_braces(const struct pb_tf_msg *tf, const void *msg,
					     cbprintf_cb out, void *ctx)
{
	return pb_tf_print_styled(tf, msg, out, ctx, true, true);
}

static inline int pb_tf_print_multiline_braces(const struct pb_tf_msg *tf, const void *msg,
					       cbprintf_cb out, void *ctx)
{
	return pb_tf_print_styled(tf, msg, out, ctx, false, true);
}
/**@}*/
#endif

/** @brief Zero a message, the way pb_tf_parse() does by default. */
void pb_tf_clear(const struct pb_tf_msg *tf, void *msg);

/**
 * @brief Human-readable text for an error code. Never NULL.
 *
 * The strings name only the fault, so a caller can append context without
 * repeating a word:
 *
 * @code
 * printk("at offset %u: %s", st.offset, pb_tf_strerror(err));
 * if (st.field != NULL) {
 *         printk(" in field '%s'", st.field);
 * }
 * // -> "at offset 14: value out of range in field 'threshold'"
 * @endcode
 */
const char *pb_tf_strerror(int err);

/** @brief Context for pb_tf_out_buf(). */
struct pb_tf_buf {
	char *buf;
	size_t size;
	/** Bytes written, excluding the NUL. */
	size_t used;
	/** Set once output was truncated. */
	bool overflow;
};


/**
 * @brief A @c cbprintf_cb that appends into a @ref pb_tf_buf.
 *
 * Terminates the buffer as it writes, and sets @c overflow instead of writing
 * past the end.
 *
 * Note the consequence of "as it writes": a message that prints nothing at all
 * -- an empty message, or one whose every field is skipped under
 * CONFIG_NANOPB_TEXTFORMAT_PRINT_SKIP_DEFAULTS -- never calls this, so the
 * buffer is left exactly as
 * the caller supplied it. Either pre-terminate it, or use pb_tf_print_buf(),
 * which handles that case.
 */
int pb_tf_out_buf(int c, void *ctx);

#if defined(CONFIG_NANOPB_TEXTFORMAT_PRINT)
/**
 * @brief Print into a caller-supplied buffer, in an explicitly given style.
 *
 * Wraps pb_tf_print_styled() and pb_tf_out_buf() for the common case, and
 * differs from using them directly in two ways worth having: @p buf is
 * NUL-terminated even when nothing is printed, and truncation comes back as
 * @c PB_TF_ERR_NO_SPACE rather than a flag the caller must remember to check.
 *
 * As above, prefer a style-named wrapper unless the style is a run-time choice.
 *
 * @param tf       descriptor from PB_TF_DEFINE
 * @param msg      the struct to print
 * @param buf      destination, always NUL-terminated on return
 * @param size     bytes available in @p buf, including the terminator
 * @param compact  one line, rather than one field per line
 * @param braces   wrap the whole message in @c { }
 *
 * @return 0, @c -PB_TF_ERR_NO_SPACE if the output was truncated, or the first
 *         print error, negated.
 */
int pb_tf_print_buf_styled(const struct pb_tf_msg *tf, const void *msg, char *buf, size_t size,
			   bool compact, bool braces);

/** @name As pb_tf_print_buf_styled(), in a style named at the call site. */
/**@{*/
static inline int pb_tf_print_buf_compact(const struct pb_tf_msg *tf, const void *msg, char *buf,
					  size_t size)
{
	return pb_tf_print_buf_styled(tf, msg, buf, size, true, false);
}

static inline int pb_tf_print_buf_multiline(const struct pb_tf_msg *tf, const void *msg, char *buf,
					    size_t size)
{
	return pb_tf_print_buf_styled(tf, msg, buf, size, false, false);
}

static inline int pb_tf_print_buf_compact_braces(const struct pb_tf_msg *tf, const void *msg,
						 char *buf, size_t size)
{
	return pb_tf_print_buf_styled(tf, msg, buf, size, true, true);
}

static inline int pb_tf_print_buf_multiline_braces(const struct pb_tf_msg *tf, const void *msg,
						   char *buf, size_t size)
{
	return pb_tf_print_buf_styled(tf, msg, buf, size, false, true);
}
/**@}*/
#endif

/*
 * ---------------------------------------------------------------------------
 * Descriptor generation
 *
 * Everything below is preprocessor plumbing over nanopb's generated
 * <MSG>_FIELDLIST(X, structname) macro, whose rows are
 *
 *     X(structname, atype, htype, ltype, fieldname, tag)
 *
 * The three dispatch families mirror nanopb's own PB_GEN_SUBMSG_INFO /
 * PB_SI_PB_LTYPE_* pattern in pb.h, including its two-level indirection --
 * see PB_TF_SUBDESC for why that second level is mandatory rather than
 * stylistic.
 * ---------------------------------------------------------------------------
 */

/** Forced-expansion stringizer, needed only for the ONEOF tuple. */
#define PB_TF_STR(x)  PB_TF_STR_(x)
#define PB_TF_STR_(x) #x

/*
 * Field name. Non-oneof rows use a *direct* #fn on purpose: routing them
 * through PB_TF_STR would prescan the argument, so a field named e.g. `errno`
 * or `stdin` would stringize to its macro expansion instead of its own
 * spelling. Oneof rows have no choice, since PB_ONEOF_NAME must expand first.
 */
#define PB_TF_NAME_REQUIRED(fn) #fn
#define PB_TF_NAME_SINGULAR(fn) #fn
#define PB_TF_NAME_OPTIONAL(fn) #fn
#define PB_TF_NAME_REPEATED(fn) #fn
#define PB_TF_NAME_FIXARRAY(fn) #fn
#define PB_TF_NAME_ONEOF(fn)    PB_TF_STR(PB_ONEOF_NAME(MEMBER, fn))

/*
 * Submessage descriptor pointer.
 *
 * nanopb emits `#define <structname>_<fieldname>_MSGTYPE <SubStructName>` for
 * every message field, so the submessage's C type is recoverable from a row --
 * which is what lets this be a self-contained recursive tree with no runtime
 * registry.
 *
 * The second level is not decoration. In PB_TF_SUBDESC the parameter is glued
 * to ## , and C99 6.10.3.3p2 suppresses macro expansion of an argument that is
 * an operand of ## . Writing `&(t##_tf)` directly in
 * PB_TF_SUB_PB_LTYPE_MESSAGE would therefore paste the *unexpanded* token and
 * emit `&(Msg_inner_MSGTYPE_tf)`. Passing through a macro where `t` is not
 * adjacent to ## expands it to `Inner` first, and only then pastes.
 */
#define PB_TF_SUBDESC(t) &(t##_tf)

#define PB_TF_SUB_PB_LTYPE_MESSAGE(t)            PB_TF_SUBDESC(t)
#define PB_TF_SUB_PB_LTYPE_MSG_W_CB(t)           PB_TF_SUBDESC(t)
#define PB_TF_SUB_PB_LTYPE_BOOL(t)               NULL
#define PB_TF_SUB_PB_LTYPE_BYTES(t)              NULL
#define PB_TF_SUB_PB_LTYPE_DOUBLE(t)             NULL
#define PB_TF_SUB_PB_LTYPE_ENUM(t)               NULL
#define PB_TF_SUB_PB_LTYPE_UENUM(t)              NULL
#define PB_TF_SUB_PB_LTYPE_FIXED32(t)            NULL
#define PB_TF_SUB_PB_LTYPE_FIXED64(t)            NULL
#define PB_TF_SUB_PB_LTYPE_FLOAT(t)              NULL
#define PB_TF_SUB_PB_LTYPE_INT32(t)              NULL
#define PB_TF_SUB_PB_LTYPE_INT64(t)              NULL
#define PB_TF_SUB_PB_LTYPE_SFIXED32(t)           NULL
#define PB_TF_SUB_PB_LTYPE_SFIXED64(t)           NULL
#define PB_TF_SUB_PB_LTYPE_SINT32(t)             NULL
#define PB_TF_SUB_PB_LTYPE_SINT64(t)             NULL
#define PB_TF_SUB_PB_LTYPE_STRING(t)             NULL
#define PB_TF_SUB_PB_LTYPE_UINT32(t)             NULL
#define PB_TF_SUB_PB_LTYPE_UINT64(t)             NULL
#define PB_TF_SUB_PB_LTYPE_EXTENSION(t)          NULL
#define PB_TF_SUB_PB_LTYPE_FIXED_LENGTH_BYTES(t) NULL

#define PB_TF_SUBTF_REQUIRED(ltype, st, fn) PB_TF_SUB##ltype(st##_##fn##_MSGTYPE)
#define PB_TF_SUBTF_SINGULAR(ltype, st, fn) PB_TF_SUB##ltype(st##_##fn##_MSGTYPE)
#define PB_TF_SUBTF_OPTIONAL(ltype, st, fn) PB_TF_SUB##ltype(st##_##fn##_MSGTYPE)
#define PB_TF_SUBTF_REPEATED(ltype, st, fn) PB_TF_SUB##ltype(st##_##fn##_MSGTYPE)
#define PB_TF_SUBTF_FIXARRAY(ltype, st, fn) PB_TF_SUB##ltype(st##_##fn##_MSGTYPE)
#define PB_TF_SUBTF_ONEOF(ltype, st, fn)                                                           \
	PB_TF_SUBTF_ONEOF2(ltype, st, PB_ONEOF_NAME(UNION, fn), PB_ONEOF_NAME(MEMBER, fn))
#define PB_TF_SUBTF_ONEOF2(ltype, st, u, m) PB_TF_SUBTF_ONEOF3(ltype, st, u, m)
#define PB_TF_SUBTF_ONEOF3(ltype, st, u, m) PB_TF_SUB##ltype(st##_##u##_##m##_MSGTYPE)

/* Forward declaration for each submessage type, so row order never matters. */
#define PB_TF_EXTDECL(t) extern const struct pb_tf_msg t##_tf;

#define PB_TF_EXT_PB_LTYPE_MESSAGE(t)            PB_TF_EXTDECL(t)
#define PB_TF_EXT_PB_LTYPE_MSG_W_CB(t)           PB_TF_EXTDECL(t)
#define PB_TF_EXT_PB_LTYPE_BOOL(t)
#define PB_TF_EXT_PB_LTYPE_BYTES(t)
#define PB_TF_EXT_PB_LTYPE_DOUBLE(t)
#define PB_TF_EXT_PB_LTYPE_ENUM(t)
#define PB_TF_EXT_PB_LTYPE_UENUM(t)
#define PB_TF_EXT_PB_LTYPE_FIXED32(t)
#define PB_TF_EXT_PB_LTYPE_FIXED64(t)
#define PB_TF_EXT_PB_LTYPE_FLOAT(t)
#define PB_TF_EXT_PB_LTYPE_INT32(t)
#define PB_TF_EXT_PB_LTYPE_INT64(t)
#define PB_TF_EXT_PB_LTYPE_SFIXED32(t)
#define PB_TF_EXT_PB_LTYPE_SFIXED64(t)
#define PB_TF_EXT_PB_LTYPE_SINT32(t)
#define PB_TF_EXT_PB_LTYPE_SINT64(t)
#define PB_TF_EXT_PB_LTYPE_STRING(t)
#define PB_TF_EXT_PB_LTYPE_UINT32(t)
#define PB_TF_EXT_PB_LTYPE_UINT64(t)
#define PB_TF_EXT_PB_LTYPE_EXTENSION(t)
#define PB_TF_EXT_PB_LTYPE_FIXED_LENGTH_BYTES(t)

#define PB_TF_EXTTF_REQUIRED(ltype, st, fn) PB_TF_EXT##ltype(st##_##fn##_MSGTYPE)
#define PB_TF_EXTTF_SINGULAR(ltype, st, fn) PB_TF_EXT##ltype(st##_##fn##_MSGTYPE)
#define PB_TF_EXTTF_OPTIONAL(ltype, st, fn) PB_TF_EXT##ltype(st##_##fn##_MSGTYPE)
#define PB_TF_EXTTF_REPEATED(ltype, st, fn) PB_TF_EXT##ltype(st##_##fn##_MSGTYPE)
#define PB_TF_EXTTF_FIXARRAY(ltype, st, fn) PB_TF_EXT##ltype(st##_##fn##_MSGTYPE)
#define PB_TF_EXTTF_ONEOF(ltype, st, fn)                                                           \
	PB_TF_EXTTF_ONEOF2(ltype, st, PB_ONEOF_NAME(UNION, fn), PB_ONEOF_NAME(MEMBER, fn))
#define PB_TF_EXTTF_ONEOF2(ltype, st, u, m) PB_TF_EXTTF_ONEOF3(ltype, st, u, m)
#define PB_TF_EXTTF_ONEOF3(ltype, st, u, m) PB_TF_EXT##ltype(st##_##u##_##m##_MSGTYPE)

/** One row of the field table. */
#define PB_TF_GEN_ROW(st, atype, htype, ltype, fn, tag)                                            \
	{                                                                                          \
		PB_TF_NAME_##htype(fn), (uint16_t)(tag), (uint8_t)PB_TF_VT_##ltype,                 \
		(uint8_t)(PB_TF_AT_##atype | PB_TF_HT_##htype),                                     \
		PB_TF_SUBTF_##htype(_PB_LTYPE_##ltype, st, fn)                                     \
	},

/** Forward declarations for every submessage type referenced by a row. */
#define PB_TF_GEN_EXTERN(st, atype, htype, ltype, fn, tag)                                         \
	PB_TF_EXTTF_##htype(_PB_LTYPE_##ltype, st, fn)

/** Contributes @c +1 per field, for BUILD_ASSERTs. */
#define PB_TF_COUNT_ONE(st, atype, htype, ltype, fn, tag) +1

/**
 * @brief Number of fields in a message, from its FIELDLIST macro.
 *
 * @code BUILD_ASSERT(PB_TF_FIELD_COUNT(UI_CONFIG_FIELDLIST) == 2); @endcode
 */
#define PB_TF_FIELD_COUNT(FIELDLIST) (0 FIELDLIST(PB_TF_COUNT_ONE, _))

/**
 * @brief Define the text-format descriptor for one message.
 *
 * Mirrors @c PB_BIND(msgname, structname, width): @p msgname is the prefix of
 * the generated @c _FIELDLIST macro and @p structname is the C struct, which
 * differ under nanopb's @c --c-style (@c UI_CONFIG vs @c ui_config_t). The
 * descriptor is named <tt><structname>_tf</tt>.
 *
 * File scope only -- at block scope the definition would have no linkage and
 * the generated @c extern declarations could not resolve.
 *
 * @code PB_TF_DEFINE(UI_CONFIG, ui_config_t); @endcode
 */
#define PB_TF_DEFINE(msgname, structname)                                                          \
	msgname##_FIELDLIST(PB_TF_GEN_EXTERN, structname)                                          \
	static const struct pb_tf_field structname##__tf_fields[] = {                                   \
		msgname##_FIELDLIST(PB_TF_GEN_ROW, structname){NULL, 0, 0, 0, NULL}};               \
	const struct pb_tf_msg structname##_tf = {                                                      \
		&structname##_msg, structname##__tf_fields,                                        \
		(uint16_t)(ARRAY_SIZE(structname##__tf_fields) - 1U),                              \
		(uint16_t)sizeof(structname)};                                                     \
	extern const struct pb_tf_msg structname##_tf

/**
 * @brief Declare a descriptor defined in another translation unit.
 *
 * @code PB_TF_DECLARE(ui_config_t); @endcode
 */
#define PB_TF_DECLARE(structname) extern const struct pb_tf_msg structname##_tf

#ifdef __cplusplus
}
#endif

#endif /* NANOPB_TEXTFORMAT_H_ */
