# zephyr-nanopb-textformat

Protobuf **text format** for [nanopb][nanopb] messages, as a [Zephyr][zephyr] module.

Parses a subset of the text format directly into a nanopb-generated struct, and prints such a struct
back out. No heap, no scratch buffers, no recursion.

```
uart:~$ cfg set delay: 100, threshold: 500
uart:~$ cfg set tags: [1,2,3], path: [{x: 7, y: 0}, {x: 9, y: 1}]
uart:~$ cfg get
delay: 100
threshold: 500
```

[nanopb]: https://jpa.kapsi.fi/nanopb/
[zephyr]: https://zephyrproject.org/

## Why

nanopb descriptors carry **no field names** — not `pb_msgdesc_t`, not the generated `.pb.c`. So the
usual way to accept a field value from a shell command is a generated macro per field, which works
until you need `optional`, `repeated`, or a submessage. A macro cannot recurse into a submessage's
field list: the preprocessor will not re-enter a macro inside its own expansion.

This library builds a companion descriptor from nanopb's own generated `<MSG>_FIELDLIST` X-macro, so
the field table becomes a **recursive tree** resolved entirely at compile time, with no runtime
registry and no codegen changes. Walking that tree handles every field shape uniformly.

Rather than invent a path syntax, it uses the standard that already exists for this: a message
written as text.

## Quick start

Add the module to your manifest, then one line per message at file scope:

```c
#include <nanopb_textformat.h>
#include "ui.pb.h"

PB_TF_DEFINE(UI_CONFIG, ui_config_t);   /* mirrors PB_BIND's two-token form */
```

```c
struct ui_config cfg;
pb_tf_status_t st;

if (pb_tf_parse(&ui_config_t_tf, &cfg, "delay: 100, threshold: 500", 0U, &st) != PB_TF_OK) {
	printk("at offset %u: %s\n", st.offset, pb_tf_strerror(st.code));
}

char text[128];

(void)pb_tf_print_buf(&ui_config_t_tf, &cfg, text, sizeof(text), PB_TF_MULTILINE);
```

```
CONFIG_NANOPB=y
CONFIG_NANOPB_TEXTFORMAT=y
CONFIG_CBPRINTF_FULL_INTEGRAL=y   # only if you have 64-bit fields
```

For driving this from a shell command, see **[docs/shell-integration.md](docs/shell-integration.md)**:
it needs `SHELL_OPT_ARG_RAW` on a leaf command, and `CONFIG_SHELL_WILDCARD=n`, because wildcard
expansion rewrites the command buffer before the handler sees it.

## Try it

`samples/textformat/` is a shell application: it registers a `cfg` command that edits a nanopb struct
by typing whole text-format messages, which is the use case this library exists for.

```sh
just run          # builds and boots it under QEMU (qemu_x86 by default)
just run mps2/an385
```

Then, at the `uart:~$` prompt:

```
cfg examples                                  worked examples of every field shape
cfg help                                      the command reference

cfg set delay: 100, threshold: 500            replace the configuration
cfg set path: [{x: 1, y: 2}, {x: 3, y: 4}]    a repeated submessage
cfg set origin { x: -3, y: 4 }                the colon is optional before braces
cfg set name: "hello world"                   quote the value, not the argument

cfg merge tags: [3]                           keep what is there; repeated fields append
cfg get                                       one field per line
cfg get compact                               a single line, valid standalone text format
cfg get set                                   the same, as a pasteable `cfg set` line
cfg get brief                                 omit implicit-presence zeros
cfg clear
```

Errors carry an offset and the field at fault, so a typo says where it is:

```
uart:~$ cfg set delay: -1
at offset 7: value out of range in field 'delay'
uart:~$ cfg set path: [{x:1},{x:2},{x:3},{x:4}]
at offset 25: too many values in field 'path'
```

`just` lists everything else — `just test` is the full gate, `just check` the fast inner loop.

## API

```c
pb_tf_err_t pb_tf_parse(const pb_tf_msg_t *tf, void *msg, const char *text,
                        unsigned int flags, pb_tf_status_t *status);
pb_tf_err_t pb_tf_print(const pb_tf_msg_t *tf, const void *msg,
                        cbprintf_cb out, void *ctx, unsigned int flags);
pb_tf_err_t pb_tf_print_buf(const pb_tf_msg_t *tf, const void *msg,
                            char *buf, size_t size, unsigned int flags);
void        pb_tf_clear(const pb_tf_msg_t *tf, void *msg);
const char *pb_tf_strerror(pb_tf_err_t code);
int         pb_tf_out_buf(int c, void *ctx);   /* a bundled cbprintf_cb */
```

`pb_tf_parse()` zeroes the message first unless `PB_TF_NOINIT` is set. Print flags are
`PB_TF_MULTILINE` (default) or `PB_TF_COMPACT`, optionally OR'd with `PB_TF_BRACES` and
`PB_TF_SKIP_DEFAULTS`. Print-all is the default: implicit-presence scalars print even when zero.

Prefer `pb_tf_print_buf()` over `pb_tf_print()` + `pb_tf_out_buf()`. It terminates the buffer even
when a message prints nothing, and reports truncation as `PB_TF_ERR_NO_SPACE` rather than a flag you
have to remember to check.

## What it accepts

Everything this library accepts is valid text format that protobuf's own parser accepts; everything it
prints, protobuf's own parser accepts. The reverse does not hold — it is a subset, and every narrowing
turns an unsupported construct into an error rather than a silent misparse.

| | |
|---|---|
| Scalars | `delay: 100` — decimal, `0x` hex, leading-zero octal, all bool spellings, numeric enums |
| Optional | presence set on write, including for a zero value |
| Repeated | `tags: [1,2,3]` or `tags: 1 tags: 2` — both append |
| Submessages | `inner { a: 7 }` — the colon is optional, the braces are not |
| Oneof | one member per union; switching clears the union first |
| Strings, bytes | `"…"` / `'…'`, with `\xH[H]`, `\NNN` and `\uXXXX` escapes |
| Floats | `1.0`, `-2.5e3`, `1f`, `inf`, `nan` |
| Outer braces | optional on input — the one documented extension |

**[docs/dialect.md](docs/dialect.md)** is the precise contract: what is in, what is out, and why.
The short version of what is out: maps, groups, extension fields, `Any`, `#` comments,
angle-bracket delimiters, **enum values by name**, and non-`STATIC` field allocation.

Field names must be `lower_snake_case` in the `.proto` — nanopb's `--c-style` mangles anything else,
and stringification is the only source of names available.

## Cost

Measured on `mps2/an385` (Cortex-M3) at `-Os`.

| | |
|---|---|
| Code | 5.2 KB text total — parse 3.3 KB, print 1.4 KB, common 0.5 KB |
| `pb_tf_parse()` stack | `132 + 20n` bytes, *n* = `MAX_DEPTH` → **296 B** at the default 8 |
| `pb_tf_print()` stack | **264 B** |
| RAM | none — no statics, no heap |
| `FLOAT=n` saves | ~12 KB text, and links no `strtod` |

**Neither direction recurses.** Submessages are walked with an explicit frame array, so stack cost is
a compile-time constant visible in one `-fstack-usage` line rather than a function of the input. That
matters because the intended consumer is a shell command, and `CONFIG_SHELL_STACK_SIZE` defaults to
2048 bytes: a recursive implementation measured 144 bytes *per nesting level*, wanting ~1.2 KB at the
default depth and ~4.6 KB at the Kconfig maximum — enough to overflow the stack it runs on, via the
one knob a user would raise without suspecting it spends stack. It also keeps the library clear of
MISRA C:2025 Rule 17.2, which forbids recursion direct or indirect.

## Configuration

| Symbol | Default | |
|---|---|---|
| `NANOPB_TEXTFORMAT` | `n` | Enable the module |
| `NANOPB_TEXTFORMAT_FLOAT` | `y` | Float and double support. `depends on !MINIMAL_LIBC` (needs `strtod`) and selects `CBPRINTF_FP_SUPPORT`. `n` costs ~12 KB less text and makes float fields report `PB_TF_ERR_NO_FLOAT` |
| `NANOPB_TEXTFORMAT_MAX_DEPTH` | `8` | Submessage nesting limit, range 1–32. Costs 20 bytes of stack per level |

Messages with 64-bit fields also need `CONFIG_CBPRINTF_FULL_INTEGRAL=y`, since
`CBPRINTF_INTEGRAL_CONV` can otherwise drop 64-bit conversion.

## Tests

`tests/textformat/` is one ztest application, run under twister on two QEMU targets across four
configurations — the defaults, `FLOAT=n`, `NANOPB_WITHOUT_64BIT=y`, and `MAX_DEPTH=1`. The last three
exist so that the degradation paths are exercised rather than merely configured.

```sh
west twister -T tests -p mps2/an385 -p qemu_x86 --inline-logs
```

Eight suites: the macro machinery and generated table shape; scalar parsing; the full error table;
submessages; strings and bytes; printing; floats; and **round-trip** — parse → print → parse,
comparing both the structs and the two printed texts, over 13 inputs × 8 flag combinations plus all
255 non-NUL byte values.

Round-trip is the strongest check here, because it exercises the descriptor's type code in both
directions at once. nanopb's runtime `pb_type_t` cannot tell a `float` from a `fixed32`, so a printer
that had lost that distinction would emit a value the parser could not read back.

Three fixtures, each earning its place:

- `fixture.proto` — proto3, every field shape, generated with `--c-style`;
- `fixture2.proto` — proto2, for `required`, an `extensions` range, `MSG_W_CB` and non-`STATIC`
  allocation, none of which proto3 can express;
- `fixture3.proto` — the same shapes again with nanopb's **default** naming, which is what proves
  `PB_TF_DEFINE` is naming-agnostic: under `--c-style` its two arguments differ (`FX_SHAPES` vs
  `fx_shapes_t`) and here they coincide (`FxDef`, `FxDef`).

Pinned to Zephyr **v4.4.0** and the nanopb revision it carries (`5499fd4c`), which is what every
claim above was verified against.

## Development

`ws/` is a gitignored west workspace inside the repo, so it can be reset wholesale.

```sh
west init -l ws/manifest
cd ws && west update --narrow --fetch-opt=--depth=1
west packages pip --install
```

`--narrow` and `--depth=1` keep the fetch small; the trade-off is no usable history in the fetched
projects. The module is injected into the test build via `EXTRA_ZEPHYR_MODULES` pointing at the repo
root.

The library is C99-compatible source compiled at C11 — `pb.h` defaults `PB_STATIC_ASSERT` to
`_Static_assert`, so any translation unit including it needs C11. Same as nanopb itself.

[`docs/original_plan.md`](docs/original_plan.md) is the design document this was built from. It is
kept as a record rather than as current documentation — read `README.md` and
[`docs/dialect.md`](docs/dialect.md) for how the library behaves today. Its value is the reasoning:
each decision that was revised during implementation is marked in place, with the measurement that
forced it.

## License

Apache-2.0.
