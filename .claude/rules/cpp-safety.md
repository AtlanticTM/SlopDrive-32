---
paths:
  - "src/**"
  - "include/**"
  - "lib/slopmotion/**"
  - "lib/sloplog/**"
  - "lib/slopglow/**"
---

# Memory safety and lifetime (operator directive 2026-07-31 -- binding)

Where this file and older doctrine disagree on a memory-safety question, this
file wins.

**Scope, and it is deliberate.** These rules bind `src/`, `include/`, and the
`lib/slop*` ecosystem libraries. They do NOT bind `lib/espasyncwebserver`,
`lib/asynctcp`, `lib/ruckig`, the Arduino core, ESP-IDF, or anything under
`lib_deps`. That code is third-party, is re-vendored from upstream, and is
full of constructs the safe subset rejects. Enforcing there would mean
choosing between a permanently red build and patching code we do not own.
Vendored include paths are `-isystem` so their warnings cannot block ours.

## The safe subset

Preferred in the scope above, applied opportunistically whenever a file is
touched for any reason:

| Instead of | Use |
|---|---|
| C arrays | `std::array<T, N>` |
| pointer plus length parameters | `std::span<T>` |
| raw `new`/`delete`/`malloc`/`free` | fixed-capacity storage; `std::unique_ptr` only when dynamic lifetime is genuinely unavoidable |
| heap allocation after startup | static / fixed-capacity |
| `std::vector` / `std::string` / Arduino `String` in steady-state code | fixed-capacity containers |
| `strcpy`/`strcat`/`sprintf` | `snprintf` at minimum |
| null as "no value" | `std::optional<T>` |
| `union` | `std::variant<A, B>` |
| bool/error-code returns for fallible operations | `std::expected<T, E>` |
| C-style casts | `static_cast`; `reinterpret_cast` only in the hardware layer |
| manual acquire/release | RAII, cleanup in destructors |

## The rules no tool enforces -- these are the ones that actually bite

Treat a violation as severity-critical in review.

- Never return a reference, `span`, or `string_view` to a local.
- Never store a `span` or `string_view` as a class member. Parameters only;
  members own their data.
- **Lambdas handed to tasks, timers or callbacks capture BY VALUE.** Never
  `[&]` for anything that outlives the enclosing scope.
- Never mutate a container while iterating it.
- Rule of zero: most classes declare no destructor, copy, or move at all.

## Concurrency

New mutable state is owned by exactly one task and reached by message, not by
shared memory. This does NOT mandate retrofitting what already exists: the
SlopSync WS transport's lock-free SPSC ring and its deferred attach/detach are
the fix for field bug #5 (T5), are reasoned about in-source, and stay. The
rule binds new code. Introducing shared mutable state behind a mutex needs an
operator ruling first.

## What the toolchain enforces mechanically

- `build_src_flags` in `platformio.ini` carries the warning set. It is
  `build_src_flags`, not `build_flags`, precisely so it stops at `src/`.
- **Ten warnings are hard errors and the list is a floor, never weakened:**
  `dangling-reference`, `dangling-pointer`, `use-after-free`,
  `free-nonheap-object`, `return-type`, `uninitialized`, `array-bounds`,
  `stringop-overflow`, `nonnull`, `sizeof-pointer-memaccess`. Our code is
  clean on all ten; keep it that way rather than widening the exemption.
- Blanket `-Werror` is the goal, NOT yet reachable: first enable produced ~509
  warnings, largely `-Wshadow` and `-Wconversion`, and a large share come from
  third-party headers pulled in via `lib_deps` as `-I` (PlatformIO gives no
  `-isystem` hook for those). Burn the backlog down per file, then widen.
- `monitor_filters = esp32_exception_decoder` so panic backtraces decode to
  file:line.

## Static analysis -- configured, NOT yet operational

`.clang-tidy` exists at repo root with the directive's `WarningsAsErrors`
floor intact and a `HeaderFilterRegex` scoped to our code. It is not running,
and neither route works on this host as-is:

- `pio check` dies with `WinError 206: filename or extension is too long`. It
  inlines every ESP-IDF include path into one clang-tidy command and blows the
  Windows 32 KB command-line limit.
- Invoking `clang-tidy -p .` against the root `compile_commands.json` reaches
  the compiler but hits 5 hard errors from GCC-only flags clang rejects
  (`-mlongcalls`, `-fno-tree-switch-conversion`,
  `-fstrict-volatile-bitfields`, `-mdisable-hardware-atomics`) plus a missing
  `stddef.h`, and reports ~12,700 findings because the header filter is not
  biting.

The fix shape is a sanitized `compile_commands.json`: strip GCC-only flags,
add clang's builtin include path, the same treatment `.clangd` already applies
via its `Remove:` list. **Do not claim clang-tidy coverage until that lands.**

**A per-edit clang-tidy hook is REFUSED as specified.** On this host a
single-file check has not completed inside ten minutes. It belongs at a
pre-commit or on-demand gate; the fast feedback loop is the compiler, where
the ten fatal warnings above already live.

## Lifetime traps that have bitten this codebase

### T1 -- object reset via `*this = T{}` is a stack bomb

**Rule:** reset large objects with in-place destroy plus placement-new
(`obj.~T(); new (&obj) T();`), never `*this = T{}` or `obj = T{}`.
**Mechanism:** the right-hand `T{}` is a full temporary constructed ON THE
CURRENT STACK before assignment. A multi-KB object on an 8 KB FreeRTOS task
stack overflows it instantly. Host-side tests never catch this class because
desktop threads get megabyte stacks. Enforced by canon_lint `this-assign`.
Bit us: a ~9 KB session temporary panicking the hub task on every client
connect (July 2026).

### T4 -- no function-local statics inside critical sections

**Rule:** never declare a function-local `static` of class type inside
`portENTER_CRITICAL` or any no-abort context; hoist to file scope.
**Mechanism:** first execution of a function-local static registers its
destructor via `__cxa_atexit` and takes an init-guard lock. Both can allocate
or abort, and inside a critical section that aborts the core. A path that has
never run live (ours: token minting) hides this until the first real use.

### T9 -- C++ default arguments bind to the STATIC type

**Rule:** forwarding proxies and wrappers pass explicit sentinels through to
the base; they never restate the base's default arguments.
**Mechanism:** default args are substituted at the CALL SITE from the declared
static type of the expression, not the dynamic type, so a proxy that
redeclares a default silently overrides a subclass's different default.

### T29 -- a callback that can destroy its own caller's object

**Rule:** before a loop calls user code, ask whether that call can free the
object the loop is iterating on. If it can, the liveness check must live
OUTSIDE the object: you cannot ask a freed object whether it is freed. This
applies to every dispatch loop in an async/callback library, ours or vendored.

**Mechanism:** AsyncTCP's teardown is SYNCHRONOUS IN THE CALLER'S CONTEXT.
`AsyncClient::close()` reaches `_close()`, `_tcp_close()`, and then, still on
the caller's stack, `_discard_cb(...)`. ESPAsyncWebServer's `_discard_cb`
deletes both the `AsyncWebSocketClient` and the `AsyncClient` before control
returns. `AsyncClient::_recv()` iterates a pbuf CHAIN, invokes `_recv_cb` once
per buffer, then touches `this` again (`_ack_pcb`, `_pcb`, `_tcp_recved`) with
no liveness check between iterations.

**Read the address, it names the bug.** `0xfefefefe` is ESP-IDF's
comprehensive-poisoning free-fill pattern. A fault at `0xfefefefe + small` is
a pointer READ OUT OF FREED MEMORY and then member-accessed; the offset is the
member. Bit us twice in one reproduction (2026-07-31): `0xfefeff3a` (poison
plus `0x3C`) and `0xfefeff4e` (poison plus `0x50`), both `EXCCAUSE 28` on task
`async_tcp`. This is the fastest read in the whole hunt and it needs no
debugger, but it only works while poisoning is on.

**Fix:** two LOCAL PATCHes in `lib/asynctcp/` (see its `VENDORED.md`) --
`~AsyncClient()` purges the async event queue unconditionally, and a
file-scope `_dispatching_client` that `~AsyncClient` clears and `_recv`
re-checks after every callback. Out-of-band ON PURPOSE: a flag stored in the
object would itself be freed memory. A third patch in `lib/espasyncwebserver/`
(`queueLen(uint32_t id)`) closes the mirror image on our side, because
`AsyncWebSocket::client(id)` releases `_ws_clients_lock` before returning its
pointer. Never hold an `AsyncWebSocketClient*`.

**NOT THE WHOLE STORY.** These three took the reproduction from
reboot-every-run to roughly one-in-four, and the surviving failure reverted to
the original `remove_free_block` from `wifi_malloc` signature. A rate change
is evidence a real bug was removed; it is NOT evidence the last one was.
