# AsyncTCP — vendored

| | |
|---|---|
| **Upstream** | https://github.com/ESP32Async/AsyncTCP |
| **Version** | 3.5.0 |
| **Commit** | `fd296a703104463c841d8711de3d5f63ea8ffb14` (2026-07-24) |
| **Vendored** | 2026-07-26 |
| **Licence** | LGPL-3.0 (see `LICENSE`) |
| **Companion** | `lib/espasyncwebserver/` — ESP32Async/ESPAsyncWebServer 3.12.0, also patched |

Only `src/`, `library.json`, `LICENSE` and `README.md` are vendored.

## THIS COPY IS PATCHED

It was vendored unpatched on 2026-07-26 and gained its first local patch on
2026-07-31. Every patched hunk is marked in-source with `LOCAL PATCH
(SlopDrive)` — grep for that string to find them all. Do not add unmarked local
changes.

### The patch: `~AsyncClient()` must purge the event queue unconditionally

`AsyncTCP.cpp`.

Upstream:

```cpp
AsyncClient::~AsyncClient() {
  if (_pcb) {
    _close();
  }
}
```

lwIP's TCP callbacks run on the tcpip thread and only ENQUEUE
`lwip_tcp_event_packet_t` records; the `async_tcp` task dequeues and dispatches
them later. Each record holds a bare `AsyncClient *`, so every teardown path has
to purge the queue of records naming the client that is going away. Upstream
does exactly that in `_reset_tcp_callbacks()`, in both else-branches of
`_tcp_close_api` / `_tcp_abort_api`, and in `tcp_error()`.

The destructor is the one path that does not. It purges only as a side effect of
`_close()`, which it skips entirely when `_pcb` is already null — and null `_pcb`
is the state `tcp_error()` deliberately leaves behind (it nulls `_pcb` before
enqueuing `LWIP_TCP_ERROR`). A client torn down that way is freed with its
RECV/SENT/POLL records still queued, and `_async_service_task` then dispatches
them into freed memory.

The patch moves the purge out of the conditional. It is idempotent — purging an
already-purged queue walks a list and removes nothing.

**This was a live crash, not a theoretical one.** Reproduced on demand with 12
concurrent WebSocket sessions (`isolate.py many 12`), fw 2.3.5, comprehensive
heap poisoning on:

    tcp_recved            (lwip/core/tcp.c:985)   EXCCAUSE 28, vaddr 0xfefeff3a
     <- _tcp_recved_api    (AsyncTCP.cpp:625)
     <- tcpip_api_call
     <- _tcp_recved        (AsyncTCP.cpp:637)
     <- AsyncClient::_recv (AsyncTCP.cpp:1098)
     <- handle_async_event (AsyncTCP.cpp:304)
     <- _async_service_task(AsyncTCP.cpp:340)

`0xfefefefe` is ESP-IDF's comprehensive-poisoning free-fill pattern and
`0xfefeff3a` is that value plus `0x3C`: `_recv` read `_pcb` out of a freed,
poisoned `AsyncClient` and handed the poison to lwIP as a `tcp_pcb *`. Mechanism
and the wrong turns taken before finding it: TRAPS T29.

### Upstreaming

Intended to go upstream as a PR — it closes a use-after-free in upstream's own
lifetime invariant, and the fix is one statement. Until it is merged and
released, keep the `LOCAL PATCH (SlopDrive)` marker and re-apply on every
version bump.

## Update procedure

1. Fetch the new upstream `src/`, `library.json`, `LICENSE`, `README.md`.
2. Overwrite this directory's copies.
3. Re-apply every hunk marked `LOCAL PATCH (SlopDrive)` above.
4. Update the version/commit table at the top.
5. Rebuild `env:sd32` and re-run the 12-session reproduction — a re-vendor that
   silently drops the patch looks fine until the device reboots under load.
