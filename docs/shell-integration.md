# Using this from a Zephyr shell command

The motivating use case: a shell command that configures a running component by name, and prints its
configuration back.

```
uart:~$ cfg set delay: 100, threshold: 500
uart:~$ cfg set tags: [1,2,3], path: [{x: 7, y: 0}, {x: 9, y: 1}]
uart:~$ cfg get
delay: 100
threshold: 500
```

Everything below was verified against **Zephyr v4.4.0**.

## Register the leaf command as `SHELL_OPT_ARG_RAW`

This is not optional, and it is the part most likely to be got wrong.

The obvious approach — take the message as ordinary arguments and join `argv` back together — is
broken in two ways that are silent:

1. **`CONFIG_SHELL_ARGC_MAX` defaults to 20**, and the shell **drops everything past the cap**
   (`subsys/shell/shell.c:275-280`) rather than reporting it. A text-format message reaches 20 tokens
   at roughly eight fields, after which fields simply disappear.
2. **`z_shell_make_argv()` collapses whitespace runs and strips quotes and backslashes**
   (`subsys/shell/shell_utils.c:112-134` — note the `memmove` calls). Rejoining with single spaces
   cannot reconstruct the original, so any string value containing a space, a quote or a backslash is
   corrupted.

`SHELL_OPT_ARG_RAW` (`include/zephyr/shell/shell.h:61`) avoids both. On a **leaf** command — one with
no subcommands — `active_cmd_prepare()` stops tokenising after the mandatory arguments
(`shell.c:588-591`), and the handler is passed the **untouched remainder** of `cmd_buff`
(`shell.c:804-806`): no quote stripping, no backslash processing, no whitespace collapsing, no ARGC
cap.

```c
static int cmd_config(const struct shell *sh, size_t argc, char **argv)
{
	struct ui_config cfg;
	pb_tf_status_t st;
	pb_tf_err_t err;

	/*
	 * argv[1] is the raw remainder of the command line. It is optional, so
	 * argc can be 1 -- check before touching it.
	 */
	if (argc < 2) {
		shell_error(sh, "usage: config <text-format message>");
		return -EINVAL;
	}

	err = pb_tf_parse(&ui_config_t_tf, &cfg, argv[1], 0U, &st);
	if (err != PB_TF_OK) {
		/* strerror() names only the fault, so context composes cleanly. */
		if (st.field != NULL) {
			shell_error(sh, "at offset %u: %s in field '%s'", st.offset,
				    pb_tf_strerror(err), st.field);
		} else {
			shell_error(sh, "at offset %u: %s", st.offset, pb_tf_strerror(err));
		}
		return -EINVAL;
	}

	return ui_apply(&cfg);
}

/* mandatory = 1: just the command word. See the table below. */
SHELL_STATIC_SUBCMD_SET_CREATE(sub_ui,
	SHELL_CMD_ARG(config, NULL, "Set configuration", cmd_config, 1, SHELL_OPT_ARG_RAW),
	SHELL_SUBCMD_SET_END
);
```

### Getting `mandatory` right

`mandatory` counts the command word plus any **tokenized** mandatory arguments. The raw string then
lands at `argv[mandatory]`:

| `mandatory` | `argv[0]` | `argv[1]` | `argv[2]` |
|---|---|---|---|
| **1** | command | **raw remainder** | — |
| 2 | command | one tokenized argument | **raw remainder** |

So use `1` when the whole remainder is the message, and `2` when the command takes one ordinary
argument first — `config <instance> <message>`, say. Zephyr's own
`tests/subsys/shell/shell/src/main.c` asserts both shapes; that is the authority if you need to check.

Note also that the raw argument is **optional**: with `mandatory = 1`, `argc` can be 1 when the user
types the command with nothing after it. Check `argc` before reading `argv[1]`.

## Three caveats that come with raw arguments

These follow directly from "untouched" and are easy to be surprised by. The third is a build setting
you should just apply.

### 1. Do not wrap the argument in quotes

Quotes are no longer stripped, so they become part of the message text:

```
cfg set '{delay: 100}'     # WRONG: the parser sees the ' characters
cfg set {delay: 100}       # right
```

A leading `'` is not a valid start to a text-format message, so this fails with
`PB_TF_ERR_SYNTAX` at offset 0 rather than doing something subtle. Quotes *inside* string values are
fine and necessary — `{name: "a b"}`.

### 2. The line length cap is `CONFIG_SHELL_CMD_BUFF_SIZE`, default 256

That is the whole command line, not just the message. A long message needs the buffer raised:

```
CONFIG_SHELL_CMD_BUFF_SIZE=512
```

Use compact output (`PB_TF_COMPACT`, no `PB_TF_BRACES`) when generating text meant to be pasted back
in, since it is the shortest form that still re-parses.

### 3. Turn `CONFIG_SHELL_WILDCARD` off

```
CONFIG_SHELL_WILDCARD=n
```

Not conditionally — do this as a matter of course when a command takes a raw text-format argument.

`CONFIG_SHELL_WILDCARD` is `default y if !SHELL_MINIMAL`, so it is on unless you turn it off, and
wildcard expansion **rewrites `cmd_buff`** before the handler ever sees it. A `*` or `?` anywhere in
the raw argument — including inside a quoted string value, which the shell has no reason to treat as
quoted — gets expanded against the command table. The handler then receives text the user did not
type, and the failure is silent and confusing rather than an error.

There is no workaround on the command side: by the time the handler runs, the damage is done. It has
to be a build decision, so make it once and be done with it. The only reason to leave it on is if
something else in your firmware genuinely depends on shell wildcards, in which case keep `*` and `?`
out of your message values entirely.

## Printing configuration back

Use `pb_tf_print_buf()` rather than `pb_tf_print()` + `pb_tf_out_buf()` unless you have a reason not
to. It terminates the buffer even when the message prints nothing — an empty message, or every field
skipped under `PB_TF_SKIP_DEFAULTS` — and turns truncation into `PB_TF_ERR_NO_SPACE` instead of a
flag you have to remember to check.

```c
static int cmd_get_config(const struct shell *sh, size_t argc, char **argv)
{
	struct ui_config cfg;
	char text[256];
	pb_tf_err_t err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	ui_snapshot(&cfg);

	err = pb_tf_print_buf(&ui_config_t_tf, &cfg, text, sizeof(text), PB_TF_MULTILINE);
	if (err != PB_TF_OK) {
		shell_error(sh, "print: %s", pb_tf_strerror(err));
		return -EIO;
	}

	shell_print(sh, "%s", text);
	return 0;
}
```

To stream straight to the shell with no intermediate buffer at all, pass your own `cbprintf_cb`:

```c
static int out_shell(int c, void *ctx)
{
	shell_fprintf(ctx, SHELL_NORMAL, "%c", (char)c);
	return c;
}

(void)pb_tf_print(&ui_config_t_tf, &cfg, out_shell, (void *)sh, PB_TF_MULTILINE);
```

## Generating the descriptors

One `PB_TF_DEFINE` per message, at **file scope**, next to where the message is used:

```c
#include <nanopb_textformat.h>
#include "ui.pb.h"

PB_TF_DEFINE(UI_CONFIG, ui_config_t);   /* --c-style naming */
```

The two arguments mirror `PB_BIND(msgname, structname, width)`: the first is the prefix of the
generated `_FIELDLIST` macro, the second is the C struct. They differ under `--c-style` (`UI_CONFIG`
vs `ui_config_t`) and coincide without it (`UiConfig`, `UiConfig`).

If a code generator emits your message glue, emit one `PB_TF_DEFINE` line per request/response
message alongside the existing `PB_BIND`.

File scope matters: at block scope the definition would have no linkage, and the `extern`
declarations the macro generates for submessage types could not resolve.

To use a descriptor defined in another translation unit, declare it with `PB_TF_DECLARE(ui_config_t)`.

## Kconfig checklist

```
CONFIG_NANOPB=y
CONFIG_NANOPB_TEXTFORMAT=y

# Wildcard expansion rewrites cmd_buff before the handler sees it, which
# corrupts a raw text-format argument. Turn it off; see caveat 3.
CONFIG_SHELL_WILDCARD=n

# 64-bit fields need full integral conversion in cbprintf.
CONFIG_CBPRINTF_FULL_INTEGRAL=y

# Only if a command line may exceed 256 bytes
# CONFIG_SHELL_CMD_BUFF_SIZE=512
```

`CONFIG_NANOPB_TEXTFORMAT_FLOAT` is `default y` and selects `CBPRINTF_FP_SUPPORT`. Setting it to `n`
makes float fields report `PB_TF_ERR_NO_FLOAT` in both directions rather than failing the build, and
saves 8–12 KB of flash — see the README's Cost section.

## What to expect on the stack

Both directions use a fixed frame array rather than recursion, so cost is a compile-time constant
rather than a function of the input. Measured on Cortex-M3 at `-Os`:

| | Stack |
|---|---|
| `pb_tf_parse()` | `132 + 20n` bytes, where *n* is `CONFIG_NANOPB_TEXTFORMAT_MAX_DEPTH` — 296 B at the default 8 |
| `pb_tf_print()` | 264 B |

The shell thread gets **2048 bytes** by default (`CONFIG_SHELL_STACK_SIZE`), so the default depth
leaves comfortable room. Raising `MAX_DEPTH` is linear and visible in a single `-fstack-usage` line;
at the Kconfig maximum of 32 the parser costs 776 B.
