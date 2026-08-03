---
paths:
  - "src/**"
  - "include/**"
  - "lib/slopmotion/**"
  - "lib/sloplog/**"
  - "lib/slopglow/**"
  - "test/native/**"
  - "examples/**"
---

# C++ navigation: grep to find, LSP to understand

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
