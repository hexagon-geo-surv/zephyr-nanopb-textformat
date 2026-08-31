/*
 * Copyright (c) 2026 Rodrigo Peixoto
 * SPDX-License-Identifier: Apache-2.0
 */

/* Internal helpers shared by the parser and the printer. Not a public API. */

#ifndef NANOPB_TEXTFORMAT_PRIV_H_
#define NANOPB_TEXTFORMAT_PRIV_H_

#include <nanopb_textformat.h>

/** Submessage nesting limit, from Kconfig. */
#define PB_TF_MAX_DEPTH CONFIG_NANOPB_TEXTFORMAT_MAX_DEPTH

/*
 * Widest integer this build can hold, mirroring pb_decode.c:52-58.
 *
 * nanopb defines pb_int64_t/pb_uint64_t privately inside pb_decode.c rather
 * than in pb.h, so they cannot be reused from here -- hence our own aliases
 * under the same rule. Note what PB_WITHOUT_64BIT does and does not do: it
 * does NOT narrow a generated struct member (the generator never sees the
 * define, so an int64 field stays int64_t and 8 bytes wide). It narrows only
 * the codec's accumulator, which is why nanopb then rejects a genuine 8-byte
 * field at runtime with "invalid data_size" (pb_decode.c:452-453). Real width
 * narrowing comes from nanopb's int_size option instead.
 */
#ifdef PB_WITHOUT_64BIT
#define pb_tf_uint64_t   uint32_t
#define pb_tf_int64_t    int32_t
#define PB_TF_UINT64_MAX UINT32_MAX
#define PB_TF_INT64_MAX  INT32_MAX
#define PB_TF_INT64_MIN  INT32_MIN
#else
#define pb_tf_uint64_t   uint64_t
#define pb_tf_int64_t    int64_t
#define PB_TF_UINT64_MAX UINT64_MAX
#define PB_TF_INT64_MAX  INT64_MAX
#define PB_TF_INT64_MIN  INT64_MIN
#endif

/** True for the two submessage vtypes, which take an optional colon. */
bool pb_tf_vtype_is_msg(uint8_t vtype);

/**
 * Look a field up by name.
 *
 * @p name need not be NUL-terminated; @p len gives its length, which lets the
 * lexer point straight into the input instead of copying the identifier out.
 *
 * EXTENSION rows are skipped: nanopb synthesises one named "extensions" for a
 * message declaring an extension range, and it must never be addressable by
 * that name. (pb_field_iter_find() refuses to return them either.)
 *
 * @return the row, or NULL if no field matches.
 */
const pb_tf_field_t *pb_tf_field_find(const pb_tf_msg_t *tf, const char *name, size_t len);

/**
 * True if this field's shape can be read or written at all.
 *
 * Rejects POINTER and CALLBACK allocation (this library only handles STATIC),
 * FIXARRAY (nanopb's fixed_count: there is no count member to grow, and
 * iter.pSize aliases the iterator itself), and EXTENSION fields.
 */
bool pb_tf_shape_supported(const pb_tf_field_t *f);

#endif /* NANOPB_TEXTFORMAT_PRIV_H_ */
