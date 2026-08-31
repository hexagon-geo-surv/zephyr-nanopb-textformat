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
