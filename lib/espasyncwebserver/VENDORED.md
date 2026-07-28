# ESPAsyncWebServer — vendored

| | |
|---|---|
| **Upstream** | https://github.com/ESP32Async/ESPAsyncWebServer |
| **Version** | 3.12.0 |
| **Commit** | `a008cccf07aad47e35f02c21393e3dc41688981b` (2026-07-26) |
| **Vendored** | 2026-07-26 |
| **Licence** | LGPL-3.0 (see `LICENSE`) |
| **Companion** | `lib/asynctcp/` — ESP32Async/AsyncTCP 3.5.0, `fd296a703104463c841d8711de3d5f63ea8ffb14` (2026-07-24), vendored unpatched |

Only `src/`, `library.json`, `LICENSE` and `README.md` are vendored. Examples,
tests and CI are deliberately not, to keep the tree small.

## Why this is here at all

It replaces **links2004/arduinoWebSockets** as the transport under
`slopsync::ITransport`. That library's send path is a *synchronous busy-wait on
the caller's task* — a backed-up client socket blocks the sender until the peer
drains or `WEBSOCKETS_TCP_TIMEOUT` expires. The project had already capped that
timeout from 5000 ms to 1200 ms and built a whole layer of scaffolding around it
(activity gate, send-stall mute, `kStallEvictMs` sweep, zombie reaper) — all of
which exists solely to survive a transport that can stall a task. The operator
reported WS dropouts severe enough that the WebUI showed "unreachable".

`AsyncWebSocket` gives bounded per-client queues that **drop or close, never
block**, which is SPEC §9/§10.4 backpressure implemented by the transport instead
of compensated for by the application.

## THIS COPY IS PATCHED — unlike `lib/ruckig/`

`lib/ruckig/` is byte-identical to upstream and must never be patched locally.
**This one is different**: it carries a deliberate local patch, so the two
vendoring policies are NOT the same and should not be assumed so.

Every patched hunk is marked in-source with `LOCAL PATCH (SlopDrive)` — grep for
that string to find them all. Do not add unmarked local changes.

### The patch: RFC 6455 §4.2.2 subprotocol selection

`AsyncWebSocket.cpp` / `AsyncWebSocket.h`.

Upstream's handshake was:

```cpp
if (request->hasHeader(WS_STR_PROTOCOL)) {
  const AsyncWebHeader *protocol = request->getHeader(WS_STR_PROTOCOL);
  // ToDo: check protocol
  response->addHeader(WS_STR_PROTOCOL, protocol->value());
}
```

It echoed the client's **entire** `Sec-WebSocket-Protocol` header back verbatim.
That is wrong two ways:

1. **Malformed response.** RFC 6455 §4.2.2 requires the server to select
   *exactly one* subprotocol. A comma-separated list in the response is illegal.
2. **The server claims to speak anything it is asked for.** A client offering a
   future protocol revision gets told "yes" by a server that has never heard of
   it. The mismatch then surfaces later as garbled frames rather than as a clean
   refusal at handshake time. For SlopSync — whose whole value proposition is
   that a client can trust what the hub tells it — that is a conformance defect,
   not a nicety.

The patch adds:

* `AsyncWebSocket::setSubprotocol(...)` / `subprotocol()` — the one subprotocol
  this server will agree to. SlopSync sets `"slopsync.v1"`.
* `AsyncWebSocket::_selectSubprotocol(offered, accepted)` — tokenises the
  client's comma-separated list, trims whitespace, and returns **exactly one**
  token or an empty string.
* Handshake: emit the selected token, or **omit the header entirely** when
  nothing acceptable was offered. Per RFC 6455 the client MUST then fail the
  connection — which is precisely the refusal we want.

**Backwards compatibility was preserved deliberately:** with no accepted
subprotocol configured, the helper returns the client's *first* offered token
rather than the whole list. That keeps upstream's permissive behaviour while
still emitting a legal single-value response, so any other consumer of this
library does not suddenly start failing handshakes because of this patch. That
property is what makes it upstreamable.

### Upstreaming

This patch is intended to go upstream as a PR (it fixes a real spec violation
and a literal `// ToDo` in upstream's own code). Until it is merged and released:

* keep the `LOCAL PATCH (SlopDrive)` markers,
* re-apply on every version bump — see below.

## Update procedure

1. `git clone --depth 1 https://github.com/ESP32Async/ESPAsyncWebServer` at the
   new tag; do the same for `AsyncTCP` if its pin moves.
2. Diff the new upstream `AsyncWebSocket.{h,cpp}` against **upstream at the
   commit recorded above** (not against this copy) so the local patch is not
   mistaken for an upstream change.
3. Copy `src/`, `library.json`, `LICENSE`, `README.md` over this directory.
4. Re-apply the patch — or, if the PR landed upstream, DELETE the patch and this
   section, and set the pin to the release that contains it.
5. Update the table at the top of this file.
6. Rebuild and re-run `tools/slopsoak.py` (SlopSync repo). A transport change
   is not verified by compiling; it is verified by soaking. See
   `docs/ws-transport-baseline.md` for the links2004 numbers this replacement
   has to beat.

## Notes

* **Line endings are CRLF**, as they arrive from the clone on Windows. Left as-is
  so the vendored files stay directly diffable against upstream. Patch scripts
  must be CRLF-aware.
* `AsyncWebSocket`'s cross-task safety was verified in source before adopting it:
  `_queue_lock` guards each client's `_messageQueue`, `_ws_clients_lock` guards
  the client list, and the public send methods take both in a consistent nesting.
  Sending from a task other than the AsyncTCP callback task is therefore
  supported — which is exactly what the SlopSync hub task does.
