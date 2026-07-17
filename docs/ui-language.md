# The ui markup language — assembly tier

The markup language silhouette's compiler (`uic`, `src/uic/`) compiles
into C++ that draws through the Sink concept (`src/paint/sink.hpp`).
This document is the language's normative spec; semantics chapters land
with their conformance vectors, and the parser chapter below tracks what
`uic` implements today. The assembly tier is deliberately low-level — a
faithful porting surface for existing retained-mode UI trees; a
higher-level tier may later choose different laws.

## Design rules

- **Generated code depends on silhouette headers and sibling generated
  code only.** No runtime library grows behind the language; layout is
  compiled to specialized straight-line arithmetic per module.
- **Everything folds at compile time**: template parameters, asset
  references, host-call bindings. Nothing about a template ships as
  runtime data. String interpolation does not exist.
- **Only text is quoted.** Dimensions, enum words, colors, numbers,
  booleans, style names, resource paths, and identifiers are unquoted
  literals; quoted strings appear only where a human-readable string is
  the value (label content, format strings).
- **Names are identifiers.** A widget name declares a compile-time
  symbol — one distinct entity per template instantiation — referenced
  by address in generated code. No name strings exist at runtime.
- **Data flows one way.** A module renders a host-provided snapshot
  struct (declared in schema modules); interaction is declared with
  `action` and compiles to typed calls on a host-implemented class —
  an undefined method fails at link time.

## Lexical structure

- UTF-8 text; `//` comments to end of line.
- **Identifiers**: `[A-Za-z_][A-Za-z0-9_]*`.
- **Numbers**: integer or decimal, optional leading sign in value
  positions.
- **Dimension literals**: a number glued to a unit suffix from
  `% @ h w a s p i` (e.g. `100%`, `-7.5%`, `20.2h`, `+32%`). The unit
  semantics are the layout chapter's (pending vectors); the lexer only
  recognizes the shape.
- **Color literals**: `#rrggbb` / `#rrggbbaa`, or (in plain attribute
  values) whitespace-separated float components.
- **Resource paths**: `/`-rooted unquoted literals (`/art/frame.img`);
  no whitespace. The compiler statically validates every authored path
  against the build's asset root, including expansion families
  (nine-slice suffixes, flipbook frames).
- **Strings**: double-quoted with `\"` and `\\` escapes. Text only.
- **Plain attribute values are verbatim**: after `name:` in a widget or
  style body, everything up to the terminating `;` is the value, kept
  as written (this is what lets ported trees stay diffable against
  their source material).

## Module items

```
import { Name, Other } from "relative/module.ui";
const kMask: asset = /art/mask.img;
struct Frames { up: asset; over: asset; }        // fields typed; defaults allowed
const Standard = Frames { up: /art/f_up.img; over: /art/f_over.img; }
style centered { align: center; valign: center; }
fn clamped(v: int, hi: int) -> int = v > hi ? hi : v;
native fn fmtTime(seconds: double) -> str {{{
    /* verbatim C++, emitted into the generated module */
}}}
template button_face { ... }                     // see below
panel { ... }                                    // top-level widget tree
```

## Widgets

A widget is `tag { body }`. A template in scope instantiates with the
same shape — invocation is a plain tag; template names share the tag
namespace with builtin widget classes and collisions are errors.

Body statements:

- `attr: value;` — a plain attribute, value verbatim to `;`.
- `bind attr: expr;` — the attribute becomes a pure function of the
  snapshot (and constexpr surroundings). Bound dimensions take typed
  unit values; two restricted own-geometry references exist
  (`self.height`, `parent.parent.width`) with a schedule-checked
  position.
- `action event: host.Method(args);` — declares what an input event
  does. The compiler collects every `host.*` call, unifies signatures,
  and emits the host class declaration; hit regions carry typed thunks.
- `if (expr) { widgets } else if (expr) { } else { }` — **structural**
  arms: a dead arm's subtree does not exist. Distinct from a bound
  `visible`, which follows the layout chapter's seat-keeping laws.
  Arms over an enum without `else` must be exhaustive.
- `widgetstate name { widgets }` — one state renders, selected by
  `bind state:` on the owning widget.
- nested widgets / template invocations.

## Templates

```
template member_cell {
    in a: SlotState;        // typed; wired with expressions at the call site
    in col: align;          // enum-typed parameter
    in frames: Frames;      // struct-of-assets parameter
    in pad: dim = 1.5h;     // defaults allowed
    panel { align: col; ... }
}
```

Parameters are typed and fold at instantiation; each instantiation
expands inline in generated code where possible. There is no textual
parameter substitution of any kind.

## Reserved words

`import from const struct style template in bind action if else
widgetstate fn native let match uses true false self parent snapshot
screen`.

## Status (what uic implements)

- **Parser** (`src/uic/`): the full surface above — modules, imports,
  consts (raw and struct-init), structs, styles, fns (expression-bodied)
  and native fns, templates with `in` params, widgets with plain
  attrs / binds / actions / structural if-chains / widgetstates, and the
  expression grammar (ternary, logical, comparison, additive,
  multiplicative, unary, calls, field access, indexing; number / dim /
  color / path / string / bool literals). Errors carry file:line:col;
  parsing recovers at statement boundaries and reports multiple errors.
- **Pending, each landing with conformance vectors**: the schema pass
  (struct → C++ header), instantiation/fold, asset validation, the
  layout chapter (unit grammar, flow/grow scheduling, rounding laws),
  the text chapter (cell alignment, pass structure, metrics via the
  sink's font surface), emission, and the host-class pass.
