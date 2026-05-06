# `ice idf ldgen`

`ice idf ldgen` reads ESP-IDF linker fragment files (`.lf`) and an
entity database (the set of archives the build will link), and emits a
linker script that places every input section in the output section
its `.lf` rules dictate.

It is a from-scratch reimplementation of `tools/ldgen/ldgen.py`.  The
grammar of `.lf` files is preserved; the emission strategy is not.

This document covers:

1. **Design** — the rule-driven approach and why it produces a
   linker script that is order-independent and unambiguous.
2. **Pipeline** — what `gen_compile` / `gen_resolve` / `gen_emit` do.
3. **Output format** — the shape of the generated linker script and
   the role of `EXCLUDE_FILE` in the root catch-all.
4. **Worked examples** — small `.lf` snippets and the lines they
   produce.
5. **Grammar** — the lexical and syntactic rules of `.lf` files,
   transcribed from the upstream pyparsing definitions.

## Design

### The problem

A `.lf` file declares **rules** that bind input sections (matched by
glob patterns) to output sections (called *targets*).  A rule may
attach wrappers — `KEEP`, `SORT`, `ALIGN`, `SURROUND` — that bracket
its input section description in the generated linker script.  The
linker, given the resulting script, consults each output section's
input section descriptions in order and routes input sections from
input files to output sections by **first-match-wins**.

There are two well-known emission strategies:

- **Wildcards plus EXCLUDE_FILE** (Python ldgen).  Each rule emits a
  glob like `*(.text)`; rules with overlapping patterns use
  `EXCLUDE_FILE(...)` to exclude inputs claimed by more-specific
  rules, plus *intermediate placements* that explicitly enumerate
  which sections of the excluded files still belong to the broader
  rule.  Concise output but requires substantial machinery to compute
  the exclusion graph (entity tree, basis chains, intermediate
  placements) — roughly two thirds of the upstream Python
  implementation.

- **Explicit listings** (this implementation).  Walk the entity
  database, resolve every `(archive, object, section)` triple to its
  most-specific rule, and emit each triple by **literal section
  name** under that rule's target.  No globs in the output for known
  inputs.  The linker has zero ambiguity to resolve, so output-section
  order in the template becomes irrelevant.

The trade-off is verbosity for robustness.  `sections.ld` grows from
~200 lines (Python) to ~3000 lines (this implementation) for a
typical IDF project.  Linker scripts are not read by humans on a
regular basis and are regenerated on every reconfigure, so size cost
is negligible.  In return:

- Output-section order in `sections.ld.in` no longer affects
  correctness.  The template author can place targets in any order.
- Symbol carve-outs (e.g. one function from `bar.o` in IRAM, the rest
  in IROM) work without `EXCLUDE_FILE` because each section is named
  in exactly one place.
- `KEEP` / `SORT` / `ALIGN` / `SURROUND` wrappers always emit, even
  for rules whose patterns matched zero input sections — so boundary
  symbols defined by `SURROUND(<name>)` are always present, even
  when the surrounded section is empty in the current build.  This
  is load-bearing for components like `espcoredump` that reference
  `_<name>_start` / `_<name>_end` from C unconditionally.

### Why glob lines need `EXCLUDE_FILE`

In the explicit-listings approach, each `(archive, object, section)`
triple from a known archive is named literally in the output, so
inputs from the entity DB have only one matching input section
description in the entire script.  Globs appear in only two places:

1. The root rule's catch-all (`*(...)`) — to handle archives ldgen
   never sees (toolchain libraries `libgcc.a`, `libc.a`, prebuilt
   blobs, anything resolved via a bare `-l<x>` flag).
2. An archive-specific rule's fallback (`*<archive>(...)`) — when the
   rule names an archive that isn't in the libraries-file (typically
   a prebuilt blob the user wants routed to a specific target).

Both are *globs*: they match more than the rule's authored scope.
That creates ambiguity whenever a more-specific rule routes a subset
of the same content to a different target.  Without protection, the
linker's first-match-wins would resolve the ambiguity by **template
order**, which we explicitly do not want.

The protection is per-glob `EXCLUDE_FILE`.  At resolution time,
every rule that narrows a less-specific basis routing to a different
target appends its file pattern to that basis's per-rule
`exclude_list`.  At emit time the basis's glob carries that list:

    *(EXCLUDE_FILE(<patterns>) <section_patterns>)
    *libfoo.a(EXCLUDE_FILE(<patterns>) <section_patterns>)

The format of each entry depends on what kind of glob will consume it:

| Basis | Contributor | Entry shape | Why |
|-------|-------------|-------------|-----|
| Root catch-all | Archive rule | `*libfoo.a` | Whole archive is enumerated in matches; catch-all needn't see it. |
| Root catch-all | Object rule, archive in DB | `*libfoo.a` | Other objects in libfoo.a are in archive/root's literal listings; whole-archive exclude is exact. |
| Root catch-all | Object rule, archive **not** in DB | `*libfoo.a:bar.*` | Other objects of libfoo.a must keep falling through so the catch-all routes them to the basis target. |
| Archive fallback | Object rule | `*libfoo.a:bar.*` | Per-object always — an archive-level entry would shadow the entire `*libfoo.a(...)` selector. |

If an unknown archive contains a section that needs special placement
(a prebuilt blob with `.iram_*` the user wants in IRAM), it falls
through to the linker's `--orphan-handling=error` and produces a
loud error naming the section.  The user fixes by adding the archive
to the libraries-file or writing a `.lf` rule that names it
explicitly (which makes the archive's content reachable via the
rule's fallback selector and adds it to relevant `EXCLUDE_FILE`
lists).  Failure is never silent.

### Symbol-rule lifecycle

Symbol-specific rules (`bar:hot_func (in_iram)`) are special:
they're effectively single-section placements (`.text.hot_func`,
`.literal.hot_func`).  Two failure modes are caught at resolution
time and surfaced as warnings rather than silent drops or downstream
link errors:

- **Archive or object not in DB.**  The rule's archive isn't in the
  libraries-file, or the archive is there but doesn't contain the
  named object.  No DB section can carry the rule, so the rule is
  marked dropped before the resolution walk and the user gets a
  diagnostic naming the missing entity.

- **Pattern matched no real section.**  The archive and object are
  in the DB but no section matches the rule's symbol-substituted
  patterns (`.text.<sym>`, `.text.<sym>.*`, …).  The function or
  data the rule named was inlined or eliminated by the compiler.
  The rule is marked dropped after the resolution walk and a
  diagnostic suggests the inlining/DCE explanation.

Dropped rules don't emit anything (no frame, no body, no
`EXCLUDE_FILE` contribution).  If the rule's `SURROUND` symbols are
referenced from C, the link will fail with `undefined reference`,
naming the symbols — that's the user's signal to fix the rule.

## Pipeline

The three phases are exposed as separate functions in `gen.h`:

    gen_compile() ── parsed .lf fragments ──▶ rules
    gen_resolve() ── rules + entity DB ─────▶ rules with matches
                                              + per-rule exclude_list
                                              (symbol rules dropped
                                               on no-DB-entity / no
                                               matching section)
    gen_emit()    ── rules ─────────────────▶ linker script

### `gen_compile`

Parses `.lf` files into rules.  One `gen_rule` is produced per
*(mapping-entry × scheme-entry)* cross-product, because a single
mapping entry like `* (default)` means "all of this archive's
content follows the default scheme", and the default scheme may bind
several section families to several targets — each scheme entry
becomes its own rule.

Conditional arms (`if` / `elif` / `else` evaluated against
sdkconfig) are resolved during this pass, so only the rules from
selected branches reach the rule list.

The compiled rule list is sorted by **(specificity ASC, source-order
ASC)**.  The sort puts the most-specific rules at the *end* of the
array so `gen_resolve`'s reverse-walk implementation of
most-specific-wins is a single linear pass.

A rule has the shape:

    struct gen_rule {
        char    *archive;            /* NULL = "*" */
        char    *object;             /* NULL = "*" */
        char    *symbol;             /* NULL = object- or archive-level */
        char   **section_patterns;   /* expanded globs from [sections] */
        char    *target;
        gen_flag *flags;             /* KEEP / SORT / ALIGN / SURROUND */
        int      specificity;        /* 0..3 */
        int      source_order;
        gen_rule_match *matches;     /* filled by gen_resolve, pass 2 */
        char    *exclude_list;       /* filled by gen_resolve, pass 4 */
        int      dropped;            /* set by gen_resolve, pass 1/3 */
    };

Specificity is encoded:

    archive  object  symbol  -> specificity
    NULL     NULL    NULL    -> 0  (root, "* (foo)")
    set      NULL    NULL    -> 1  (archive-only)
    set      set     NULL    -> 2  (object-level)
    set      set     set     -> 3  (symbol-level)

Section patterns marked `+` in the `[sections]` fragment are expanded
during compile — `.text+` becomes the two-pattern set
`[".text", ".text.*"]`.  Both forms are stored, so symbol-specific
rules (which only substitute `.*` placeholders) still find
`.text.<symbol>` and `.text.<symbol>.*`.

### `gen_resolve`

Four sub-passes:

1. **Early drop.**  Symbol-specific rules whose archive or object
   isn't in `sinfo_db` are marked dropped (with a warning).  The DB
   walk that follows can't ever assign sections to them, so we
   surface the diagnostic up-front rather than at link time.

2. **Match attachment.**  For every `(archive, object, section)`
   triple in `sinfo_db`, the first matching rule (specificity DESC,
   walking the sorted rule array backwards) gets the triple appended
   to its `matches` array.  A triple matches when:
    - The archive matches `rule.archive` (or `rule.archive` is NULL).
    - The object matches `rule.object` glob-expanded against the
      suffix variants `.o`, `.*.o`, `.obj`, `.*.obj` (or
      `rule.object` is NULL).
    - The section matches one of `rule.section_patterns` — for
      symbol-specific rules, with `.*` substituted by `.<symbol>`.
   Sections matched by no rule are silently dropped; they become
   orphans at link time, either landing in the linker's default
   handling or triggering `--orphan-handling=error`.

3. **Late drop.**  Symbol-specific rules whose patterns matched no
   real section are marked dropped (with a warning).  The function
   or data the rule named was inlined or DCE'd.

4. **Cross-target exclusion.**  For every surviving rule that
   narrows a less-specific basis (per `walk_back_for_overlap`)
   routing to a different target, the rule's file pattern is
   appended to the basis's per-rule `exclude_list`.  The basis's
   glob-emitting line at emit time carries this list as
   `EXCLUDE_FILE(...)`, so cross-target placement holds regardless
   of output-section order in the template.

   Pattern shape per the table in [Why glob lines need
   `EXCLUDE_FILE`](#why-glob-lines-need-exclude_file).

### `gen_emit`

Iterates rules per target and writes the linker script body.  The
loop, with all the layering visible:

    for each target T (in template-discovery order):
        for each rule R targeting T (in source order):
            if R is dropped: continue
            emit_pre_wrappers(R)        # SURROUND start, ALIGN pre

            if R has matches:
                for each (archive, object) group in R->matches:
                    emit "*<archive>:<object>(<literal sections>)"

            else if R has an archive but no DB matches:
                emit "*<archive>[:<object>.*](
                          [EXCLUDE_FILE(<R->exclude_list>)]
                          <effective patterns>)"
                # `effective patterns` are R->section_patterns for
                # non-symbol rules, or symbol-substituted forms
                # (.text.<sym>, .text.<sym>.*) for symbol rules --
                # though after the late-drop in gen_resolve no
                # symbol rule reaches this branch.

            if R is a root rule:
                emit "*([EXCLUDE_FILE(<R->exclude_list>)]
                        <R->section_patterns>)"
                # one wildcard_spec per pattern, since GNU ld grammar
                # accepts EXCLUDE_FILE only inside SORT_BY_NAME or
                # immediately before a single name -- never wrapping
                # a SORT.

            emit_post_wrappers(R)       # ALIGN post, SURROUND end

Some properties of this loop:

- A non-symbol rule with **zero matches** still iterates through it.
  The body loop is no-op (or emits a fallback for archive-specific
  rules); the frame still emits.  This is the espcoredump case:
  SURROUND wrappers always reach the linker script, even when no
  input section in the current build configuration populates the
  surrounded area.

- A **dropped** symbol rule emits nothing.  Its archive/object isn't
  in the DB, or its symbol pattern matched nothing — gen_resolve
  already issued a diagnostic.

- An archive-specific rule with **zero matches** emits a fallback
  selector based on the rule's own `archive` and `object` fields —
  not the DB.  This handles archives the user named in `.lf` but
  ldgen does not see (prebuilt blobs not in the libraries-file).
  The fallback carries `EXCLUDE_FILE(R->exclude_list)` so
  more-specific narrowing object rules' content stays out.

- A root rule's catch-all and an archive-fallback are the **only**
  places globs appear in the output.  Their `EXCLUDE_FILE` lists are
  populated per-rule by gen_resolve so cross-target placement
  doesn't depend on output-section order in the template.

### Within-rule grouping

`gen_resolve` walks the entity DB in archive → object → section
order, so matches appended to a rule are naturally contiguous per
`(archive, object)`.  `emit_rule_matches` walks the array,
detecting boundaries by simple pointer-comparison of consecutive
`archive` and `object` fields (the strings are borrowed from
`sinfo_db`, so equal strings have identical pointers within one
archive's section list).

Each `(archive, object)` group becomes one input section
description with all of its literal sections concatenated:

    *libfoo.a:bar.c.obj(.text.foo .text.bar .literal.foo .literal.bar)

## Output format

A worked end-to-end example — for a project linking
`libcomponent.a` (one C file `widget.c.obj`) and `libfoo.a`
(one C file `bar.c.obj`), with this `.lf`:

    [sections:text]
    entries:
        .text+
        .literal+

    [scheme:default]
    entries:
        text -> flash_text

    [scheme:noflash]
    entries:
        text -> iram0_text

    [mapping:catch_all]
    archive: *
    entries:
        * (default)

    [mapping:hot]
    archive: libfoo.a
    entries:
        bar:hot_func (noflash);
            text -> iram0_text SURROUND(hot_zone)

— and a `sections.ld.in` template containing both
`mapping[flash_text]` and `mapping[iram0_text]` markers in some
order — the generated `sections.ld` body looks like:

    .iram0.text : {
        _hot_zone_start = ABSOLUTE(.);
        *libfoo.a:bar.c.obj(.text.hot_func .literal.hot_func)
        _hot_zone_end = ABSOLUTE(.);
    }

    .flash.text : {
        *libcomponent.a:widget.c.obj(.text .text.helper .text.init
                                     .literal .literal.helper .literal.init)
        *libfoo.a:bar.c.obj(.text .text.cold_func .text.another_func
                            .literal .literal.cold_func .literal.another_func)
        *(EXCLUDE_FILE(*libfoo.a) .text  EXCLUDE_FILE(*libfoo.a) .text.*
          EXCLUDE_FILE(*libfoo.a) .literal  EXCLUDE_FILE(*libfoo.a) .literal.*)
    }

Notes on the catch-all line:

- Only `*libfoo.a` appears in `EXCLUDE_FILE`, not `*libcomponent.a`.
  `libfoo.a` has a more-specific symbol rule routing some sections
  to a different target, so the catch-all must exclude it to avoid
  template-order ambiguity.  `libcomponent.a` has no more-specific
  narrowing rule, so its sections need no exclusion — they're either
  named in the literal listing above or aren't claimed by any rule.
- `EXCLUDE_FILE(...)` repeats per pattern.  GNU ld accepts
  `EXCLUDE_FILE` either as a wildcard_spec on its own
  (`EXCLUDE_FILE(...) .text`) or wrapped in `SORT_BY_NAME` — but
  *not* the other way around.  Repeating per pattern keeps the same
  emit path correct for both SORT and non-SORT rules without
  relying on the propagating form documented inconsistently across
  binutils versions.

Trace through each input the linker sees:

| Input | Outcome |
|-------|---------|
| `libfoo.a:bar.c.obj:.text.hot_func` | `.iram0.text` literal line names it. → IRAM. |
| `libfoo.a:bar.c.obj:.literal.hot_func` | `.iram0.text` literal line names it. → IRAM. |
| `libfoo.a:bar.c.obj:.text.cold_func` | `.flash.text` literal line names it. → flash. |
| `libcomponent.a:widget.c.obj:.text.init` | `.flash.text` literal line names it. → flash. |
| `libgcc.a:_udivsi3.o:.text` | No literal line names libgcc.a; not in EXCLUDE_FILE; catch-all matches. → flash. |
| `libwifi_phy.a:phy.o:.iram_phy_text` | No literal line; no rule's patterns match `.iram_phy_text`; orphan-handling fires with the section name. |

Order of `.flash.text` and `.iram0.text` in the script is irrelevant
— each input has exactly one matching input section description.

### `EXCLUDE_FILE` and `SORT_BY_NAME`

GNU ld's grammar accepts `EXCLUDE_FILE` either as a wildcard_spec by
itself (`EXCLUDE_FILE(<files>) <name>`) or wrapped inside
`SORT_BY_NAME` (`SORT_BY_NAME(EXCLUDE_FILE(<files>) <name>)`) — but
**not** the other way around.  `EXCLUDE_FILE(...) SORT_BY_NAME(...)`
is a syntax error.

To keep the catch-all consistent across SORT and non-SORT rules,
ldgen always emits one wildcard_spec per pattern:

    /* without SORT */
    *(EXCLUDE_FILE(<list>) .text  EXCLUDE_FILE(<list>) .text.*)

    /* with SORT */
    *(SORT_BY_NAME(EXCLUDE_FILE(<list>) .text)
      SORT_BY_NAME(EXCLUDE_FILE(<list>) .text.*))

Per-pattern repetition is verbose but unambiguous and works
identically across binutils versions.

## What ldgen does **not** know about

- The set of archives the linker actually loads.  `--libraries-file`
  is the contract: ldgen places sections from those archives
  explicitly; everything else falls into the catch-all or becomes an
  orphan.
- Toolchain libraries (`libgcc.a`, `libc.a`, etc.).  These are
  unknown by default and caught by the catch-all.  If the user wants
  to write a `.lf` rule referencing one (e.g. `archive: libgcc.a (in_iram)`),
  the rule emits its own selector via the archive-fallback path —
  the libraries-file does not need to be extended.
- The order of output sections in `sections.ld.in`.  Order does not
  affect correctness; templates can be reorganised freely.
- Linker-script-only constructs (`PROVIDE`, `MEMORY`, `PHDRS`).
  These come from `sections.ld.in` directly; ldgen only fills the
  `mapping[<target>]` markers.

## CLI

    ice idf ldgen --libraries-file <path> \
                  --config <sdkconfig> \
                  --kconfig <Kconfig> \
                  --input <sections.ld.in> \
                  --output <sections.ld> \
                  <fragment.lf> [<fragment.lf> ...]

Modes:

- **Analyse mode** (no `--info` / `--libraries-file` / `--input` /
  `--canonical`): just parse and report counts.  `--dump` prints the
  parsed AST.
- **Resolve mode** (any of the above): full pipeline.  With
  `--input`, fills the template; without, emits per-target blocks to
  stdout (or `--output`).  With `--canonical`, dumps
  `archive|object|section|target` lines for diffing against the
  Python implementation's resolved output.

The Python-ldgen flags `--env-file` and `--objdump` are accepted for
build-system compatibility and ignored — env vars are not consulted,
and AR + ELF parsing is in-process via `ar.c` and `elf.c`.

---

# `.lf` file format

The grammar below is extracted from the pyparsing definitions in
upstream IDF's `tools/ldgen/ldgen/fragments.py` (`class Fragment`,
`class Sections`, `class Scheme`, `class Mapping`,
`parse_fragment_file()`).

## Lexical elements

Four distinct name classes are used in different positions:

    IDENT    = [a-zA-Z_] [a-zA-Z0-9_]*             Fragment.IDENTIFIER
    ENTITY   = [a-zA-Z0-9.\-_$+]+                  Fragment.ENTITY
    SEC_NAME = [a-zA-Z_.] [a-zA-Z0-9._-]* '+'?     Sections.ENTRY (Combine)
    OBJ_NAME = [a-zA-Z_] [a-zA-Z0-9\-_]*           Mapping._obj

Other terminals:

    NUM      = [0-9]+
    EXPR     = [^:\n]+                              SkipTo(':') in get_conditional_stmt()
    SORT_KEY = 'name' | 'alignment' | 'init_priority'

Our lexer unifies the four name classes into a single NAME token
covering their union.  The parser relies on context to distinguish them.

### Indentation

pyparsing's `IndentedBlock(_stmt)` groups statements whose leading
whitespace is >= the first statement's indent.  Our lexer emits
INDENT / DEDENT tokens with the same semantics.

Comments (`#` to end of line) and blank lines are transparent to
indentation — they neither set nor break a block level.

## Productions

### File

    file        = { fragment }
    fragment    = sections | scheme | mapping | cond(fragment)

### Sections

    sections    = '[sections:' IDENT ']' NL
                  'entries:' suite(sec_stmt)

    sec_stmt    = SEC_NAME NL
                | cond(sec_stmt)

### Scheme

    scheme      = '[scheme:' IDENT ']' NL
                  'entries:' suite(sch_stmt)

    sch_stmt    = IDENT '->' IDENT NL
                | cond(sch_stmt)

### Mapping

    mapping     = '[mapping:' IDENT ']'
                  'archive:' suite(archive_stmt)
                  'entries:' suite(map_stmt)

    archive_stmt = (ENTITY | '*') NL
                 | cond(archive_stmt)

    map_stmt    = map_entry NL
                | map_entry ';' flag_list
                | cond(map_stmt)

    map_entry   = (OBJ_NAME [ ':' IDENT ] | '*') '(' IDENT ')'

Note: `Mapping.parse_archive()` enforces that the archive suite
resolves to exactly one value after conditional evaluation.

### Flags

    flag_list   = flag_item { ',' flag_item }       DelimitedList in Mapping.ENTRY_WITH_FLAG
    flag_item   = IDENT '->' IDENT flag { flag }    Flag.FLAG (OneOrMore)
    flag        = 'KEEP()'                          Keep.KEEP (single Keyword)
                | 'ALIGN' '(' NUM [',' 'pre'] [',' 'post'] ')'
                                                    Align.ALIGN (order matters: pre before post)
                | 'SORT' '(' [ SORT_KEY [',' SORT_KEY] ] ')'
                                                    Sort.SORT
                | 'SURROUND' '(' IDENT ')'          Surround.SURROUND

### Conditionals

    cond(S)     = 'if' EXPR ':' suite({ S })
                  { 'elif' EXPR ':' suite({ S }) }
                  [ 'else:' suite({ S }) ]

`else:` is a single literal (`Literal('else:')` in `get_conditional_stmt()`),
not two tokens.

### Suite

    suite(S)    = IndentedBlock(S | comment | cond(S))

A suite is an indented block built by `get_suite()`.  The first content
line sets the indent level; subsequent lines must be at the same or
deeper level.  The block ends when a line appears at a shallower level.

## LL(1) decision points

| After seeing         | Lookahead     | Choose                |
|----------------------|---------------|-----------------------|
| `entries:` NL        | INDENT        | indented block        |
| `entries:` NL        | NAME / `*`    | same-level block      |
| `archive:` ...       | NAME / `*`    | inline value          |
| `archive:` ...       | NL            | indented block        |
| NAME in map_entry    | `(`           | object (scheme)       |
| NAME in map_entry    | `:`           | object:symbol (scheme)|
| `)` in map_entry     | `;`           | entry with flags      |
| `)` in map_entry     | NL            | plain entry           |
| NAME in stmts        | NAME is `if`  | conditional           |
| NAME after DEDENT    | NAME is `elif`| continue conditional  |
| NAME after DEDENT    | NAME is `else`| else branch           |
| NAME after DEDENT    | other         | conditional done      |
