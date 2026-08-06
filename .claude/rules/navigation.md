---
paths:
  - "**"
---

# Code navigation: discovery, then ground truth

Two tools, two jobs. Using the discovery tool for a C-9 deletion proof is how
live code gets deleted with a clean conscience.

## Discovery: codebase-memory (start here)

Finding your way in: what exists, where a subsystem lives, which functions are
hairy. `search_graph(query="...")` takes NATURAL LANGUAGE and is the only tool
here that does, so it is the right first move when you do not yet know the
symbol name. Also `get_architecture`, plus complexity and degree metadata.
Its line numbers are 1-BASED.

## Ground truth: serena (before you touch anything)

Its tools are DEFERRED, so load the schema once per session:

```
ToolSearch("select:mcp__serena__find_symbol,mcp__serena__find_referencing_symbols")
```

Use serena, not the graph, for the definition of a symbol, every caller of it,
and anything a decision rests on. Its line numbers are 0-BASED: add 1 before
citing one as `file:line`.

## The boundary is measured, not a style preference

The graph resolves call edges by NAME, not by type. Tested 2026-08-04 on
`ServoModbus::emergencyStop`. One probe, wrong in both directions:

- MISSED `_bus.emergencyStop()` at `ModbusServoDriver.cpp:148`, though
  `ServoModbus& _bus` is declared at `ModbusServoDriver.h:165`.
- INVENTED an edge from `MotionArbiter::emergencyStop` to slopsim's
  `MotionDigest::operator==`, built from `xQueueReceive(...) == pdTRUE`,
  crossing into a target that compiles into no firmware env at all.

It is strong where names are unique and weak where they collide, which is the
opposite of when you need the help. Serena asks clangd, which is the
compiler's own answer.

## C-9 proof-of-no-callers

Serena AND Grep, reconciled. The graph counts as neither. Grep cannot see
through an `#include` or tell a declaration from a call; serena is limited by
its index. They fail in opposite directions, which is why both are required.

An EMPTY serena reference result is INDEX SUSPECT, never "no callers". Treat
it that way until some symbol you know is called comes back non-empty. Two
causes: a cold index for about a minute after session start, or
`Index.Background` in `.clangd` not set to `Build`. Verified 2026-08-04 under
`Skip`, `ServoModbus::emergencyStop` reported zero references while
`ModbusServoDriver.cpp:148` calls it.

## Precedence over the installed reminder

codebase-memory installs SessionStart and SubagentStart hooks saying to use its
tools FIRST for ANY code exploration. In this repo that holds for DISCOVERY
only. It does not override the C-9 rule above, and this file wins. The hooks
are vendor-installed at user scope and get rewritten on upgrade, so the
override lives here rather than as an edit to them.
