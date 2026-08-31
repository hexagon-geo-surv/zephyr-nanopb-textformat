/*
 * Copyright (c) 2026 Rodrigo Peixoto
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nanopb_textformat_priv.h"

/* TODO(milestone 5): implement. */

pb_tf_err_t pb_tf_print(const pb_tf_msg_t *tf, const void *msg, cbprintf_cb out, void *ctx,
			unsigned int flags)
{
	ARG_UNUSED(tf);
	ARG_UNUSED(msg);
	ARG_UNUSED(out);
	ARG_UNUSED(ctx);
	ARG_UNUSED(flags);

	return PB_TF_ERR_INTERNAL;
}
