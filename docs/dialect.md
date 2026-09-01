# The dialect

This library implements a **subset** of the [protobuf text format][spec]. The contract is one-way but
strict:

> Everything this library accepts is valid text format that protobuf's own parser accepts.
> Everything it prints, protobuf's own parser accepts.

The reverse does not hold — there are valid text-format documents it rejects. Each narrowing below is
deliberate, and rejecting is always the safe direction for a subset: a narrowing turns an
unsupported construct into an error instead of a silent misparse.

There is exactly **one** extension beyond the spec, called out under [Extensions](#extensions).

[spec]: https://protobuf.dev/reference/protobuf/textformat-spec/

## Supported

| Production | Example |
|---|---|
| scalar field | `delay: 100` |
| message field, colon optional | `inner { a: 7 }` · `inner: {a: 7}` |
| scalar short list | `tags: [1, 2, 3]` |
| message short list | `items: [{a: 1}, {a: 2}]` |
| empty list | `tags: []` — legal per spec, parses as a no-op |
| repeated by repetition | `tags: 1 tags: 2` (appends) |
| optional separators | `{a: 1, b: 2}` · `{a: 1; b: 2}` |
| integers | decimal, `0x` hex, leading-zero octal (`0755` → 493), optional `-` |
| floats | `1.0`, `-2.5e3`, `.5`, `1.`, `1f`, `inf`, `infinity`, `nan` |
| bools | `true` `false` `True` `False` `t` `f` `1` `0` |
| strings / bytes | `"…"` or `'…'` |
| escapes | `\a \b \f \n \r \t \v \? \\ \' \"`, `\xH[H]`, `\NNN`, `\uXXXX` |
| oneof | one member per union; switching members clears the union first |

`inf`, `infinity` and `nan` are matched case-insensitively, with an optional leading sign.

## Not supported

Each of these is a strict narrowing. Nothing here is accepted-but-ignored — all of them are errors.

### Syntax

| Construct | Why |
|---|---|
| `<msg>` angle-bracket delimiters | Nothing generates them; protobuf's own printer always emits `{}` |
| `#` line comments | Not needed for a shell command line |
| maps | Requires the `map<k,v>` entry-message machinery |
| groups | Deprecated proto2 construct |
| extension fields `[a.b]: v` | Needs an extension registry |
| `Any` — `[type.url/T] {…}` | Needs type resolution at runtime |
| adjacent literal concatenation `"a" "b"` | In the spec, but never emitted by protobuf's printer |
| `\U000XXXXX` escapes | See [Unicode](#unicode) |
| `\u` surrogate pairs | See [Unicode](#unicode) |
| leading `+` on a number | The spec has no `+` in its numeric literals |
| hex floats (`0x1p3`) | `strtod()` accepts them; text format has no such literal |
| raw bytes below `0x20` inside a literal | See [Control characters](#control-characters) |

### Enum values by name

**Numeric only, in both directions.** `mode: 2`, never `mode: FX_MODE_FAST`.

This was evaluated and declined rather than overlooked. A field row *can* reach its enum type —
nanopb emits `<Msg>_<field>_ENUMTYPE`, the analogue of the `_MSGTYPE` this library already uses for
submessages — but:

- nanopb generates **no value list** to search;
- its `enum_to_string` helper is **one-way**, so it could not serve parsing;
- under `--c-style` the type token is `mode_t` while the name function is `mode_name` and the bounds
  are `_MODE_MIN`/`_MODE_MAX`. The preprocessor can neither uppercase a token nor strip `_t`, so none
  of those names are derivable from a FIELDLIST row;
- `--c-style` also prepends the enum's own name to each value, so an idiomatically-prefixed
  `FX_MODE_FAST` is emitted as `FX_MODE_FX_MODE_FAST` — another shape the preprocessor cannot
  reconstruct.

Supporting names would cost a registration line per enum type plus the strings in flash. If it is
ever wanted, the shape is a `pb_tf_enum_t` referenced from the row exactly like `sub`, so nothing
here forecloses it.

### Field shapes

These are **runtime** errors, never build failures — that distinction is the point, so that one
awkward field cannot break a whole message's build:

| Shape | Parse | Print |
|---|---|---|
| `POINTER` allocation | `PB_TF_ERR_UNSUPPORTED` | skipped |
| `CALLBACK` allocation | `PB_TF_ERR_UNSUPPORTED` | skipped |
| `FIXARRAY` (nanopb `fixed_count`) | `PB_TF_ERR_UNSUPPORTED` | skipped |
| synthetic `EXTENSION` row | not addressable → `PB_TF_ERR_UNKNOWN_FIELD` | skipped |
| 8-byte field under `PB_WITHOUT_64BIT` | `PB_TF_ERR_UNSUPPORTED` | `PB_TF_ERR_UNSUPPORTED` |
| float field under `FLOAT=n` | `PB_TF_ERR_NO_FLOAT` | `PB_TF_ERR_NO_FLOAT` |

The printer **skips** what it cannot represent rather than refusing the whole message: an incomplete
view of a struct is more useful than no view because one field is a pointer. The parser rejects those
same fields on the way in, so nothing is silently lost on a round-trip through text.

## Extensions

**Optional outer braces on input.** `{a: 1, b: 2}` and `a: 1, b: 2` are both accepted.

This is unambiguous: a top-level text-format `Message` can only begin with a field name or `[`, so a
leading `{` cannot be anything else. It is only safe because the matching `}` is **required to be the
last token** — `{a: 1} junk` is an error.

On **output** braces are a separate, off-by-default flag (`PB_TF_BRACES`), so compact output is
valid standalone text format that `protoc` will parse as-is.

## Semantics worth knowing

### Presence and repetition

- A `singular`/`required` field has no presence bit. Writing it twice overwrites; for a submessage
  that means the two mentions **merge**, which is what text format asks for.
- An `optional` field's presence bit is set on a successful write — including for a zero value.
- A `repeated` field **appends**, whether written as a list or by repetition, so `tags: [1,2] tags: 3`
  yields three elements. Each appended element is cleared first, so it cannot inherit bytes from a
  previous, longer occupant.
- A failed value is **not** committed: if the third element of a list is out of range, the count still
  reads two.
- Switching `oneof` members clears the union first, so a new member never starts from the previous
  one's bytes.

### Initialisation

`pb_tf_parse()` **zeroes** the message first, unless `PB_TF_NOINIT` is passed.

Zeroing is not the same as applying proto2 defaults. Those live in `desc->default_value`, and
nanopb's decoder for them is `static`, so this library cannot reach it. If you need proto2 defaults,
pass `PB_TF_NOINIT` over a struct you initialised with `<MSG>_init_default` yourself.

Proto2 `required` presence is **not enforced**. A message missing a required field parses fine; whether
that matters is the caller's business, not the parser's.

### Integer widths

Every numeric write dispatches on the field's own `data_size`, mirroring nanopb's own chain. This is
why the descriptor row carries a type code rather than just `{name, tag}` — nanopb's runtime
`pb_type_t` is lossy, collapsing `INT32`/`INT64`/`ENUM` into one varint type and
`FLOAT`/`FIXED32`/`SFIXED32` into one fixed32 type.

Consequences you can rely on:

- nanopb's `int_size` option is honoured. `int_size: IS_8` gives a 1-byte field, and `128` is a range
  error rather than a silent truncation to `-128`.
- An enum's width is implementation-defined (`-fshort-enums` makes it 1 or 2 bytes) and is read from
  the descriptor, not assumed.
- A negative literal in an unsigned field is a **range error**, never a wrap.
- `PB_WITHOUT_64BIT` does *not* narrow storage — the generator never sees it, so an `int64` member
  stays 8 bytes. It narrows the codec, so a genuine 8-byte field reports `PB_TF_ERR_UNSUPPORTED`.
  Real narrowing comes from `int_size` instead.

### Floats

`strtod()` is used but not trusted: the literal's shape is validated independently and `strtod()`'s
`endptr` must agree, which is what keeps hex floats and `nan(chars)` out.

Only the words `inf`/`infinity`/`nan` may produce a non-finite value. A numeric literal that
overflows reports `PB_TF_ERR_RANGE` — both `1e39` into a 4-byte field (fine as a double, would become
`inf` when narrowed) and `1e309` into an 8-byte one (`strtod()` itself returns `inf`). Underflow to
zero is left alone, as `protoc` leaves it.

Printing uses `%.9g` for a 4-byte field and `%.17g` for an 8-byte one — the shortest precisions that
round-trip. The *width* decides, not the declared proto type, because `PB_CONVERT_DOUBLE_FLOAT`
stores a proto `double` as a C `float`.

### Strings and bytes

Capacities follow nanopb's own arithmetic, not the declared `max_size`:

| Type | Capacity |
|---|---|
| `string` | `data_size - 1` characters; the NUL is always written |
| `bytes` | `data_size - offsetof(pb_bytes_array_t, bytes)` |
| `fixed_length_bytes` | exactly `data_size` bytes, **or** empty |

For `bytes` this can exceed the declared `max_size` by whatever tail padding the struct carries. That
is true of nanopb itself, so it is not advertised as exact — do not rely on the extra room.

An empty `fixed_length_bytes` value means **all zeros**, matching nanopb's own special case. A
non-empty value of the wrong length is `PB_TF_ERR_TYPE`, since `PB_TF_ERR_NO_SPACE` would claim it
was too long when it is too short.

Values are unescaped **straight into the destination field**. There is no scratch buffer, so a value
that will not fit is refused at the byte that overflows.

#### Unicode

`\uXXXX` is encoded as UTF-8. A **lone surrogate** (`\ud800`–`\udfff`) is refused rather than encoded.

`\U000XXXXX` and surrogate pairs are out of scope, so no input can reach a code point above U+FFFF.
Together these mean this library can never emit ill-formed UTF-8 — which also keeps a
`CONFIG_NANOPB_VALIDATE_UTF8` build from rejecting a struct this parser just filled.

#### Control characters

Every raw byte below `0x20` inside a quoted literal is refused. `protoc` certainly rejects an
unescaped newline; for a literal tab this is **stricter** than `protoc`. Narrowing is safe for a
subset, and costs nothing real here: the printer escapes control characters, so round-trip is
unaffected, and shell line editing never delivers a raw tab anyway.

#### Output escaping

On output, anything outside printable ASCII becomes a **three-digit octal** escape — never `\xHH`.

The fixed width is the whole point. `\x0` followed by `a` reads back as the single byte `0x0A`, so a
hex escape is ambiguous whenever the next byte happens to be a hex digit. Three octal digits cannot
run on. This also keeps output pure ASCII, so a UTF-8 payload survives round-trip without the printer
needing to understand encodings.

## Field naming

Field names come from stringifying the `.proto` field name, which is the only source available —
nothing in nanopb stores a field-name string, not `pb_msgdesc_t` and not the generated `.pb.c`.

Under `--c-style`, nanopb runs field names through `underscore()`, which is the identity for
`lower_snake_case`. A `camelCase` field in the `.proto` would therefore only be addressable by its
mangled spelling. **Keep `.proto` field names `lower_snake_case`** — which the protobuf style guide
asks for anyway.

One pathological case: a field named after a macro in scope (`errno`, `stdin`) stringifies to its own
spelling for a normal field but could differ for a `oneof` member, because the oneof path must expand
`PB_ONEOF_NAME` before stringifying. Avoid naming fields after macros.

## Error codes

| Code | Meaning |
|---|---|
| `PB_TF_OK` | success |
| `PB_TF_ERR_SYNTAX` | unexpected token, unterminated brace/literal/list, malformed escape |
| `PB_TF_ERR_UNKNOWN_FIELD` | no field of that name in this message |
| `PB_TF_ERR_TYPE` | literal is the wrong shape for the field's type |
| `PB_TF_ERR_RANGE` | numeric literal does not fit the field's width |
| `PB_TF_ERR_TOO_MANY` | repeated field already holds `array_size` elements |
| `PB_TF_ERR_NO_SPACE` | string or bytes value longer than the destination |
| `PB_TF_ERR_DEPTH` | nesting deeper than `CONFIG_NANOPB_TEXTFORMAT_MAX_DEPTH` |
| `PB_TF_ERR_UNSUPPORTED` | field shape this build cannot handle |
| `PB_TF_ERR_NO_FLOAT` | float field with `CONFIG_NANOPB_TEXTFORMAT_FLOAT=n` |
| `PB_TF_ERR_INTERNAL` | descriptor inconsistency: a bug here, or a nanopb mismatch |

`pb_tf_status_t` carries the byte `offset` into the input and, where one was in scope, the `field`
name. The strings name only the fault, so a caller can append context without repeating a word:
`at offset 14: value out of range in field 'threshold'`.
