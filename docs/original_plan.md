# `zephyr-nanopb-textformat` — minimal protobuf text-format parser/printer for nanopb on Zephyr

## Context

The usual way to accept a field value from a Zephyr shell command is a generated macro per field.
That handles exactly one field shape — `STATIC, SINGULAR, <scalar>` — and everything else pastes into
an undefined macro name and hard-fails the build. Optional, repeated and submessage fields need
something a macro fundamentally cannot do: recurse into a submessage's field list, which the
preprocessor refuses because it will not re-enter a macro inside its own expansion.

The obvious alternative is to invent a path grammar (`items[0].a=7`, `tags=1,2,3`). But a standard
for this already exists, so this repo implements a **subset of the protobuf text format** instead:

```
uart:~$ cfg set delay: 100, threshold: 500
uart:~$ cfg set tags: [1,2,3], path: [{x: 7, y: 0}, {x: 9, y: 1}]
uart:~$ cfg get
delay: 100
threshold: 500
```

Outcome: a **Zephyr module** that binds text format directly into a nanopb struct and prints one
back out. Everything it accepts is valid text format that protobuf's own parser accepts, with one
documented deviation (optional outer braces, off by default on output).

## Decisions

| | |
|---|---|
| Name | Repo/dir **`zephyr-nanopb-textformat`**; module.yml `name: nanopb_textformat`; header `nanopb_textformat.h`; Kconfig `NANOPB_TEXTFORMAT` (verified free — zephyr's nanopb module owns only `NANOPB_{ENABLE_MALLOC, MAX_REQUIRED_FIELDS, NO_ERRMSG, BUFFER_ONLY, WITHOUT_64BIT, ENCODE_ARRAYS_UNPACKED, VALIDATE_UTF8}`); symbols stay `pb_tf_*` / `PB_TF_*`. |
| Binding | **Direct into the struct** via nanopb's `pb_field_iter_t`. No intermediate wire buffer. |
| Field names | **X-macro in this library** over nanopb's generated `<MSG>_FIELDLIST`. No codegen changes. |
| Scope | **Parse + print.** Reading a configuration back needs print; round-trip is the strongest test. |
| Platform | **Zephyr-native.** Use Zephyr's utilities rather than reimplementing them. |
| Shell input | **`SHELL_OPT_ARG_RAW`** — the handler gets the untouched remainder of `cmd_buff`. |
| Printer | Engine is **`cbprintf`**; flags select multi-line/compact, braces, skip-defaults. Print-all default. |
| Init | **`pb_tf_parse` clears the struct**, `PB_TF_NOINIT` opts out (mirrors `PB_DECODE_NOINIT`). |
| Floats | `CONFIG_NANOPB_TEXTFORMAT_FLOAT`, `default y`, `depends on !MINIMAL_LIBC`, `select CBPRINTF_FP_SUPPORT`. Guarded with `#if`, not `IS_ENABLED`, so `=n` genuinely references neither `strtod()` nor FP formatting — verified: no `strtod` symbol in the image, and 8 KB less flash in the sample / 12 KB in the test app on mps2/an385, and no RAM either way. |
| Enums | **Numeric only**, both directions. Rationale below. |
| Tests | **ztest + twister** on `mps2/an385` and `qemu_x86`, in a gitignored in-repo workspace. |
| Zephyr | Pinned to **`v4.4.0`** (which pins nanopb `5499fd4c`) — the exact trees every fact below was verified against. |

### Taken from Zephyr instead of written

- **`cbprintf(cbprintf_cb out, void *ctx, fmt, ...)`** (`cbprintf.h:686`) is the printer's output
  engine — its contract is literally "unbounded length without incurring the cost of a temporary
  buffer", and `cbprintf_cb` (`cbprintf.h:294`) becomes the library's writer type. Deletes a
  hand-rolled integer formatter and gives float formatting for free.
- **`SHELL_OPT_ARG_RAW`** (`shell.h:61`) for command registration — see fact 9.
- **`char2hex` / `hex2char` / `hex2bin` / `bin2hex`** (`sys/util.h:760-794`) for `\xHH` escapes and
  bytes printing.
- **`strtoull` / `strtoll`** — present in every Zephyr libc including minimal
  (`lib/libc/minimal/source/stdlib/strtoull.c`), called with an explicit base after prefix sniffing.
- **`IS_ENABLED()`**, **`BUILD_ASSERT`**, `ARRAY_SIZE`, `MIN`, `CLAMP`, and `util_macro.h`'s
  `UTIL_CAT` for the X-macro glue.
- **`LOG_MODULE_REGISTER` + `Kconfig.template.log_config`**, the usual way a module's `Kconfig` ends.
- **ztest + twister**, in the conventional in-tree test layout.

## Verified facts this design rests on

All of these were read against **Zephyr `v4.4.0`** and the nanopb revision that tag pins
(`5499fd4c9a478f8139eeb07a82c3b4468d6067f7`, nanopb-1.0.0-dev, `PB_PROTO_HEADER_VERSION 40`).
Every cited file — `subsys/shell/shell.c`, `shell_utils.c`, `include/zephyr/sys/cbprintf.h`,
`sys/util.h`, `shell/shell.h`, `boards/arm/mps2/mps2_an385.yaml`, `modules/nanopb/{CMakeLists.txt,
Kconfig}` — is byte-identical at that tag, so the line numbers below are valid as written and need
no re-verification during milestone 0.

1. `pb.h:724-757` — nanopb dispatches submessage info as
   `PB_SUBMSG_INFO_##htype(_PB_LTYPE_##ltype, structname, fieldname)` and relies on
   `structname##_##fieldname##_MSGTYPE` (emitted at `nanopb_generator.py:1517-1531`, with a `ONEOF`
   variant `<msg>_<union>_<member>_MSGTYPE`). **The submessage's C type is recoverable from an
   X-macro row**, so the name table is a recursive tree with no runtime registry.
   `pb.h:720-723`'s `PB_ONEOF_NAME(MEMBER, tuple)` destructures the parenthesised oneof fieldname,
   so **oneof needs no special case**.
2. **The `##` trap.** `PB_TF_SUB_PB_LTYPE_MESSAGE(t) &(t##_tf)` does **not** work: `t` is adjacent
   to `##`, so C99 6.10.3.3p2 suppresses argument expansion and it emits
   `&(Msg_inner_MSGTYPE_tf)`. nanopb splits this across two macros for exactly this reason
   (`pb.h:746-747` → `pb.h:757`). Every `_MSGTYPE`-consuming macro needs that second level.
3. **Verified by build in milestone 7, not just by reading.** The milestone-1 plan called for the
   fixture to be generated twice — default naming and `--c-style` — and that never happened; the
   test app used `--c-style` only, so this fact rested on reading the generator. Writing the README
   surfaced it (the claim "generated twice" was false), and `tests/textformat/src/fixture3.proto` now
   closes it: default naming emits `FxDef_FIELDLIST` and `FxDef_sub_MSGTYPE`, so
   `PB_TF_DEFINE(FxDef, FxDef)` resolves from the same macro that handles
   `PB_TF_DEFINE(FX_SHAPES, fx_shapes_t)`. `NANOPB_OPTIONS` is read per
   `zephyr_nanopb_sources()` call, which is what makes two styles in one build possible.

   Under nanopb's **`--c-style`**, a message `sensor_config` gives FIELDLIST
   `SENSOR_CONFIG_FIELDLIST`, struct `sensor_config_t` and descriptor `sensor_config_t_msg` — three
   different spellings of one name.
   So the macro mirrors `PB_BIND(msgname, structname, width)`'s two-token form; keying the generated
   symbol off `structname` makes it naming-style-agnostic.
4. `pb.h:302-308` — **`PB_HTYPE_OPTIONAL == PB_HTYPE_SINGULAR == 0x10`** and
   **`PB_HTYPE_REPEATED == PB_HTYPE_FIXARRAY == 0x20`**. Indistinguishable at runtime, so the row
   carries the htype token from the macro; never read `PB_HTYPE(iter.type)`.
5. `pb.h:878-898` — the ltype mapping is **lossy**: `FLOAT`/`FIXED32`/`SFIXED32` all collapse to
   `PB_LTYPE_FIXED32`, `INT32`/`INT64`/`ENUM` to `PB_LTYPE_VARINT`. A runtime-only design *cannot*
   tell whether to print `1.0` or `1`. The row's `vtype` (pre-map ltype token) is the only correct
   source — this is why the table carries a type code, not just `{name, tag}`.
6. `pb_common.c:74-117` — `STATIC REPEATED`: `pSize` = `&msg->field_count`, `data_size` = **one
   element**. `STATIC OPTIONAL`: `pSize` = `&msg->has_field`. `STATIC SINGULAR` **and proto2
   `REQUIRED`**: `size_offset == 0` so `pSize` is **NULL** — dereferencing it is a null write.
   `FIXARRAY`: `pSize` points at `&iter->array_size`, *inside the iterator*.
7. `pb_common.c:188-193` — `pb_field_iter_next()` **wraps around**; iteration is `do/while`.
   `pb_common.c:156-164` — `pb_field_iter_begin()` returns **false for an empty message**
   (`field_count == 0`), which any marker message is; check the return value.
   Row order == `field_info` order (the generator sorts by tag and feeds the same FIELDLIST to
   `PB_BIND`), so the printer can drive `pb_field_iter_next()` in lockstep instead of re-`find`ing.
8. Nothing in nanopb stores a field-name string — not `pb_msgdesc_t` (`pb.h:349-360`), not the
   generated `.pb.c`. The X-macro `#fieldname` stringification is the only source. Under
   `--c-style`, `var_name()` runs `underscore()` (`nanopb_generator.py:171-188`), which is identity
   for `lower_snake_case` protos; a `camelCase` field would only be accepted mangled. Documented.
9. **Shell input.** *(Line numbers re-verified in milestone 7 before going into user-facing docs;
   three had drifted. The argc drop is `shell.c:275-280`, `active_cmd_prepare()` is
   `shell.c:588-591`, the raw handoff is `shell.c:804-806`, and quote/backslash stripping is
   `shell_utils.c:112-134`. One finding came out stronger than written: `CONFIG_SHELL_WILDCARD` is
   `default y if !SHELL_MINIMAL`, so the wildcard hazard is the **default state**, not an opt-in.)*

   *(Corrected again in milestone 7, this time by running it rather than reading it: the note that
   `args_left = mandatory - 1` "so the raw string arrives as argv[1]" was a wrong inference.
   `mandatory` counts the command word plus any **tokenized** mandatory arguments, and the raw string
   lands at `argv[mandatory]` — so `mandatory = 1` puts it at `argv[1]`, and `mandatory = 2` tokenizes
   one more word and puts the raw string at `argv[2]`. The raw argument is also optional, so `argc`
   can be 1. Zephyr's `tests/subsys/shell/shell/src/main.c` asserts both shapes. The sample built in
   this milestone failed on its first command, which is how this surfaced.)*

   `CONFIG_SHELL_ARGC_MAX` defaults to 20 and the shell **silently drops**
   everything past the cap (~8 text-format fields), and `make_argv` collapses whitespace runs — so
   joining `argv` silently corrupts both long messages and string values.
   `SHELL_OPT_ARG_RAW` on a leaf command makes `active_cmd_prepare` stop
   tokenising, and the shell hands the handler the **untouched remainder** of `cmd_buff`:
   no quote stripping, no backslash processing, no collapse, no ARGC cap. Document three caveats:
   don't wrap the argument in `'…'` (quotes are no longer stripped), `CONFIG_SHELL_CMD_BUFF_SIZE`
   caps the line at 256 by default, and `CONFIG_SHELL_WILDCARD=y` rewrites `cmd_buff` so `*`/`?`
   inside values are a hazard.
10. `nanopb_generator.py:643-648` — a **proto3 singular message field is `OPTIONAL` with a `has_`
    bit**, so a singular and an optional message field are byte-identical in the generated
    FIELDLIST. The `proto3_singular_msgs`
    option flips it to `SINGULAR` with no `has_`; the row-carried htype handles both (fact 6).
11. **`MSG_W_CB` needs nothing.** `nanopb_generator.py:789-793` puts `pb_callback_t cb_<name>;`
    *before* `bool has_<name>;`, and `size_offset` derives from `has_`, so the callback sits strictly
    before `pSize` and writing `pField` can never touch it. Alias it to `MESSAGE`.
12. **Widths are never assumable.** nanopb's `int_size` option (`nanopb.proto:30-31,68`) produces
    1- and 2-byte scalars; `PB_CONVERT_DOUBLE_FLOAT` (`pb.h:48`) stores a proto `double` as a C
    `float`; **`int_size: IS_32` on a proto `int64` narrows it to `int32_t`**; and an enum's
    `data_size` is implementation-defined (`-fshort-enums` → 1 or 2). Every numeric write must
    dispatch on `data_size` exactly as `pb_decode.c:1469-1478,1513-1522` does.
    **Corrected during milestone 1:** an earlier reading claimed `PB_WITHOUT_64BIT`
    (`CONFIG_NANOPB_WITHOUT_64BIT`, wired to the define at
    `zephyr/modules/nanopb/CMakeLists.txt:37-38`) narrows `int64` storage. It does not. The
    generator has no knowledge of the define — `nanopb_generator.py` never mentions it — so the
    struct member stays `int64_t`, and `pb_int64_t` is a **`pb_decode.c`-local macro**
    (`pb_decode.c:52-58`), not a public `pb.h` type. What the define actually does is make the
    codec **reject** a genuine 8-byte field at runtime with `"invalid data_size"`
    (`pb_decode.c:452-453`) and compile out `pb_decode_varint`/`pb_decode_fixed64` entirely
    (`pb_decode.c:229,1427`). So `int_size` is the only thing that changes a width, and the
    `no64` config proves 8-byte fields degrade cleanly — the same shape as `nofloat`, not a
    narrowing test.
13. `lib/libc/minimal/source/stdlib/` has `strtoull.c`/`strtoll.c` but **no `strtod.c`** — hence
    `depends on !MINIMAL_LIBC` on the float option. `Kconfig.cbprintf`'s `CBPRINTF_INTEGRAL_CONV`
    choice can drop 64-bit conversion, so 64-bit fields need `CONFIG_CBPRINTF_FULL_INTEGRAL`.
14. A message with `extensions 100 to max;` gets a synthetic row
    (`nanopb_generator.py:1090-1108`): `X(a, CALLBACK, OPTIONAL, EXTENSION, extensions, 100)`. Both
    the `EXTENSION` vtype and sub macros must exist or the TU won't compile.
15. `{` can never legally start a top-level text-format Message (`FieldName` is `[` or `IDENT`), so
    optional outer braces are unambiguous — provided the matching `}` is required to be the last
    token, so `{a:1} junk` errors.
16. `mps2/an385` (`boards/arm/mps2/mps2_an385.yaml`) is `simulation: qemu`, `testing: default: true`,
    Cortex-M3, 4 MB RAM/flash — the canonical replacement for the old `qemu_cortex_m3`.
    `qemu_x86` (`boards/qemu/x86/qemu_x86.yaml`) is the faster second target. The three modules a
    minimal manifest needs are **`cmsis_6`**, `nanopb` and `picolibc`. **Corrected during milestone
    0:** an earlier reading of `zephyr/west.yml:119` said `cmsis`, but in v4.4.0 Cortex-M takes
    `cmsis_core.h` from `zephyr/modules/cmsis_6/` (`Kconfig:8-10`,
    `select HAS_CMSIS_CORE_M if CPU_CORTEX_M`) while `zephyr/modules/cmsis/` now serves only
    Cortex-A/R — allowlisting `cmsis` fails the build with `fatal error: cmsis_core.h: No such file
    or directory`. Note west leaves a de-allowlisted project on disk; delete `modules/hal/cmsis` after
    changing the list.

17. **`anonymous_oneof` needs nothing.** With nanopb's `anonymous_oneof:true` the union is emitted
    without a name and the FIELDLIST row's tuple loses its prefix --
    `(pick,p_num,p_num)` instead of `(pick,p_num,pick.p_num)` (`nanopb_generator.py:938-948`).
    `PB_TF_DEFINE` reads only elements 1 and 2 via `PB_ONEOF_NAME(UNION/MEMBER, ...)`, and the
    `<type>_<union>_<field>_MSGTYPE` symbol is emitted identically either way
    (`nanopb_generator.py:1519-1526`) -- only `PB_ONEOF_NAME(FULL, ...)`, which nanopb itself uses
    for `offsetof`, differs. So both forms work unchanged, submessage members included.

## The dialect

Supported — all valid text format:

| Production | Example |
|---|---|
| scalar field | `delay: 100` |
| message field, colon optional | `inner { a: 7 }` / `inner: {a: 7}` |
| scalar / message short list | `tags: [1, 2, 3]` / `items: [{a: 1}, {a: 2}]` |
| empty list | `x: []` — legal per spec, parses as a no-op |
| repeated by repetition | `tags: 1 tags: 2` (appends) |
| optional `,` / `;` separators | `{a: 1, b: 2}` / `{a: 1; b: 2}` |
| integers | decimal, `0x` hex, **and octal** (`0755` → 493), optional `-` |
| floats | `1.0`, `-2.5e3`, `1f`, `inf`, `nan` (behind `CONFIG_NANOPB_TEXTFORMAT_FLOAT`) |
| bools | `true`/`false`/`True`/`False`/`t`/`f`/`1`/`0`; output is always `true`/`false` |
| strings / bytes | `"…"` / `'…'` with `\a \b \f \n \r \t \v \? \\ \' \" \xH[H] \NNN \uXXXX` |
| oneof | one member per union; switching members clears the union first |

Out of scope — each a strict narrowing except the last:

- `<msg>` angle-bracket delimiters (nothing generates them; protobuf's printer always emits `{}`)
- `#` line comments, maps, groups, extensions `[a.b]: v`, `Any` (`[type.url/T] {…}`)
- adjacent string concatenation (`"a" "b"`) — in the spec, never emitted by protobuf's printer
- **enum values by name**, both directions — evaluated and declined. A field row *can* reach its enum
  type (`nanopb_generator.py:1535-1553` emits `<Msg>_<field>_ENUMTYPE`, the analogue of `_MSGTYPE`),
  but nanopb generates **no value list**, `enum_to_string` is **one-way**, and under `--c-style` the
  type token is `mode_t` while the name function is `mode_name` and the bounds are
  `_MODE_MIN`/`_MODE_MAX` (`nanopb_generator.py:465-504`) — the preprocessor can neither uppercase
  nor strip `_t`, so none of those are derivable from the row. Under `--c-style` the generator
  also prepends the enum's own name to every value (`nanopb_generator.py:499`), so an idiomatically
  prefixed `FX_MODE_FAST` is emitted as `FX_MODE_FX_MODE_FAST` -- one more token shape the
  preprocessor cannot derive from a row. Names would cost a registration line
  per enum type plus the strings in flash. If revisited, the shape is a `pb_tf_enum_t` referenced
  from the row exactly like `sub`, so nothing here forecloses it.
- `\U000XXXXX` escapes and `\u` surrogate pairs — a lone surrogate is rejected rather than emitted
  as invalid UTF-8, which also keeps `CONFIG_NANOPB_VALIDATE_UTF8` builds from failing on a struct
  this parser filled
- `POINTER` / `CALLBACK` allocation, `FIXARRAY` (`fixed_count`), and `EXTENSION` fields → clear
  **runtime** errors, not build breaks, so one awkward field cannot break a whole message's build.
- proto2 `required` presence is **not** enforced — stated in the header, not silently ambiguous
- **one extension:** optional outer braces on *input*. On output they are a separate off-by-default
  flag, so compact mode emits `a: 1, b: 2` — valid standalone text format that protoc will parse.

One further documented narrowing, from milestone 4: **every raw byte below 0x20 is refused inside a
quoted literal**. protoc certainly rejects an unescaped newline; for a literal tab this is stricter
than protoc. Narrowing is safe for a subset, and costs nothing real -- the printer escapes control
characters so round-trip is unaffected, and shell input never carries a raw tab past line editing.

Documented caveats: field names must be `lower_snake_case` in the `.proto` (fact 8); `PB_TF_DEFINE`
is **file scope only**; init semantics are zero-init, not proto2 `X_init_default` (those defaults
live in `desc->default_value` and nanopb's decoder for them is `static`); and the byte capacity of a
`bytes` field is `data_size - offsetof(pb_bytes_array_t, bytes)`, which can exceed the declared
`max_size` by tail padding — same as nanopb itself, so don't advertise it as exact.

## Layout

Step 0 is a rename: `protobuf_textformat_parser` → `zephyr-nanopb-textformat` (the directory is empty, so
this is free, and doing it first avoids touching paths later).

```
zephyr-nanopb-textformat/
  CMakeLists.txt            zephyr_library under if(CONFIG_NANOPB_TEXTFORMAT)
  Kconfig                   NANOPB_TEXTFORMAT, _FLOAT, _MAX_DEPTH, + log_config template
  zephyr/module.yml         name: nanopb_textformat
  include/nanopb_textformat.h   public API + the PB_TF_DEFINE X-macro machinery
  src/nanopb_textformat_common.c        name lookup, iterator glue, pb_tf_strerror, pb_tf_clear
  src/nanopb_textformat_parse.c         lexer + iterative state machine + value binding
  src/nanopb_textformat_print.c         iterative printer, driven by cbprintf
  tests/textformat/         one ztest app, five suites, + testcase.yaml
  docs/dialect.md           which spec productions are in, which are out, and why
  ws/                       gitignored west workspace (see Verification)
  .gitignore                ws/ , twister-out/
```

### 1. Descriptor and X-macro

```c
typedef struct pb_tf_msg_s pb_tf_msg_t;

typedef struct {
    const char        *name;   /* NULL for the terminator row and for EXTENSION rows */
    uint16_t           tag;
    uint8_t            vtype;  /* PB_TF_VT_* — pre-map ltype token, closes fact 5 */
    uint8_t            rules;  /* atype | htype token, closes fact 4 */
    const pb_tf_msg_t *sub;    /* MESSAGE/MSG_W_CB only, else NULL */
} pb_tf_field_t;

struct pb_tf_msg_s {
    const pb_msgdesc_t  *desc;
    const pb_tf_field_t *fields;
    uint16_t             count;
    uint16_t             struct_size;  /* sizeof(structname) — enables pb_tf_clear */
};

#define PB_TF_DEFINE(msgname, structname)                                     \
    msgname##_FIELDLIST(PB_TF_GEN_EXTERN, structname)                         \
    static const pb_tf_field_t structname##__tf_fields[] = {                  \
        msgname##_FIELDLIST(PB_TF_GEN_ROW, structname)                        \
        { NULL, 0, 0, 0, NULL } /* sentinel: also fixes empty messages */     \
    };                                                                       \
    const pb_tf_msg_t structname##_tf = {                                     \
        &structname##_msg, structname##__tf_fields,                           \
        (uint16_t)(ARRAY_SIZE(structname##__tf_fields) - 1u),                 \
        (uint16_t)sizeof(structname)                                          \
    }
```

Three dispatch families, each copying a pattern nanopb already uses, each with the **two-level
indirection fact 2 requires**:

```c
#define PB_TF_SUB_PB_LTYPE_MESSAGE(t)   PB_TF_SUBDESC(t)   /* t expanded here… */
#define PB_TF_SUB_PB_LTYPE_MSG_W_CB(t)  PB_TF_SUBDESC(t)
#define PB_TF_SUBDESC(t)                &(t##_tf)          /* …pasted only here */
#define PB_TF_SUB_PB_LTYPE_BOOL(t)      NULL               /* + 18 more, all NULL */
```

- The 21 ltype tokens are `BOOL, BYTES, DOUBLE, ENUM, UENUM, FIXED32, FIXED64, FLOAT, INT32, INT64,
  MESSAGE, MSG_W_CB, SFIXED32, SFIXED64, SINT32, SINT64, STRING, UINT32, UINT64, EXTENSION,
  FIXED_LENGTH_BYTES` — **19 NULLs**, and `EXTENSION` and `FIXED_LENGTH_BYTES` are the two easy to
  forget (fact 14). **Implemented differently than first sketched:** rather than forcing
  `name = NULL`, an `EXTENSION` row keeps nanopb's placeholder name and `pb_tf_field_find()` skips
  it on `vtype`. Same guarantee -- "extensions" is never addressable -- but the name stays
  available for diagnostics, and the row macro needs no special case.
- For non-`MESSAGE` ltypes the replacement list ignores `t`, so `structname##_##fieldname##_MSGTYPE`
  is never substituted (C99 6.10.3.1p1) — exactly how nanopb's `PB_SI_PB_LTYPE_*` work.
- `PB_TF_GEN_EXTERN` emits `extern const pb_tf_msg_t <sub>_tf;` for message rows, nothing otherwise,
  and is expanded **first**, so declaration order is irrelevant. (Note the limit of that claim: a
  *self-referential* message has no finite STATIC layout, so nanopb only generates one under
  POINTER or CALLBACK allocation -- which this library rejects anyway. The forward declarations
  earn their keep for order independence and mutual references, not recursion.)
  Duplicate externs are legal C.
- Names: `PB_TF_NAME_SINGULAR(fn)` is a **direct `#fn`** — routing it through a two-level stringizer
  would prescan the argument, so a field named `errno` or `stdin` would stringize to its macro
  *expansion*. Only the `ONEOF` variant needs two levels, to expand `PB_ONEOF_NAME(MEMBER, fn)`
  before `#`. The two paths can disagree for such names; documented.

Plus `PB_TF_FIELD_COUNT(FIELDLIST)`, for `BUILD_ASSERT`s on the generated table.

### 2. Parser

Single pass, no heap, no scratch buffers, **and no recursion** (see below) — strings and bytes unescape **directly
into the destination**, bounded by `iter.data_size`. Writes go through `iter.pData` (identical to
`pField` for STATIC, but it is what nanopb uses and degrades better).

```
message(tf, msg, terminator):
  loop until terminator:
    ident  -> linear scan tf->fields (skipping name == NULL)
    row->vtype is MESSAGE ? optional ':' : required ':'
    value | '[' [ value {',' value} ] ']'      /* '[' only if rules is REPEATED */
    optional ',' | ';'
```

Storage dispatch on `row->rules`:

| rules | action |
|---|---|
| `SINGULAR` / `REQUIRED` | write `pData`. **Never touch `pSize` — it is NULL** (fact 6). Repeated occurrences of a singular message *merge*, which plain writes already give. |
| `OPTIONAL` | assert `pSize != NULL`, write `pData`, then `*(bool *)pSize = true` |
| `REPEATED` | `n = *(pb_size_t *)pSize`; `n >= array_size` → `ERR_TOO_MANY`; **`memset` element `n`** (an appended element must not inherit ghost bytes); write `(char *)pData + n * data_size`; commit `n + 1` **only on success** |
| `ONEOF` | if `*(pb_size_t *)pSize != tag`, **`memset(pData, 0, data_size)`** first — otherwise a new member inherits the previous member's union bytes; then set `which_` and write |
| `FIXARRAY` | `ERR_UNSUPPORTED` — "fixed_count fields are not supported" |
| `POINTER` / `CALLBACK` atype | `ERR_UNSUPPORTED`, with the field name |

Value writing dispatches on `row->vtype` **and** `iter.data_size`, mirroring
`pb_decode.c:1469-1478,1513-1522`'s chain verbatim (`sizeof(pb_int64_t)` / `int32_t` /
`int_least16_t` / `int_least8_t`, else `"invalid data_size"`) — this is what makes enums,
`int_size`, `PB_WITHOUT_64BIT` and `PB_CONVERT_DOUBLE_FLOAT` all correct (fact 12), and it means the
lexer accumulator uses `pb_tf_uint64_t`/`pb_tf_int64_t` aliases rather than hard-coded `uint64_t`.
Negative literals are rejected for the unsigned vtypes. `STRING` uses nanopb's exact bound —
`len + 1 <= data_size`, i.e. `data_size - 1` characters, NUL always written.

**No recursion — revised during milestone 3.** A submessage value is itself a field body, so the
natural shape is mutual recursion under `CONFIG_NANOPB_TEXTFORMAT_MAX_DEPTH`. That was the original
design and it was wrong for this target. Measured at `-Os` on Cortex-M3, the cycle
(`parse_fields` 32 B + `store_value` 56 B + `parse_value` 56 B) cost **144 bytes per level**: ~1.2 KB
at the default depth of 8 and ~4.6 KB at the Kconfig maximum of 32, against a
`CONFIG_SHELL_STACK_SIZE` that defaults to **2048** (`subsys/shell/Kconfig:50`) — and the intended
consumer *is* a shell command. A depth cap bounds the level count but converts to bytes nowhere, so
raising it silently spends stack. It is also a **MISRA C:2025 Rule 17.2** (Required, decidable)
violation, which no deviation would be worth taking for a library aimed at embedded targets.

The parser is therefore a three-state machine (`FIELD` → `VALUE` → `AFTER`) over an explicit array
of frames held in `struct pb_tf_parser`:

```c
struct pb_tf_frame {
    const pb_tf_msg_t   *tf;          /* message this body fills   */
    void                *msg;
    const pb_tf_field_t *row;         /* field in progress         */
    void                *commit_ptr;  /* parent's pSize, or NULL   */
    pb_size_t            commit_val;
    uint8_t              commit_kind; /* PRESENT | SIZE | NONE     */
    uint8_t              flags;       /* BRACED | IN_LIST          */
};                                    /* 20 bytes on a 32-bit target */
```

A submessage pushes a frame and re-enters `FIELD`; its closing brace commits, pops, and resumes the
parent in `AFTER` — exactly where a returning recursive call would have landed. The one subtlety
recursion handled for free: a repeated submessage's element count must not include an element whose
body failed halfway, so the **commit is deferred** and carried on the child frame rather than applied
at the call site.

Measured cost after the change — `132 + 20n` bytes, in **one** `-fstack-usage` line:

| `MAX_DEPTH` | recursive | iterative |
|---|---|---|
| 1 | ~176 B | **152 B** |
| 8 (default) | ~1.2 KB | **296 B** |
| 32 (maximum) | ~4.6 KB | **776 B** |

Verified against the linked image, not just the object file: `pb_tf_parse` makes no self-call, and
every callee is a leaf helper. (Unrelocated `bl` placeholders in a `.o` disassemble as apparent
self-calls, which is misleading — check the ELF.)

The printer inherits the same rule in milestone 5. Its depth is bounded by the `.proto` at build
time rather than by input, but a second stack-growth mechanism is not worth keeping for symmetry's
sake.

```c
typedef struct { pb_tf_err_t code; uint16_t offset; const char *field; } pb_tf_status_t;
/* -> "error at offset 14: value out of range for field 'threshold'" */
```

### 3. Printer

Walks the row table in declaration order, iteratively, over the same kind of frame array as the
parser (264 bytes flat on Cortex-M3, no recursion). Skips absent `OPTIONAL`, unselected `ONEOF`
members, and `EXTENSION` rows.

All formatting goes through `cbvprintf(out, ctx, ...)`, so the writer is Zephyr's own `cbprintf_cb`
and there is no temporary buffer in the print path. Float precision is explicit — `%.9g` for `float`,
`%.17g` for `double` — because a wrong precision silently breaks round-trip.

**Three deviations decided during milestone 5:**

1. **No lockstep iteration.** Fact 7 established that row order matches `field_info` order, so the
   printer *could* drive `pb_field_iter_next()` instead of re-`find`ing. That was the right call for
   a recursive printer holding the iterator in a local; with a flat frame array it means one
   `pb_field_iter_t` (~40 bytes) per open frame — 360 bytes at the default depth and 1.4 KB at the
   maximum. `pb_field_iter_begin()` + `pb_field_iter_find()` per occurrence costs a short scan over
   messages that have at most a couple of dozen fields, which is the cheaper side of that trade.
   Fact 7 still holds; it is simply not worth spending RAM on here.

2. **Repeated fields print as repetition, never as `[...]` lists.** `tags: 1 tags: 2` rather than
   `tags: [1, 2]`. Both are valid text format and both re-parse, but repetition is what protoc emits
   and it removes the list branch from the printer entirely.

3. **Escapes on output are always three octal digits**, never `\xHH`. Fixed width is the point:
   `\x0` followed by `a` reads back as the single byte `0x0A`, so a hex escape is ambiguous whenever
   the next byte happens to be a hex digit. Three octal digits cannot run on. This also keeps output
   pure ASCII, so a UTF-8 payload round-trips without the printer needing to understand encodings.

Shapes the library cannot represent — `POINTER`, `CALLBACK`, `FIXARRAY` — are **skipped** rather than
reported. Refusing to print a whole message because one field is a pointer is worse than an
incomplete view, and the parser already rejects those fields on the way in. Documented, not silent.

Flags: `PB_TF_MULTILINE` (default) / `PB_TF_COMPACT`, `PB_TF_BRACES` (off by default, so compact
output is valid standalone text format), `PB_TF_SKIP_DEFAULTS`. **Print-all is the default** —
implicit-presence scalars print even when zero, matching today's `get_config`, giving a complete
picture of the struct, and round-tripping byte-exact into a cleared target.

### Floats — as built in milestone 6

`strtod()` accepts a good deal more than text format does: hex floats (`0x1p3`), a leading `+`, its
own `inf`/`nan` spellings including `nan(chars)`. So the literal's shape is validated by a small
independent scanner and **`strtod()`'s endptr is required to agree with it** — the dialect stays a
strict subset however willing the C library is. The `f`/`F` suffix is handled after that check, since
`strtod()` will not consume it.

Two overflow paths, both of which would otherwise turn a typo into an infinity:

- a value that exceeds `FLT_MAX` but is a perfectly good double (`1e39`) would become `inf` when
  narrowed to a 4-byte field;
- a value that overflows the double itself (`1e309`) comes back from `strtod()` as `inf` with
  `ERANGE`.

Both report `PB_TF_ERR_RANGE`. **Only the `inf`/`nan` words may produce a non-finite value.**
Underflow to zero is left alone, as protoc leaves it.

The width, not the vtype, chooses the storage and the print precision — `PB_CONVERT_DOUBLE_FLOAT`
stores a proto `double` as a C `float`, and printing `%.17g` of a float's worth of information would
emit noise.

### 4. API

```c
pb_tf_err_t pb_tf_parse (const pb_tf_msg_t *tf, void *msg, const char *text,
                         unsigned flags, pb_tf_status_t *status);
                         /* clears msg unless PB_TF_NOINIT */
pb_tf_err_t pb_tf_print (const pb_tf_msg_t *tf, const void *msg,
                         cbprintf_cb out, void *ctx, unsigned flags);
void        pb_tf_clear (const pb_tf_msg_t *tf, void *msg);
const char *pb_tf_strerror(pb_tf_err_t code);
/* bundled writer; ctx is pb_tf_buf_t { char *buf; size_t size, used; bool ovf; } */
int         pb_tf_out_buf(int c, void *ctx);
/* added in milestone 5 -- see below */
pb_tf_err_t pb_tf_print_buf(const pb_tf_msg_t *tf, const void *msg, char *buf, size_t size,
                            unsigned flags);
```

`pb_tf_print_buf()` was not in the original API and exists because the streaming pair has a trap.
`pb_tf_out_buf()` terminates the buffer *as it writes*, so a message that prints **nothing** — an
empty message, or one whose every field is skipped under `PB_TF_SKIP_DEFAULTS` — never calls it and
leaves the caller's buffer untouched and unterminated. The wrapper terminates up front and turns
truncation into `PB_TF_ERR_NO_SPACE` instead of a flag the caller must remember to check. The
streaming pair remains for output that should not be buffered at all.

## Milestones

Each ends green under twister before the next starts.

0. **Rename + workspace bootstrap.** Rename the directory, write `ws/manifest/west.yml`, run
   `west init -l ws/manifest` then `west update --narrow --fetch-opt=--depth=1`, and confirm a
   hello-world builds and *runs* on both `mps2/an385` and `qemu_x86`. Nothing else is worth doing
   until QEMU actually boots on this machine.
1. **Skeleton + macro machinery.** Module scaffolding and `tests/textformat/src/fixture.proto`
   covering every shape: singular/optional/repeated scalars, submessage, repeated submessage, oneof,
   depth 3, non-STATIC allocation (POINTER and CALLBACK, standing in for the recursive message
   that cannot be STATIC at all), string/bytes/fixed-length-bytes, enum, float, an empty message, a
   `fixed_count` field, an `extensions` range, and an `int_size: IS_8` field. Test = *it compiles*,
   plus `BUILD_ASSERT`s on the table, plus the fixture built twice — default naming and `--c-style`
   — to lock fact 3.
2. **Lexer + scalar parsing.** Singular, optional, repeated, `[...]` incl. empty, separators, all
   integer bases, all bool spellings, the `data_size` dispatch chain, the full error table.
3. **Submessages.** Colon-optional `{}`, repeated submessages, message lists, oneof (incl. the
   union-clearing case), depth cap, and `FIXARRAY`/`POINTER`/`CALLBACK`/`EXTENSION` rejection.
4. **Strings and bytes.** Escapes both ways via `char2hex`/`hex2char`, `\u` → UTF-8, lone-surrogate
   rejection, nanopb's exact capacity bounds.
5. **Printer** over `cbprintf` + round-trip suite across every flag combination.
6. **Float** behind `CONFIG_NANOPB_TEXTFORMAT_FLOAT`, plus the `=n` variant proving float fields degrade
   to a clean `ERR_UNSUPPORTED` rather than a build break.
7. **Docs.** `README.md`, `docs/dialect.md`, and a shell integration guide (including the
   `SHELL_OPT_ARG_RAW` registration and its three caveats).

## Verification

### The workspace

`ws/` is a gitignored T2 workspace living inside the repo, so deleting its fetched contents fully
resets it.
`ws/manifest/west.yml` imports Zephyr with a three-module allowlist (fact 16):

```yaml
manifest:
  projects:
    - name: zephyr
      url: https://github.com/zephyrproject-rtos/zephyr
      revision: v4.4.0
      import:
        name-allowlist: [cmsis, picolibc, nanopb]
```

```
$ west init -l ws/manifest
$ cd ws && west update --narrow --fetch-opt=--depth=1
$ west packages pip --install          # zephyr + nanopb python deps
```

`--narrow` fetches only the manifest-named revisions rather than every remote ref, and
`--fetch-opt=--depth=1` makes each fetch shallow — together a fraction of the download for a
workspace that only ever builds. The trade-off is no usable history in the fetched projects
(`git log`/`bisect`/`describe` won't work); deepen a single project with `git fetch --unshallow` if
that's ever needed.

The module is injected into the test build via `EXTRA_ZEPHYR_MODULES` pointing at the repo root —
which is the conventional way to test a module in its own repo. Note the two `zephyr` directories at
different levels (`<repo>/zephyr/module.yml`, the module manifest, vs `<repo>/ws/zephyr/`, the
fetched tree) are unrelated and unambiguous.

### The suite

One ztest app, `tests/textformat/`: `CMakeLists.txt` with
`set(NANOPB_OPTIONS "--c-style")` + `zephyr_nanopb_sources`, a `prj.conf` (`CONFIG_ZTEST`,
`CONFIG_NANOPB`, `CONFIG_NANOPB_TEXTFORMAT`, `CONFIG_CBPRINTF_COMPLETE`, `CONFIG_CBPRINTF_FP_SUPPORT`,
`CONFIG_CBPRINTF_FULL_INTEGRAL`), and a `testcase.yaml` with
`platform_allow: mps2/an385, qemu_x86`.

Suites: `macro` (table shape, `BUILD_ASSERT`s, both naming styles), `parse` (per-shape, nesting ≥ 2,
oneof member switching, append semantics, merge semantics for singular submessages), `errors`
(asserts `{code, offset, field}` for unknown field, missing colon, unterminated brace/string/list,
bad escape, lone surrogate, over-range integer, over-long string/bytes, repeated past `array_size`,
depth overflow, trailing junk after `}`, `[` on a non-repeated field, and each unsupported shape),
`print` (both modes × braces × skip-defaults), `roundtrip` (parse → print → parse with `memcmp`
struct equality plus text equality in compact mode).

Three `testcase.yaml` entries, so the width-dispatch claim in fact 12 is actually exercised rather
than asserted:

| entry | extra_configs | proves |
|---|---|---|
| `nanopb_textformat.core` | — | everything |
| `nanopb_textformat.nofloat` | `CONFIG_NANOPB_TEXTFORMAT_FLOAT=n` | float fields degrade cleanly |
| `nanopb_textformat.no64` | `CONFIG_NANOPB_WITHOUT_64BIT=y` | genuine 8-byte fields degrade cleanly (they do **not** narrow — see fact 12) |

```
$ cd ws && west twister -T ../tests -p mps2/an385 -p qemu_x86 --inline-logs
```

Note on the "C99" claim: `pb.h:210-213` defaults `PB_STATIC_ASSERT` to `_Static_assert`, so any TU
including `pb.h` needs C11. The library is C99-compatible source compiled at C11, like nanopb.

Real-shell confirmation of `SHELL_OPT_ARG_RAW` (fact 9) needs an actual shell, which arrived with
the sample in milestone 7 — and promptly showed this document's reading of `mandatory` to be wrong.

## Out of scope here

Consuming the library from an application: replacing a per-field macro framework's parse and print
macros with `pb_tf_parse` / `pb_tf_print`, re-registering commands as `SHELL_OPT_ARG_RAW` leaves,
emitting one `PB_TF_DEFINE` line per message from whatever generates the message glue, and adding
this repository to the consuming project's west manifest. `samples/textformat/` shows the shape of all
of that in miniature.

---

## Status

All seven milestones are complete. Verified on Zephyr v4.4.0 with nanopb `5499fd4c`:

```
west twister -T tests -p mps2/an385 -p qemu_x86
8/8 configurations, 878/878 test cases, no warnings
```

Two design decisions were revised during implementation, both recorded above with their
measurements: **no recursion** in either direction (parser rewritten, printer built that way), and
**no lockstep iteration** in the printer. Both traded a micro-optimisation for bounded, visible RAM.

Corrections made to this document while implementing, each verified against the pinned trees:
`cmsis` → `cmsis_6` (fact 16); `PB_WITHOUT_64BIT` does not narrow storage (fact 12); `EXTENSION` rows
keep their name and are skipped on vtype; forward declarations do not make self-referential messages
work, since those have no STATIC layout; fact 3 verified by a second naming style rather than by
reading; fact 9's line numbers re-verified and its wildcard caveat strengthened.

One API addition beyond the plan: `pb_tf_print_buf()`, because `pb_tf_out_buf()` cannot terminate a
buffer for a message that prints nothing.

**Superseded after 0.1.0, so read the API below as history.** `README.md` and `docs/dialect.md`
describe the library as it is; this file describes how it was arrived at. What changed:

- the runtime `flags` arguments designed here — `PB_TF_NOINIT` and the four print flags — became
  Kconfig options plus `pb_tf_merge()` in 0.3.0, and then, for the print style, eight
  style-named functions in 0.6.0;
- the `_t` typedefs used throughout became plain `struct` and `enum` tags in 0.6.0, and the
  functions returning `pb_tf_err_t` now return `int` — 0 or a negated code, as Zephyr does.

What remains is consumption by an application, listed under "Out of scope" above and deliberately
not part of this repository. `samples/textformat/` shows the shape of it.