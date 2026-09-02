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

## Adding it to your project

Add this repository to your west manifest and `west update`:

```yaml
manifest:
  remotes:
    - name: rodrigopex
      url-base: https://codeberg.org/rodrigopex

  projects:
    - name: zephyr-nanopb-textformat
      remote: rodrigopex
      revision: v0.4.0
      path: modules/lib/zephyr-nanopb-textformat
```

Pin a tag rather than `main`, so a `west update` cannot move the library under you.

`zephyr/module.yml` makes it a Zephyr module, so nothing further is needed in your CMake. Then:

```
CONFIG_NANOPB=y
CONFIG_NANOPB_TEXTFORMAT=y
```

To try it without touching your manifest, `EXTRA_ZEPHYR_MODULES` pointing at a clone also works —
that is what `samples/textformat/CMakeLists.txt` does.

## Quick start

One line per message at file scope:

```c
#include <nanopb_textformat.h>
#include "ui.pb.h"

PB_TF_DEFINE(UI_CONFIG, ui_config_t);   /* mirrors PB_BIND's two-token form */
```

```c
struct ui_config cfg;
pb_tf_status_t st;

if (pb_tf_parse(&ui_config_t_tf, &cfg, "delay: 100, threshold: 500", &st) != PB_TF_OK) {
	printk("at offset %u: %s\n", st.offset, pb_tf_strerror(st.code));
}

char text[128];

(void)pb_tf_print_buf(&ui_config_t_tf, &cfg, text, sizeof(text));
```

```
CONFIG_NANOPB=y
CONFIG_NANOPB_TEXTFORMAT=y
```

That is all the library needs. Driving it from a shell command needs a couple of *application*
settings too — see [docs/shell-integration.md](docs/shell-integration.md).

For driving this from a shell command, see **[docs/shell-integration.md](docs/shell-integration.md)**:
it needs `SHELL_OPT_ARG_RAW` on a leaf command, and two shell buffers raised.

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
                        pb_tf_status_t *status);   /* clears msg first */
pb_tf_err_t pb_tf_merge(const pb_tf_msg_t *tf, void *msg, const char *text,
                        pb_tf_status_t *status);   /* keeps what is there */
pb_tf_err_t pb_tf_print(const pb_tf_msg_t *tf, const void *msg,
                        cbprintf_cb out, void *ctx);
pb_tf_err_t pb_tf_print_buf(const pb_tf_msg_t *tf, const void *msg,
                            char *buf, size_t size);
void        pb_tf_clear(const pb_tf_msg_t *tf, void *msg);
const char *pb_tf_strerror(pb_tf_err_t code);
int         pb_tf_out_buf(int c, void *ctx);   /* a bundled cbprintf_cb */
```

**There are no runtime flags.** Output style is a Kconfig choice, so a firmware states it once
rather than at every call site, and the unused branches are not compiled:

| | |
|---|---|
| `NANOPB_TEXTFORMAT_PRINT_MULTILINE` | one field per line, submessages indented (default) |
| `NANOPB_TEXTFORMAT_PRINT_COMPACT` | a single line — the shortest form that still re-parses |
| `NANOPB_TEXTFORMAT_PRINT_BRACES` | wrap the whole message in `{ }` |
| `NANOPB_TEXTFORMAT_PRINT_SKIP_DEFAULTS` | omit implicit-presence fields that are still zero |

Print-all is the default, so a printed message shows every field and round-trips byte-exact into a
cleared struct. Replace-versus-merge is likewise two named functions rather than a flag.

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
| Floats | `1.0`, `-2.5e3`, `1f`, `inf`, `nan` — doubles round-trip to 16 significant digits ([why](docs/dialect.md#floats)) |
| Outer braces | optional on input — the one documented extension |

**[docs/dialect.md](docs/dialect.md)** is the precise contract: what is in, what is out, and why.
The short version of what is out: maps, groups, extension fields, `Any`, `#` comments,
angle-bracket delimiters, **enum values by name**, and non-`STATIC` field allocation.

Field names must be `lower_snake_case` in the `.proto` — nanopb's `--c-style` mangles anything else,
and stringification is the only source of names available.

## Cost

All figures measured on `mps2/an385` (Cortex-M3) at `-Os`, reproducible with `just size` and
`just stack`.

### Flash

| | |
|---|---|
| Library code | **5132 B** — parse, print and common combined |
| with `FLOAT=n` | **4566 B** (−566 B of library code) |
| Per message descriptor | `12 × (fields + 1) + 12` bytes, plus the field names as strings |

The descriptor is `const`, so it lives in flash, not RAM. Two worked examples from the sample:

| Message | Fields | Rows | Descriptor | Names | Total |
|---|---|---|---|---|---|
| `Point` | 2 | 36 B | 12 B | 4 B | **52 B** |
| `Config` | 6 | 84 B | 12 B | 38 B | **134 B** |

So a dozen small messages cost on the order of 1 KB of flash in descriptors. Only messages you write
a `PB_TF_DEFINE` for are paid for.

### RAM

**The library has no static RAM at all** — `data` and `bss` are both zero, and there is no heap use
and no scratch buffer. `just size` prints those columns, so it is checkable rather than a claim.

Your RAM cost is therefore only:

- `sizeof(YourMessage)` — the nanopb struct you already had; and
- stack, below.

### Stack

Neither direction recurses. Submessages are walked with an explicit frame array, so cost is a
compile-time constant rather than a function of the input:

Both grow linearly with `CONFIG_NANOPB_TEXTFORMAT_MAX_DEPTH`, at 20 bytes per level for the parser
and 16 for the printer, on top of a fixed frame. Measured:

| `MAX_DEPTH` | `pb_tf_parse()` | `pb_tf_print()` |
|---|---|---|
| 1 | 160 B | 144 B |
| **8 (default)** | **304 B** | **264 B** |
| 32 (maximum) | 792 B | 648 B |

Add ~60 B for the deepest leaf helper. Budget **about 400 bytes** for a call into the library at the
default depth, and see [Tips](#tips) for what that means for your thread stacks — usually your own
output buffer matters more than the library does.

Being iterative is not a micro-optimisation. A recursive implementation measured **144 bytes per
nesting level**: ~1.2 KB at the default depth and ~4.6 KB at the Kconfig maximum, against a
`CONFIG_SHELL_STACK_SIZE` that defaults to 2048. The depth cap looked like a safety limit but bounded
the level *count*, so the one knob a user would plausibly raise was the one that would overflow the
stack. It also keeps the library clear of MISRA C:2025 Rule 17.2, which forbids recursion direct or
indirect.

### Whole-image reference

The sample — Zephyr, the shell, nanopb and this library — on `mps2/an385`:

| | Flash | RAM |
|---|---|---|
| `FLOAT=y` (default) | 56 772 B | 11 712 B |
| `FLOAT=n` | 48 584 B | 11 712 B |

Turning floats off saved 8.2 KB of flash here and **no RAM**, because what it removes is `strtod()`
and cbprintf's floating-point formatting. Expect somewhere between 8 and 12 KB depending on how much
of that your firmware already pulls in for other reasons — if something else already formats floats,
you save less.

## Tips

**Do I need to change any stack sizes?** Usually yes, and usually not because of this library.

Zephyr's defaults are `CONFIG_MAIN_STACK_SIZE=1024` and `CONFIG_SHELL_STACK_SIZE=2048`. The library
wants ~400 bytes of that, which is comfortable. What is not comfortable is a printing buffer next to
it: `samples/textformat/` originally held a `Config` plus a 192-byte output buffer and called
`printk`, and that overflowed the 1024-byte main stack outright.

A budget that works:

| | |
|---|---|
| Library call | ~400 B at the default depth |
| Your output buffer | `sizeof(buf)`, if it is a local |
| `printk` / `shell_print` formatting | several hundred bytes |
| Head-room | the rest |

So on the shell thread, `CONFIG_SHELL_STACK_SIZE=3072` with a 256-byte buffer is comfortable; the
sample uses exactly that. If you print into a `static` buffer instead, the default 2048 is fine. If
you raise `CONFIG_NANOPB_TEXTFORMAT_MAX_DEPTH`, add 20 bytes per level.

**Keep the message struct off the stack** if it is large. A `Config` is tens of bytes, but a message
with several `max_size:64` strings is not — make it `static` or hand in a pointer to one you already
own, exactly as you would with nanopb itself.

**Use `pb_tf_print_buf()`, not `pb_tf_print()` + `pb_tf_out_buf()`**, unless you specifically want to
stream. It terminates the buffer even when a message prints nothing, and reports truncation as
`PB_TF_ERR_NO_SPACE` instead of a flag you have to remember to check.

**Prefer compact output for anything you intend to feed back in.**
`CONFIG_NANOPB_TEXTFORMAT_PRINT_COMPACT=y` without `_PRINT_BRACES` is the shortest form that is
still valid standalone text format, which matters when `CONFIG_SHELL_CMD_BUFF_SIZE` caps the line at
256 bytes by default.

**Report errors with the offset and the field.** `pb_tf_strerror()` names only the fault, so it
composes:

```c
if (st.field != NULL) {
        shell_error(sh, "at offset %u: %s in field '%s'", st.offset,
                    pb_tf_strerror(err), st.field);
} else {
        shell_error(sh, "at offset %u: %s", st.offset, pb_tf_strerror(err));
}
```

**Driving it from a shell? Read the one caveat you would never guess.**
`CONFIG_SHELL_BACKEND_SERIAL_RX_RING_BUFFER_SIZE` defaults to **64 bytes**, sized for a keyboard, so
a *pasted* message is silently truncated mid-parse while the same line typed by hand works.
[docs/shell-integration.md](docs/shell-integration.md) has that and the rest — including one setting
that guide used to recommend and no longer does.

## Configuration

| Symbol | Default | |
|---|---|---|
| `NANOPB_TEXTFORMAT` | `n` | Enable the module |
| `NANOPB_TEXTFORMAT_FLOAT` | `y` | Float and double support. `depends on !MINIMAL_LIBC` (needs `strtod`) and selects `CBPRINTF_FP_SUPPORT`. `n` makes float fields report `PB_TF_ERR_NO_FLOAT` in both directions and saves 8–12 KB of flash, depending on what else in the image formats floats |
| `NANOPB_TEXTFORMAT_MAX_DEPTH` | `8` | Submessage nesting limit, range 1–32. Costs 20 bytes of stack per level |
| `NANOPB_TEXTFORMAT_PRINT_*` | multi-line | Output style; see [API](#api) |

### cbprintf

The printer casts every integer to `long long` and prints `%lld`/`%llu`, so it needs
`CONFIG_CBPRINTF_COMPLETE=y` and `CONFIG_CBPRINTF_FULL_INTEGRAL=y` for **all** integer fields, not
only 64-bit ones.

Both are already Zephyr's defaults, so normally there is nothing to do. They are also Kconfig
*choice* members, which means this module cannot `select` them — so instead it `BUILD_ASSERT`s on
them, and a project that deliberately opted out gets a build error naming the symbol rather than
silently wrong numbers.

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

## Versioning

[Semantic versioning](https://semver.org). The current version is **0.4.0**.

Major is still 0, which means the API may change — the library is complete and tested, but nobody
outside this repository has used it yet, and the error strings were reworded as recently as the first
release. Pin a tag.

`include/nanopb_textformat.h` exposes the version if you need to compile against more than one:

```c
#if NANOPB_TEXTFORMAT_VERSION >= 0x000200   /* 0.2.0 */
```

Three of this library's contracts are not function signatures, and they are versioned as carefully as
the API is:

- **The dialect.** Accepting *more* valid text format is a minor bump; accepting *less* is a major
  one, because it breaks messages that used to work. `docs/dialect.md` is the contract.
- **Printed output.** A change that stops previously-printed text from reparsing — here or under
  `protoc` — is a major bump.
- **Descriptor layout.** `PB_TF_DEFINE` bakes `pb_tf_field_t` into *your* object files, so changing it
  is an ABI break and a major bump.

Maps, enum values by name, and `\U` escapes are deliberately absent today; adding any of them would
be a minor bump, not a breaking one.

## License

Apache-2.0. See [LICENSE](LICENSE).
