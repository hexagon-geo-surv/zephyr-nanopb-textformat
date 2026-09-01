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
rip ws/zephyr ws/modules ws/.west      # if resetting
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

# The real gate: 4 configurations x 2 boards. Run this before every commit.
west twister -T ../tests -p mps2/an385 -p qemu_x86 --inline-logs

# One configuration (core | nofloat | no64 | shallow)
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
# Stack usage. pb_tf_parse and pb_tf_print must each stay a single flat frame.
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

**Do not reintroduce recursion in either file.** Cost is now `132 + 20n` bytes for parse and a flat
264 for print.

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
  and saves ~12 KB. Verify with `nm`, not by reading.
- **`strtod` is used but not trusted.** It accepts hex floats, a leading `+`, and `nan(chars)`. The
  literal's shape is validated independently and `strtod`'s `endptr` must agree. Keep that check.
- **Only the `inf`/`nan` words may produce a non-finite value.** Both `1e39` into a 4-byte field and
  `1e309` into an 8-byte one report `PB_TF_ERR_RANGE`.
- `PB_WITHOUT_64BIT` does **not** narrow struct storage — the generator never sees it. It narrows the
  codec, so a genuine 8-byte field reports `PB_TF_ERR_UNSUPPORTED`. Real narrowing is `int_size`.
- Shapes the library cannot represent (`POINTER`, `CALLBACK`, `FIXARRAY`, `EXTENSION`) are **runtime**
  errors on parse and **skipped** on print — never build failures. That distinction is the point.

## The sample

`samples/textformat/` is a **shell application**, not a hardcoded demo: it registers a `cfg` command
that parses whole text-format messages into a nanopb struct. `just run` boots it under QEMU.

It doubles as the only executable check on `docs/shell-integration.md`. Registering
`SHELL_OPT_ARG_RAW` correctly is subtle enough that the guide was wrong until the sample was actually
run: **`mandatory` counts the command word plus any tokenized arguments, and the raw string lands at
`argv[mandatory]`**, so `mandatory = 1` puts it at `argv[1]`. The raw argument is also optional, so
`argc` can be 1 — guard before reading `argv[1]`. Zephyr's own
`tests/subsys/shell/shell/src/main.c` asserts both shapes.

To drive it non-interactively, pipe paced lines into `west build -t run`; unpaced input gets garbled
by the console mux.

## Tests

`tests/textformat/` is one ztest app, eight suites, run across four `testcase.yaml` configurations.
The three non-default configs exist so degradation paths are *exercised* rather than merely
configured — keep them meaningful:

| Config | Proves |
|---|---|
| `core` | everything |
| `nofloat` | `FLOAT=n` degrades cleanly in both directions |
| `no64` | genuine 8-byte fields degrade cleanly (they do **not** narrow) |
| `shallow` | `MAX_DEPTH=1` actually reaches `PB_TF_ERR_DEPTH` |

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

## Conventions

All public symbols are `pb_tf_*` / `PB_TF_*`; the Kconfig namespace is `NANOPB_TEXTFORMAT_*`; the
module is `nanopb_textformat` and the header `nanopb_textformat.h`.

Internal helpers belong in `src/nanopb_textformat_priv.h`, not the public header — the public API is
deliberately six functions (`parse`, `print`, `print_buf`, `clear`, `strerror`, `out_buf`) plus the
`PB_TF_DEFINE` / `PB_TF_DECLARE` macros.

`docs/dialect.md` is a contract, not a description. If you change what is accepted or printed, update
it in the same commit, and keep the guarantee it opens with true: everything accepted is valid text
format that protobuf's own parser accepts, and everything printed, protobuf's parser accepts.
