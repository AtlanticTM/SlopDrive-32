---
paths:
  - "**"
---

# C++ style and navigation

## Coding style and extensibility

- Strict OOP; `.h`/`.cpp` isolation; lifecycle hooks (`init()`, `update()`,
  `emergencyStop()`) on functional modules.
- `float` over `double` (S3 hardware FPU). Double math only at plan-time
  events, never per-sample.

### Comment style law (operator-stamped 2026-07-28)

Scope: `src/`, `include/`, `lib/`, `sim/`, `test/` C++. This is the mechanized
form of CANON C-12.

- **FILE HEADER.** Every `.h`/`.hpp`/`.cpp` opens with
  `// <Name> -- <one-line job>`, then `// Constraints:` lines holding only
  load-bearing rules (threading, ownership, units, never-do's), then `// See:`
  pointers if real ones exist. No history, no authorship, no dates, no feature
  lists.
- **SECTION BANNERS.** `// ---- <section name> ----` dash-padded to column 80
  (the tree's dominant width; amended from 76 on 2026-07-28 to normalize to
  reality instead of repadding against it). Name only: no hex ids, no
  numbering, no box art. RFC-nnn and T-nn references in a banner name are
  POINTERS, which is allowed, not numbering. Files over ~150 lines are divided
  into their natural sections this way.
- **STYLE.** `//` everywhere; `/* */` only in license headers. Multi-line is
  consecutive `//` lines indented with the code they bind to.
- **CONTENT.** A comment is exactly one of: a CONSTRAINT, an INVARIANT the
  code cannot show, a POINTER (rules file, SPEC, docs), or
  `// TODO(<board id or RFC-nnn>): <change>`. A TODO without a home reference
  is a finding. Narration, history, restated code, and diff justification are
  deleted; a story worth keeping moves to a rules file with a pointer left
  behind.
- **VOICE.** American English, fragments fine, no first person, no emoji.

### Minimalism-mode precedence (operator ruling 2026-07-29)

The ponytail agent mode governs `webui/`, `docs/`, and host `tools/`. It does
NOT govern `src/`, `include/`, `lib/`, or the SlopSync repo; there the rules
files outrank it. Diff size is not a correctness argument.

- Memory and concurrency reasoning is never the thing that gets shortened.
  Stack cost, heap/BSS/PSRAM placement, and which task a callback runs on are
  stated before a change is called done.
- Single-implementation indirection that doctrine mandates -- the SlopLog and
  SlopGlow sole paths, the MotionArbiter sole-caller rule -- is law, not
  speculative abstraction to delete.
- A SPEC-defined SlopSync field is not dead weight because one implementation
  currently ignores it. The spec decides; changes ride the RFC ritual.
- Every field bug on record was the small obvious change. The cost landed on
  stack depth, allocation lifetime, and task context, none of which a
  diff-size heuristic can see.

## Navigation: grep to find, LSP to understand

This is a workflow rule, not a preference. Follow it on every C++ task.

- Use Grep/Glob for DISCOVERY: finding files, string and pattern search,
  "where is this text mentioned".
- Use Serena/LSP symbolic tools for UNDERSTANDING: definitions, references,
  types. That means `find_symbol`, `find_referencing_symbols`,
  `find_declaration`, `get_symbols_overview`, and hover.
- After locating a file, navigate WITHIN it via symbols, never by reading the
  whole file. `get_symbols_overview` first, then `find_symbol` with a
  `name_path` and only the depth you need.

Reading a 2,600-line translation unit to answer "what does this class expose"
is the thing this rule exists to stop.

## Why it is a rule and not a suggestion

Grep answers "does this text appear". It cannot tell a declaration from a
definition from a comment mentioning the name, and it cannot follow a call
into the sibling SlopSync library. A rename driven by grep hits the string in
a doc comment and misses the one call site that spells the type through an
alias.

The compile-database trap makes this sharper here. When clangd loses its
flags, symbol search silently returns nothing while grep keeps returning
plausible text, so the wrong tool looks like the working one. Treat an empty
symbol result as a toolchain failure to diagnose, never as proof the symbol
does not exist. `.claude/rules/serena.md` has the check and the repair.

## Local constraints

- `.clangd` sets `Index: Background: Skip`, so there is no project-wide
  index. Scope symbolic queries with `relative_path`; an unscoped
  `find_symbol` walks files and can terminate the language server.
- Cross-file references therefore resolve reliably only within files the
  server has opened. Open the file you care about first.
- The vendored SlopSync surface under `lib/slopsync` is read-only from this
  repo. Navigate into it freely; changes there are RFCs, not edits.
