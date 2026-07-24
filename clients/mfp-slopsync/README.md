# SlopSync — MultiFunPlayer plugin

Streams a [MultiFunPlayer](https://github.com/Yoooi0/MultiFunPlayer) (MFP) axis to a
**SlopDrive-32** machine over its native **SlopSync** protocol — the device-shadow +
capability-negotiation sync protocol the firmware speaks on a binary WebSocket.

This is the first external client implementation of SlopSync. Its wire bytes are a
faithful mirror of the live-verified reference client `tools/slopsync_probe.py`.

Instead of TCode-over-serial/UDP, the plugin reads the L0 axis position at a fixed
rate and pushes it straight onto the machine's `motion-input` stream channel
(`0x0084`), where the firmware's SlopMotion planner consumes it — target position
*and* a handoff velocity, so the machine can plan smooth motion between samples.

---

## Install

MFP compiles plugins itself (Roslyn, at runtime) — you do **not** build anything to
install this.

1. Create a folder `SlopSync` under your MFP `Plugins` directory:
   `…\MultiFunPlayer\Plugins\SlopSync\`
2. Copy **`SlopSync.cs`** and **`SlopSync.xaml`** into it.
3. Start MFP. The plugin appears in the plugin list; open its tab.

Only those two files ship. `SlopSync.csproj`, `WireSelfTest.cs`/`.csproj`, and
`LiveWireTest.cs`/`.csproj` are dev-only (compile-check, golden-byte self-test, and
live device wire test) and must **not** be copied into the Plugins folder.

---

## Usage

1. In the plugin tab, set **Address** (the machine's IP, default `192.168.1.229`) and
   **Port** (`82`). Or hit **Discover** to find machines on the LAN via mDNS and pick
   one from the list — it fills Address/Port for you. Manual entry always works;
   discovery is a convenience.
2. Confirm **Axis** (default `L0`) and **Rate** (default `50` Hz).
3. Press the **▶ toolbar button** to connect. It streams until you press it again
   (which shows as ■). The LIVE panel shows what it's doing: session id, granted rate,
   clock offset/RTT, bundles sent, NACK counts, the last target value, and uptime.

The machine **must be homed** before it will actually move. If it is not, the firmware
accepts and counts your samples but drops them at the safety gate (correct behavior) —
you will see bundles climbing but no motion. Home it from the machine's own WebUI/app.

### Settings (persisted by MFP)

| Setting | Default | Meaning |
|---|---|---|
| **Address** | `192.168.1.229` | Machine IP / hostname. |
| **Port** | `82` | SlopSync WebSocket port. |
| **Rate (Hz)** | `50` | How often the axis is sampled and streamed. Clamped 10–250; the machine may grant a *lower* rate (its channel cap is 333 Hz) and the plugin streams at the **granted** rate, not the wish. |
| **Axis** | `L0` | Which MFP `DeviceAxis` to stream (`L0`, `L1`, `R0`, …). |
| **PIN** | *(empty)* | Pairing PIN. Sent in HELLO's token field. The firmware currently accepts any client (LAN-trust), so this is forward-compatibility plumbing — leave it empty unless the device asks for it. |

---

## What it does on the wire (protocol summary)

All of this mirrors `tools/slopsync_probe.py` and `docs/slopsync/SPEC.md`.

1. **Connect** — WebSocket to `ws://addr:port/`, subprotocol `slopsync.v1`, binary frames.
2. **HELLO → WELCOME** — identifies (stable 8-byte instance id) and wishes to *publish*
   on channel `0x0084` at the configured rate. The hub replies WELCOME with a session id
   and a `granted_publishes` entry (CBOR key 36) confirming the applied publish rate. No
   grant ⇒ the plugin surfaces an error and retries.
3. **SUBSCRIBE** — to `safety` (`0x0003`) and `motion` (`0x0080`) STATE, so the panel can
   count live device state (proof the session is bidirectional).
4. **CLOCK sync** (§7.1) — a few `0x05` exchanges; keeps the best-RTT offset. All STREAM
   timestamps are **hub time**, so the plugin converts local µs → hub µs using this offset,
   and re-syncs every ~10 s (drift between syncs is taken from a monotonic `Stopwatch`).
5. **STREAM loop** — at the granted rate: reads the axis (0..1), derives velocity
   `(x − x_prev)/dt` with a light EMA, and sends a single-sample bundle on `0x0084`. The
   4-byte sample is `{target_norm: u16 = clamp01(pos)×10000, vel_norm: i16 = clamp(vel,±32.767)×1000}`,
   little-endian, stamped with the current hub time. It keeps streaming even when the value
   is static — a constant target is a valid *hold*, and the device deadman (§11.3) handles a
   truly vanished source.
6. **Inbound** — one receive loop routes CLOCK replies, answers PING with PONG, and
   counts NACKs (surfacing `RATE_LIMITED` separately) and STATE frames. Malformed frames
   are logged and skipped, never fatal.
7. **Reconnect** — an unexpected drop retries with 2 s → 5 s → 10 s backoff until you
   disconnect; the status shows "Reconnecting".

### Wire numbers used (all from `docs/slopsync/registry/registry.yaml`)

- **Frame types:** HELLO `0x00`, WELCOME `0x01`, PING `0x03`, PONG `0x04`, CLOCK `0x05`,
  SUBSCRIBE `0x06`, GRANT `0x08`, STATE `0x0B`, STREAM `0x0C`, NACK `0x10`, GOODBYE `0x11`.
- **Frame header:** 8 bytes little-endian `[type:u8][flags:u8][channel:u16][seq:u16][len:u16]`.
- **CBOR keys:** proto_ver 1, client_kind 2, client_name 3, instance_id 4, token 5,
  session_id 6, boot_id 7, catalog_etag 8, cfg_gen 9, subscriptions 10, publishes 11,
  rate_hz 12, priority 13, granted_rate_hz 14, channel_id 15, code 16, roles 23,
  deadman_ms 24, deadman_policy 25, grants 35, granted_publishes 36.
- **Channels:** safety `0x0003`, motion `0x0080`, motion-input `0x0084` (STREAM c2h, ≤333 Hz).
- **CBOR profile:** deterministic (§5.3) — definite lengths, shortest-form ints,
  float32-only (`0xFA` + big-endian binary32), map keys ascending.

---

## Developer notes

### Compile check
```
dotnet build clients/mfp-slopsync/SlopSync.csproj
```
`SlopSync.csproj` is a dev-only project that compiles `SlopSync.cs` standalone against a
local MFP install. **Edit its `HintPath`s** if your MFP is not at
`C:\Users\Atlan\Downloads\MultiFunPlayer-1.34.5-patreon-SelfContained.10.0.300\`.
It needs the `net10.0` SDK. The `#:` directives at the top of the plugin are legal because
MFP (and this project, via `<Features>FileBasedProgram</Features>`) compile in
file-based-program mode.

### Wire self-test (golden bytes)
```
dotnet run --project clients/mfp-slopsync/WireSelfTest.csproj
```
Byte-compares the C# encoder against hex derived by running `slopsync_probe.py`'s own
builder functions (HELLO, CLOCK, STREAM bundle, SUBSCRIBE, GOODBYE). Exits 0 on all-pass.
The codec in `WireSelfTest.cs` is a deliberate copy of `SlopSync.cs`'s — if you change one,
mirror the other and re-run.

### Live wire test (against the real device)
```
dotnet run --project clients/mfp-slopsync/LiveWireTest.csproj
```
Compiles `SlopSync.cs` itself (the plugin's real codec/client/discovery classes — no
copies) into a console harness and runs the full session against the live machine:
mDNS discovery, HELLO→WELCOME publish grant, CLOCK sync, 5 s of STREAM @ 50 Hz, then
diffs the device's `/api/slopmotion` ingress counters. **It refuses to run if the
machine is homed** (unhomed = every sample is dropped at the firmware's HOMED safety
gate, so the wire is exercised with zero motion risk). Run it twice back-to-back to
also regression-check source-ownership release on session end (fw ≥ 2.1.44).

---

## Troubleshooting

- **"no publish grant"** — the hub did not grant channel `0x0084`. Confirm the firmware
  advertises `features.slopsync` and the motion-input channel (`curl http://<ip>/api/capabilities`).
- **Bundles climb but nothing moves** — the machine is not homed, or another source owns
  motion. Home it; check the machine's own UI for the active control source.
- **NACK `RATE_LIMITED` counting up** — you are asking for more than the granted rate.
  Lower **Rate**; the plugin already streams at the granted rate, so this usually means a
  transient. Persistent overage can get the session evicted.
- **Won't connect** — wrong IP/port, machine off Wi-Fi, or mid-crash. Verify with
  `curl http://<ip>/api/capabilities`. Discovery finding nothing does not mean the machine
  is down — some networks block multicast; just type the address in.
- **Motion feels laggy or jerky** — raise the rate (up to what the device grants) for a
  denser stream. Velocity handoff (`vel_norm`) already feeds the planner's feedforward.
