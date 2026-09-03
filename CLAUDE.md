# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A Zephyr module that parses a subset of the protobuf text format directly into a nanopb struct and
prints one back out. No heap, no scratch buffers, **no recursion**. `README.md` is the user-facing
overview; `docs/dialect.md` is the precise subset contract; `docs/original_plan.md` is the design
record, including the decisions that were revised mid-implementation and the measurements that forced
them.

## Environment

`west` lives in a pyenv virtualenv named `zephyr`; `.python-version` selects it automatically, so no
manual activation is needed. The Zephyr SDK is at `~/.local/zephyr-sdk-1.0.1`.

`ws/` is a **gitignored T2 west workspace inside the repo**, pinned to Zephyr v4.4.0 (which pins
nanopb `5499fd4c`). Only `ws/manifest/west.yml` is tracked. Bootstrap or reset it with:

```sh
rm -rf ws/zephyr ws/modules ws/.west   # if resetting
west init -l ws/manifest
cd ws && west update --narrow --fetch-opt=--depth=1 && west packages pip --install
```

Every command below assumes `cd ws` and `export ZEPHYR_BASE="$PWD/zephyr"`.

## Commands

A `justfile` wraps all of these; `just --list` is the index. The raw commands are below because they
are what you will want to vary.

```sh
# Fast local loop: build + run all suites on qemu_x86 (~0.4 s of test time)
west build -p always -b qemu_x86 -d build-t ../tests/textformat
west build -d build-t -t run

# The real gate: 9 scenarios, 18 configurations, 2 boards. Before every commit.
west twister -T ../tests -p mps2/an385 -p qemu_x86 --inline-logs

# And native_sim, which needs a Linux host -- `just test-native` runs it in
# Docker. Do not skip it: it is the only target that compiles against a *host*
# libc, and v0.1.0 shipped a bug that no cross target could catch. See below.

# One configuration (core | nofloat | no64 | reduced | shallow)
west twister -T ../tests -p qemu_x86 -s nanopb_textformat.no64

# Locate the scenario containing one ZTEST. Note this runs every sibling test in
# that scenario, so it selects rather than isolates. The whole suite is fast
# enough that isolation rarely matters. Names drop the `test_` prefix:
west twister -T ../tests -p qemu_x86 \
  --sub-test nanopb_textformat.core.pb_tf_roundtrip.every_byte_value
```

### Measuring, not guessing

The design rests on measured numbers, and several plan decisions were reversed by them. Re-measure
whenever you touch the frame stacks, the float guards, or add a code path.

```sh
# Stack usage. Neither direction may recurse; both are one frame plus a fixed
# array that grows with MAX_DEPTH. See the measured figures under Architecture.
west build -p always -b mps2/an385 -d build-su ../tests/textformat \
  -- -DEXTRA_CFLAGS=-fstack-usage
cat $(find build-su -name 'nanopb_textformat_*.c.su')

# Code size / RAM. data and bss must both stay 0 -- the library has no statics.
find build-su -name 'nanopb_textformat_*.c.obj' -exec \
  ~/.local/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-size {} +

# Recursion check. Must be done on the LINKED ELF -- unrelocated `bl`
# placeholders in a .o disassemble as apparent self-calls and will mislead you.
# The SDK binutils are not on PATH.
~/.local/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-objdump \
  -d build-su/zephyr/zephyr.elf | \
  awk '/^[0-9a-f]+ <pb_tf_parse>:/{f=1} f&&/^[0-9a-f]+ <[a-z_]/&&!/<pb_tf_parse>:/{f=0} f&&/\tbl\t/{print $NF}'
```

## Architecture

### The descriptor tree is the whole idea

nanopb stores **no field names** anywhere — not in `pb_msgdesc_t`, not in the generated `.pb.c`. So
`PB_TF_DEFINE(msgname, structname)` in `include/nanopb_textformat.h` builds a companion descriptor by
expanding nanopb's own generated `<MSG>_FIELDLIST` X-macro. Because a row can recover its
submessage's C type from nanopb's `<struct>_<field>_MSGTYPE` define, the result is a **recursive tree
resolved entirely at compile time** — no runtime registry, no codegen changes.

Two macro hazards are load-bearing and easy to reintroduce:

- **The `##` trap.** A token adjacent to `##` is not argument-expanded (C99 6.10.3.3p2), so every
  `_MSGTYPE`-consuming macro needs a second indirection level (`PB_TF_SUBDESC`). Writing
  `&(t##_tf)` directly emits `&(Msg_field_MSGTYPE_tf)`.
- Field names use a **direct `#fn`**, never a two-level stringizer, which would prescan and turn a
  field named `errno` into its macro expansion. Only the `ONEOF` path needs two levels.

### Why rows carry `vtype` and `rules`

nanopb's runtime `pb_type_t` is **lossy**, and this is the single most important thing to understand
before changing either direction:

- `FLOAT`/`FIXED32`/`SFIXED32` all collapse to `PB_LTYPE_FIXED32`; `INT32`/`INT64`/`ENUM` to
  `PB_LTYPE_VARINT`. A runtime-only design cannot tell whether to print `1.0` or `1`.
- `PB_HTYPE_OPTIONAL == PB_HTYPE_SINGULAR` and `PB_HTYPE_REPEATED == PB_HTYPE_FIXARRAY`.

So the row carries the **pre-map ltype token** (`vtype`) and the **htype token** (`rules`) captured
from the macro. Never read `PB_HTYPE(iter.type)` or infer a type from the iterator. Round-trip tests
are what catch violations of this.

### Both directions are iterative state machines

`src/nanopb_textformat_parse.c` (`FIELD` → `VALUE` → `AFTER`) and
`src/nanopb_textformat_print.c` each walk submessages over a **fixed frame array inside one stack
frame**. This is a deliberate reversal of the original recursive design: recursion measured 144 bytes
per nesting level, wanting ~4.6 KB at the Kconfig maximum depth against a `CONFIG_SHELL_STACK_SIZE`
that defaults to 2048 — and the intended consumer is a shell command. It also keeps the library clear
of MISRA C:2025 Rule 17.2.

**Do not reintroduce recursion in either file.** Both now grow linearly with `MAX_DEPTH` instead:
20 bytes per level for parse and 16 for print, over a fixed frame. Measured at depth 1/8/32 that is
160/304/792 for `pb_tf_parse_into` and 144/256/648 for `pb_tf_print_styled` — note print is *not*
flat, which an earlier version of this file claimed. `pb_tf_parse`/`pb_tf_merge` add a 16-byte
forwarding frame, and `pb_tf_print_buf_styled` 32.

The one subtlety recursion handled for free: a repeated submessage's element count must not include
an element whose body failed halfway through. So the **commit** (`has_` bit, repeated count, or oneof
`which_`) is deferred and carried on the *child* frame, applied when its closing brace is consumed.

### Values are written straight into the destination

Every numeric write dispatches on the field's own `iter.data_size`, mirroring nanopb's chain in
`pb_decode.c` including its assign-then-compare range check. That is what makes `int_size`-narrowed
fields and implementation-defined enum widths correct rather than silently truncated. Strings and
bytes unescape directly into the field, bounded by nanopb's own capacity arithmetic — never the
declared `max_size`, which tail padding can differ from.

## Invariants that fail silently if broken

- **Output escapes must be three-digit octal**, never `\xHH`. `\x0` followed by `a` reads back as the
  single byte `0x0A`, so a hex escape is ambiguous whenever the next byte is a hex digit. This also
  keeps output pure ASCII, so UTF-8 payloads round-trip with no encoding logic.
- **Float paths are guarded with `#if`, not `IS_ENABLED`.** `IS_ENABLED` keeps the code compiled and
  the symbols referenced, which would defeat the documented claim that `FLOAT=n` links no `strtod`
  and saves 8–12 KB of flash. Verify with `nm`, not by reading.
- **`strtod` is used but not trusted.** It accepts hex floats, a leading `+`, and `nan(chars)`. The
  literal's shape is validated independently and `strtod`'s `endptr` must agree. Keep that check.
- **Only the `inf`/`nan` words may produce a non-finite value.** Both `1e39` into a 4-byte field and
  `1e309` into an 8-byte one report `PB_TF_ERR_RANGE`.
- **A `double` round-trips to 16 significant digits, not 17.** Zephyr's `cbprintf` caps floating point
  at 16 extracted digits, so `%.17g` cannot emit a 17th. Do not "fix" this by widening the format;
  do not assume `DBL_MAX` survives a round-trip. `docs/dialect.md` states the real promise.
- `PB_WITHOUT_64BIT` does **not** narrow struct storage — the generator never sees it. It narrows the
  codec, so a genuine 8-byte field reports `PB_TF_ERR_UNSUPPORTED`. Real narrowing is `int_size`.
- Shapes the library cannot represent (`POINTER`, `CALLBACK`, `FIXARRAY`, `EXTENSION`) are **runtime**
  errors on parse and **skipped** on print — never build failures. That distinction is the point.
- **The printer needs `CBPRINTF_COMPLETE`, and `CBPRINTF_FULL_INTEGRAL` only for fields wider than
  32 bits.** `CBPRINTF_COMPLETE` is non-negotiable because `CBPRINTF_NANO` has no `%o`, which the
  three-digit octal escapes need; it is a Kconfig *choice* member and so cannot be `select`ed, hence
  `NANOPB_TEXTFORMAT_PRINT` `depends on` it. `FULL_INTEGRAL` is narrower than it looks: anything 32
  bits or less prints through `PRId32`/`PRIu32`, and only a genuine 64-bit field consults
  `IS_ENABLED(CONFIG_CBPRINTF_FULL_INTEGRAL)` and reports `PB_TF_ERR_UNSUPPORTED` when it is off.
  Earlier versions of this file claimed both were needed for every integer, that `pb_tf_print_int()`
  casts everything to `long long`, and that `print.c` `BUILD_ASSERT`s on them. All three were true
  before 0.3.0 and none are now — there is no `BUILD_ASSERT` in `print.c` at all.

## The sample

`samples/textformat/` is a **shell application**, not a hardcoded demo: it registers a `cfg` command
that parses whole text-format messages into a nanopb struct. `just run` boots it under QEMU.

It doubles as the only executable check on `docs/shell-integration.md`. Registering
`SHELL_OPT_ARG_RAW` correctly is subtle enough that the guide was wrong until the sample was actually
run: **`mandatory` counts the command word plus any tokenized arguments, and the raw string lands at
`argv[mandatory]`**, so `mandatory = 1` puts it at `argv[1]`. The raw argument is also optional, so
`argc` can be 1 — guard before reading `argv[1]`. Zephyr's own
`tests/subsys/shell/shell/src/main.c` asserts both shapes.

To drive it non-interactively, pipe lines into `west build -t run` with a short pause between them.
An earlier version of this file blamed garbled unpaced input on the console mux; that was wrong. The
cause is `CONFIG_SHELL_BACKEND_SERIAL_RX_RING_BUFFER_SIZE`, which defaults to **64 bytes** and is
sized for a keyboard — anything pasted or piped past that is dropped, silently, before the handler
sees it. The sample raises it in its own prj.conf, and pacing then only matters for reading the
transcript.

## Either half can be left out

`CONFIG_NANOPB_TEXTFORMAT_PARSE` and `_PRINT` each drop half the library, for a firmware that only
accepts a configuration or only reports one. `common.c` (descriptor lookup, `pb_tf_clear`,
`pb_tf_strerror`, `pb_tf_out_buf`) is always built.

The split is clean because neither half calls the other — keep it that way. `CMakeLists.txt` selects
the sources with `zephyr_library_sources_ifdef`, and the public header guards the four declarations,
so calling into a configured-out half is a compile error naming the function rather than a link
failure. Measured on mps2/an385: 5116 B for both (common 474, parse 3317, print 1325), 1799
print-only, 3791 parse-only — so `common + half` matches each exactly.

One consequence worth preserving: **the `CBPRINTF_COMPLETE` requirement belongs to
`_PRINT`, not to the module.** Only `print.c` touches cbprintf, and the requirement exists because
`CBPRINTF_NANO` has no `%o` conversion, so the three-digit octal escape comes out as `"\12"`. A
parse-only build therefore works under `CBPRINTF_NANO`, and `tests/split` has a scenario proving it.

## Tests

`tests/split/` covers the four combinations of those two options, including parse-only under
`CBPRINTF_NANO`. Its suites are `#if`-guarded on the options, which is what lets one source file
build in every combination. Keep it small — field shapes belong in `tests/textformat/`.

`tests/textformat/` is one ztest app, eight suites, run across five `testcase.yaml` configurations.
The four non-default configs exist so degradation paths are *exercised* rather than merely
configured — keep them meaningful:

| Config | Proves |
|---|---|
| `core` | everything |
| `nofloat` | `FLOAT=n` degrades cleanly in both directions |
| `no64` | genuine 8-byte fields degrade cleanly (they do **not** narrow) |
| `shallow` | `MAX_DEPTH=1` actually reaches `PB_TF_ERR_DEPTH` |
| `reduced` | `CBPRINTF_REDUCED_INTEGRAL` — narrow fields still print, 64-bit ones degrade |

There were once `compact`, `braces` and `skipdefaults` scenarios, one per printed style, because a
build could only print one way. 0.6.0 put the style in the function name, so one build now asserts
all four exactly (`test_every_style_exactly`) and those three scenarios were removed. Fewer
configurations, more coverage — do not restore them.

Note `fx_scalars` carries a field of every ltype, so printing it needs a build that can represent
all of them. Tests that print it guard on `SCALARS_PRINTABLE`; several used to pass under `nofloat`
and `no64` only because they asked for `SKIP_DEFAULTS`, which quietly omitted the zero float and
64-bit fields. Print-all is the default now, so those fields are reached.

### native_sim is not optional

Zephyr compiles module sources at **`-std=c17`**, which sets `__STRICT_ANSI__`. On a cross target the
SDK's picolibc declares its POSIX extras anyway, and Apple libc does too — so a POSIX-only function
compiles cleanly on every platform this project's matrix used to cover, and is an **error** on glibc.
`native_sim` on Linux is the only target that compiles against a host libc.

v0.1.0 shipped `strnlen()` for exactly this reason and broke every consumer building for
`native_sim`. Prefer a small local helper over a POSIX function, or over `_POSIX_C_SOURCE` juggling.

Two further things that target taught, worth keeping in mind:

- **twister turns warnings into errors**; a plain `west build` only warns. A clean `west build` is not
  evidence.
- Plain `native_sim` is **32-bit** and needs `gcc-multilib`, which does not exist on aarch64 — an
  arm64 host can only run `native_sim/native/64`. Both are in `platform_allow`, and `just
  test-native` **defaults to the 64-bit qualifier** for this reason. It used to default to plain
  `native_sim`, so on this machine every scenario died in `arch/posix/aarch64.cmake` before
  compiling a line — the recipe had never once worked here without an explicit argument. Treat a
  green *local* native_sim claim with suspicion unless you have seen the pass counts: this is the
  same shape of failure as the `strnlen` bug, a check that looked like it was running and was not.
- Running x86 under QEMU on Apple Silicon makes `cc1` segfault intermittently. That is emulation
  noise, not a defect; retry, or use the arm64 path for a stable signal.

Three fixtures, each load-bearing. Do not consolidate them:

- `fixture.proto` — proto3, every field shape, generated with `--c-style`
- `fixture2.proto` — proto2 only: `required`, an `extensions` range, `MSG_W_CB`, non-`STATIC`
  allocation
- `fixture3.proto` — **the same shapes with nanopb's default naming**, which is the only thing
  proving `PB_TF_DEFINE` is naming-agnostic (its two arguments differ under `--c-style`, coincide
  here). `NANOPB_OPTIONS` is read per `zephyr_nanopb_sources()` call, which is what allows two styles
  in one build.

**Round-trip is the strongest suite** — parse → print → parse, comparing both structs and both
printed texts. It exercises the `vtype` table in both directions at once, so a printer that lost a
type distinction fails there and nowhere else. Extend it when adding a field shape.

### Test-writing gotchas hit repeatedly in this codebase

- **Never** write `zassert_equal(pb_tf_parse(...), PB_TF_OK, "%s", pb_tf_strerror(st.code))`. The
  parse call and the status read are sibling arguments of one full expression with no sequence point,
  so the status may be read before the parse writes it. Store the result first:
  `err = pb_tf_parse(...); zassert_equal(err, PB_TF_OK, "%s", pb_tf_strerror(st.code));`
  GCC catches this as `'st.code' is used uninitialized` — treat that warning as a real defect, not
  something to silence with `= {0}`.
- A macro parameter is substituted **even after a `.`**, so a parameter named `buf` rewrites
  `.buf = ...` in a designated initializer. See `PRINT_INTO`'s `dst` parameter.
- The generated headers are at `<build>/src/fixture*.pb.h`, since `zephyr_nanopb_sources()` adds only
  `${CMAKE_CURRENT_BINARY_DIR}` while protoc mirrors the proto's source-relative path. Include them
  as `"src/fixture.pb.h"`.
- The test app is a **white-box** suite: it includes `src/nanopb_textformat_priv.h` for internal
  helpers, via a `target_include_directories` entry pointing at the library's `src/`.

## Two corrections worth not repeating

**`CONFIG_SHELL_WILDCARD=n` is not needed for a raw leaf at `mandatory = 1`.** This guide, the README
and a public PR comment all said it was, on plausible-sounding reasoning about wildcard expansion
rewriting `cmd_buff`. The reasoning was wrong: in `execute()` the tokenising loop is gated on
`args_left > 0` and calls `z_shell_wildcard_process()` from inside it, while a RAW leaf gets
`args_left = mandatory - 1` — zero at `mandatory = 1`. Nothing after the command name is ever offered
to wildcard processing. Verified with `WILDCARD=y`: `a*b`, `q?z`, `x*y?z` round-trip byte-identical.
It *does* still apply at `mandatory >= 2`. A downstream integration caught this, with source
references and a reproduction.

**The library shipped a Kconfig snippet it had no business shipping.** `nanopb-textformat-shell`
bundled five settings; by the time the printer used width-correct conversions and the module gained
`depends on CBPRINTF_COMPLETE`, two were redundant and the other three were application shell tuning
with magic numbers sized for one consumer's message. Its own comment had also gone stale and
contradicted the Kconfig help. Removed in 0.4.0. The general lesson: this library has no shell
dependency, so shell configuration belongs in `docs/shell-integration.md` with reasons, not in
something the module ships as if it were a requirement.

## Versioning

This project follows **semantic versioning** (https://semver.org).

`include/nanopb_textformat.h` holds the single source of truth
(`NANOPB_TEXTFORMAT_VERSION_MAJOR` / `_MINOR` / `_PATCH` / `_STRING`, plus a `0xMMmmpp`
`NANOPB_TEXTFORMAT_VERSION` for `#if` comparisons). A release bumps those macros **and** carries a
matching annotated git tag `vX.Y.Z`; a test asserts the string and the numbers agree, so they cannot
drift.

While major is 0, the API may still change, and a breaking change is a **minor** bump rather than a
major one — that is what semver says 0.x means. Classify every change anyway, so the eventual 1.0 is
a real promise rather than a guess, and say plainly in the release notes when something breaks.

What counts as breaking here is not obvious, because three of this library's contracts are not
function signatures:

**MAJOR**

- Removing, renaming or re-signaturing any of the seven public functions, the eight `static
  inline` wrappers, or the two macros.
- Changing the signature or behaviour of `pb_tf_print_styled()` / `pb_tf_print_buf_styled()`. The eight
  style wrappers are `static inline` in the header, so **user** object files call these directly and
  carry the argument order with them — exactly the ABI hazard the struct layouts have below.
- Changing the layout of `struct pb_tf_field` or `struct pb_tf_msg`. `PB_TF_DEFINE` bakes those structs into
  *user* object files, so a layout change is an ABI break: everything must be recompiled, and a stale
  object file will read garbage rather than fail to link.
- **Narrowing the dialect** — input that used to parse now errors. `docs/dialect.md` is a contract,
  and every narrowing is visible to users who had working messages.
- Changing printed output so that text this library previously produced no longer reparses, or no
  longer parses under `protoc`. Round-trip and pasteability are both promises.
- Renumbering `enum pb_tf_err`. Append new codes at the end, never insert.
- Changing the meaning of an existing error code, or making a Kconfig default break existing builds
  (`FLOAT` from `y` to `n` would).

**MINOR**

- **Widening the dialect** — accepting more valid text format than before. This is the expected
  growth direction: maps, enum-by-name and `\U` escapes are all deliberately absent, and all three
  would be minor bumps.
- A new public function, print flag, Kconfig option, or error code appended to the enum.
- Changing a documented cost (a Kconfig default that moves the stack figure, say).

**PATCH**

- Fixing behaviour to match what `docs/dialect.md` already says.
- `pb_tf_strerror()` wording. The strings are documented as human-readable, not as API — but keep them
  **composable**, i.e. naming only the fault with no trailing "for field", or callers that append
  context get "value out of range for field in field 'x'".
- Docs, tests, comments.

Two release obligations that are easy to forget:

- If a new error code is appended, extend the bound in `test_strerror_covers_every_code`, which loops
  `PB_TF_OK` to `PB_TF_ERR_INTERNAL` — an appended code past `INTERNAL` is otherwise untested.
- The README quotes **exact measured byte counts** for flash, stack and the whole-image reference. Any
  release that touches code must re-run `just size` / `just stack` and update them. Stale numbers in a
  cost table are worse than no numbers.

## Conventions

All public symbols are `pb_tf_*` / `PB_TF_*`; the Kconfig namespace is `NANOPB_TEXTFORMAT_*`; the
module is `nanopb_textformat` and the header `nanopb_textformat.h`.

Zephyr conventions, and neither is negotiable here:

- **No `typedef`s.** Types are spelled `struct pb_tf_msg`, `struct pb_tf_status`,
  `enum pb_tf_err`. The library shipped `_t` aliases up to 0.6.0; they are gone.
- **Functions return `int`: 0 on success, a negated `enum pb_tf_err` on failure.** The enumerators
  stay positive, so a caller compares against `-PB_TF_ERR_RANGE`. `pb_tf_strerror()` normalises the
  sign so a return value or a `status.code` can be passed straight in. `status.code` stores the
  negated form for the same reason.

Watch for two shapes the mechanical conversion missed once already: a `return X ? PB_TF_ERR_A : ...`
ternary is not `return PB_TF_ERR_A;`, and a test comparing against a positive enumerator still
compiles while always failing.

Internal helpers belong in `src/nanopb_textformat_priv.h`, not the public header — the public API is
deliberately small: `parse`, `merge`, `print_styled`, `print_buf_styled`, `clear`, `strerror`,
`out_buf`, plus the eight `static inline` style wrappers and the `PB_TF_DEFINE` / `PB_TF_DECLARE`
macros.

**No runtime flags, and no style-agnostic print.** The line style and the braces are in the function
name -- `pb_tf_print_buf_compact()`, `pb_tf_print_multiline_braces()` and so on, eight in all -- so a
call site says what it produces and one firmware can use several shapes. Replace-versus-merge is
likewise `pb_tf_parse()` versus `pb_tf_merge()`.

Do not add a `pb_tf_print()` that reads the style from Kconfig: that was the arrangement up to 0.5.0,
and its problem is that reading a call site tells you nothing about the output. Only
`_PRINT_SKIP_DEFAULTS` stays Kconfig, because making it per-call would mean a third boolean on every
entry point and no firmware wants it both ways.

The eight wrappers are `static inline` in the public header over `pb_tf_print_styled()` /
`pb_tf_print_buf_styled()`, which are public for a second reason: a caller choosing style at **run
time** would otherwise need a four-way branch to select a wrapper. `samples/textformat` is that
caller and is the worked example.

Do not restore them as real functions. As `static inline` they emit nothing of their own, which is
measured: `print.c` went 1469 -> 1325 bytes when they were inlined, since each was a 16-byte function
that could not be a tail call -- `pb_tf_print_styled()` takes six arguments and AAPCS passes four in
registers, so the wrapper had to `push`/`strd` the style pair and therefore keep a frame.

Speed was **not** the reason and should not be claimed as one: inlining removes one `bl`/`pop` pair,
4-6 cycles, against the ~1000 bytes of formatting a print does. The reasons are the 144 bytes and the
run-time style choice.

Because the wrappers are inline in the header, nothing links against them, so a wrapper wired to the
wrong boolean pair still compiles and still prints valid text format -- just the wrong shape.
`test_wrappers_match_styled` and `test_callback_wrappers_match_styled` compare each wrapper's bytes
against the explicit call; keep them when adding a style.

`docs/dialect.md` is a contract, not a description. If you change what is accepted or printed, update
it in the same commit, and keep the guarantee it opens with true: everything accepted is valid text
format that protobuf's own parser accepts, and everything printed, protobuf's parser accepts.
