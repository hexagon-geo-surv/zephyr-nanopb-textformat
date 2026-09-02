/*
 * Copyright (c) 2026 Rodrigo Peixoto
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief nanopb_textformat sample: edit a nanopb struct from the Zephyr shell.
 *
 * Stands in for the real use case -- a shell command that configures a running
 * component and prints its configuration back. The whole message is typed as
 * protobuf text format, so optional, repeated and submessage fields need no
 * per-field command.
 *
 * Try `cfg examples` first. See docs/shell-integration.md for why `set` and
 * `merge` are registered with SHELL_OPT_ARG_RAW and what that implies.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <nanopb_textformat.h>

/* zephyr_nanopb_sources() puts the generated header under the proto's own
 * source-relative path in the build tree. */
#include "src/config.pb.h"

/*
 * One line per message, at file scope, next to where the message is used.
 *
 * The two arguments mirror PB_BIND(msgname, structname, width): the first is the
 * prefix of the generated _FIELDLIST macro, the second is the C struct. They
 * coincide under nanopb's default naming, as here, and differ under --c-style
 * (CONFIG vs config_t). Nothing else changes.
 */
PB_TF_DEFINE(Point, Point);
PB_TF_DEFINE(Config, Config);

/** The "device configuration" this sample edits. */
static Config cfg;

/** Report a parse failure. strerror() names only the fault, so context composes. */
static void report(const struct shell *sh, pb_tf_err_t err, const pb_tf_status_t *st)
{
	if (st->field != NULL) {
		shell_error(sh, "at offset %u: %s in field '%s'", st->offset,
			    pb_tf_strerror(err), st->field);
	} else {
		shell_error(sh, "at offset %u: %s", st->offset, pb_tf_strerror(err));
	}
}

/** Shared by set and merge, which differ only in whether cfg is cleared. */
static int assign(const struct shell *sh, const char *text, bool replace)
{
	pb_tf_status_t st;
	pb_tf_err_t err;

	/* Two named functions rather than a flag: pb_tf_parse() clears first,
	 * pb_tf_merge() keeps what is there and appends to repeated fields. */
	err = replace ? pb_tf_parse(&Config_tf, &cfg, text, &st)
		      : pb_tf_merge(&Config_tf, &cfg, text, &st);

	if (err != PB_TF_OK) {
		report(sh, err, &st);
		return -EINVAL;
	}

	return 0;
}

/*
 * With mandatory = 1 the raw argument is optional, so argc may be 1 and argv[1]
 * must not be touched before checking.
 */
static int cmd_set(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: cfg set <message>   (try: cfg examples)");
		return -EINVAL;
	}

	/* argv[1] is the untouched remainder of the command line. */
	return assign(sh, argv[1], true);
}

static int cmd_merge(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: cfg merge <message>   (try: cfg examples)");
		return -EINVAL;
	}

	return assign(sh, argv[1], false);
}

static int cmd_get(const struct shell *sh, size_t argc, char **argv)
{
	char text[256];
	pb_tf_err_t err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	/*
	 * There is nothing to select here: the output style is a build option
	 * (CONFIG_NANOPB_TEXTFORMAT_PRINT_*), so a firmware states it once rather
	 * than at every call site. This sample is built multi-line; set
	 * CONFIG_NANOPB_TEXTFORMAT_PRINT_COMPACT=y to get a single line.
	 *
	 * pb_tf_print_buf() rather than pb_tf_print() + pb_tf_out_buf(): it
	 * terminates the buffer even when nothing is printed -- an all-default
	 * message under SKIP_DEFAULTS prints nothing at all -- and reports
	 * truncation instead of setting a flag the caller must remember to check.
	 */
	err = pb_tf_print_buf(&Config_tf, &cfg, text, sizeof(text));
	if (err != PB_TF_OK) {
		shell_error(sh, "print: %s", pb_tf_strerror(err));
		return -EIO;
	}

	shell_print(sh, "%s", text);

	return 0;
}

static int cmd_clear(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	pb_tf_clear(&Config_tf, &cfg);
	shell_print(sh, "cleared");

	return 0;
}

static int cmd_examples(const struct shell *sh, size_t argc, char **argv)
{
	static const char *const lines[] = {
		"A scalar. Outer braces are optional on input:",
		"  cfg set delay: 100",
		"  cfg set {delay: 100}",
		"",
		"An optional scalar. Presence is recorded even for a zero, so this",
		"still shows under CONFIG_NANOPB_TEXTFORMAT_PRINT_SKIP_DEFAULTS",
		"while delay: 0 would not:",
		"  cfg set threshold: 0",
		"  cfg get",
		"",
		"A repeated scalar, as a list or by repetition. Both append:",
		"  cfg set tags: [1, 2, 3]",
		"  cfg set tags: 1 tags: 2",
		"",
		"A submessage. The colon is optional, the braces are not:",
		"  cfg set origin { x: -3, y: 4 }",
		"  cfg set origin: {x: -3, y: 4}",
		"",
		"A repeated submessage -- the shape a macro-per-field design cannot",
		"reach, because the preprocessor will not walk Point's field list from",
		"inside Config's own expansion:",
		"  cfg set path: [{x: 1, y: 2}, {x: 3, y: 4}]",
		"  cfg set path { x: 1 } path { y: 2 }",
		"",
		"A string. Quote the value, but never the whole argument:",
		"  cfg set name: \"hello world\"",
		"",
		"Everything at once:",
		"  cfg set delay: 10, threshold: 20, tags: [7, 8], origin {x: 1, y: 2},"
		" path: [{x: 9, y: 9}], name: \"all\"",
		"",
		"set replaces; merge keeps what is there and appends repeated fields:",
		"  cfg set tags: [1, 2]",
		"  cfg merge tags: [3]",
		"  cfg get",
		"",
		"Build with CONFIG_NANOPB_TEXTFORMAT_PRINT_COMPACT=y and 'cfg get'",
		"prints one line you can paste straight back in.",
		"",
		"Errors carry an offset and the field at fault:",
		"  cfg set nosuch: 1",
		"  cfg set delay: -1",
		"  cfg set origin: 5",
		"  cfg set name: \"far too long to fit in sixteen\"",
		"  cfg set path: [{x:1},{x:2},{x:3},{x:4}]",
	};

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	ARRAY_FOR_EACH(lines, i) {
		shell_print(sh, "%s", lines[i]);
	}

	return 0;
}

/*
 * `set` and `merge` are leaf commands registered with SHELL_OPT_ARG_RAW, so each
 * handler receives the remainder of the line exactly as typed: no quote
 * stripping, no backslash processing, no whitespace collapsing, and no
 * CONFIG_SHELL_ARGC_MAX cap. Joining argv instead would silently corrupt any
 * message past roughly eight fields, or any string value containing a space.
 *
 * mandatory = 1, not 2. The count covers the command word plus any *tokenized*
 * mandatory arguments, and the raw string then lands at argv[mandatory]. With 1
 * the raw remainder is argv[1]; with 2 the shell would tokenize one more word
 * into argv[1] and the raw string would start at argv[2] -- which is what
 * Zephyr's own tests/subsys/shell/shell tests assert.
 *
 * A consequence worth noting: the raw argument is then *optional*, so argc can
 * be 1 and both handlers check before reading argv[1].
 *
 * A raw argument only works on a leaf, which is why neither command has
 * subcommands of its own.
 */
SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_cfg,
	SHELL_CMD_ARG(set, NULL,
		      "Replace the configuration from a text-format message.\n"
		      "Usage: cfg set <message>\n"
		      "Taken raw, so do not wrap the message in quotes.\n"
		      "Example: cfg set delay: 100, path: [{x: 1, y: 2}]",
		      cmd_set, 1, SHELL_OPT_ARG_RAW),
	SHELL_CMD_ARG(merge, NULL,
		      "Merge into the configuration without clearing it first.\n"
		      "Usage: cfg merge <message>\n"
		      "Scalars are overwritten; repeated fields are appended to.\n"
		      "Example: cfg merge tags: [9]",
		      cmd_merge, 1, SHELL_OPT_ARG_RAW),
	SHELL_CMD_ARG(get, NULL,
		      "Print the configuration as text format.\n"
		      "Usage: cfg get\n"
		      "The style is a build option, not an argument:\n"
		      "  CONFIG_NANOPB_TEXTFORMAT_PRINT_MULTILINE (default)\n"
		      "  CONFIG_NANOPB_TEXTFORMAT_PRINT_COMPACT\n"
		      "  CONFIG_NANOPB_TEXTFORMAT_PRINT_BRACES\n"
		      "  CONFIG_NANOPB_TEXTFORMAT_PRINT_SKIP_DEFAULTS",
		      cmd_get, 1, 0),
	SHELL_CMD_ARG(clear, NULL,
		      "Zero the configuration.\n"
		      "Usage: cfg clear",
		      cmd_clear, 1, 0),
	SHELL_CMD_ARG(examples, NULL,
		      "Print worked examples of every field shape.\n"
		      "Usage: cfg examples",
		      cmd_examples, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(cfg, &sub_cfg, "Edit a nanopb struct as protobuf text format", NULL);

int main(void)
{
	printk("\nnanopb_textformat " NANOPB_TEXTFORMAT_VERSION_STRING
	       " sample -- edit a nanopb struct from the shell.\n\n"
	       "  cfg examples                              worked examples\n"
	       "  cfg help                                  command reference\n"
	       "  cfg set delay: 100, path: [{x: 1, y: 2}]  parse a message\n"
	       "  cfg get                                   print it back\n\n");

	return 0;
}
