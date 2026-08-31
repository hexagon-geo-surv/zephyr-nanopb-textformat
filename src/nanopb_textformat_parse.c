/*
 * Copyright (c) 2026 Rodrigo Peixoto
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nanopb_textformat_priv.h"

/* TODO(milestone 2-4): implement. */

pb_tf_err_t pb_tf_parse(const pb_tf_msg_t *tf, void *msg, const char *text, unsigned int flags,
			pb_tf_status_t *status)
{
	ARG_UNUSED(text);

	if (status != NULL) {
		status->code = PB_TF_ERR_INTERNAL;
		status->offset = 0U;
		status->field = NULL;
	}

	if ((flags & PB_TF_NOINIT) == 0U) {
		pb_tf_clear(tf, msg);
	}

	return PB_TF_ERR_INTERNAL;
}
