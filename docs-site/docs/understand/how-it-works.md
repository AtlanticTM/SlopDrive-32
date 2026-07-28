---
title: How it works
description: >-
  The SlopSync mental model in seven diagrams: the catalog as a datasheet, the channel classes, connecting, ground truth, where data lives, safety, and a settings screen that builds itself.
register: STE
---

# How it works

A SlopSync machine describes itself. A client reads that description and builds
its interface from it. Everything on this page follows from those two
sentences.

This page is mostly pictures. Read them in order. You need no prior knowledge
of this protocol, of CBOR, or of motion control.

## How to read these diagrams

Every diagram on this site uses one visual language. Learn it once here.

<div class="ss-keys">
<span class="ss-key ss-key--reality"><b>reality</b>Measured truth. What the machine actually did, applied, or is reporting now.</span>
<span class="ss-key ss-key--intent"><b>intent</b>A wish. What somebody asked for, before the machine confirms anything.</span>
<span class="ss-key ss-key--safety"><b>safety</b>Stops and latches. This color never carries a second meaning.</span>
<span class="ss-key"><b>rectangle</b>A party or a step: a hub, a client, a decision.</span>
<span class="ss-key"><b>cylinder</b>Something that persists somewhere.</span>
<span class="ss-key"><b>rounded</b>One frame, in flight on the wire.</span>
<span class="ss-key"><b>▶ START</b>Where a flowchart begins. Every flowchart on this site marks it.</span>
</div>

The two colors are not decoration. They are the two colors the SlopDrive-32
machine paints on its own screen: blue for what the machine measured or
applied, purple for what a person asked for. They mean the same thing in a
diagram, in a table and on the machine. Amber and red are safety, in the
documentation and on the machine alike, and neither may restyle them.

The distinction those two colors carry is the whole product. Hold on to it and
the rest of this page is straightforward.

## 1. The machine hands you its datasheet

<p class="ss-cap" markdown>A client that knows nothing connects, reads the machine's self-description, and from that alone knows everything it may read, everything it may send, and everything it may not touch.</p>

```mermaid
flowchart LR
    C["▶ START<br/>A new client<br/>knows nothing<br/>about this machine"]:::party
    H["The hub<br/>one machine,<br/>one authority"]:::hub
    CAT[("Catalog<br/>the machine's own datasheet")]:::store
    R["Values it may read<br/>name · type · unit · limits"]:::truth
    W["Commands it may send<br/>name · fields · limits"]:::wish
    A["Doors it may not open<br/>an access level per channel"]:::party
    UI["A screen built from<br/>the machine, not from<br/>a driver"]:::party

    C -->|"1 · connects and asks"| H
    H -->|"2 · answers with its<br/>self-description"| CAT
    CAT --> R
    CAT --> W
    CAT --> A
    R --> UI
    W --> UI
    A --> UI

    classDef hub fill:#3183cc2e,stroke:#3183cc,stroke-width:2px
    classDef truth fill:#3183cc2e,stroke:#3183cc
    classDef store fill:#3183cc2e,stroke:#3183cc
    classDef wish fill:#8158d82e,stroke:#8158d8
    classDef party fill:none,stroke:#8a8f98
```

<p class="ss-point" markdown>**The point.** There is no device driver. The client is not compiled against this machine, and the machine is not compiled against this client. A control added in firmware appears on every client at its next connection, with the label, the unit and the limits supplied by the machine.</p>

A [catalog](../reference/dictionary.md#catalog) entry describes one
**channel**. Each entry carries an id, a name, a class, a direction, an access
level, a maximum rate, and a field-by-field description of its payload. The
whole catalog is hashed into an [etag](../reference/dictionary.md#etag), which
names exactly which catalog a hub exposes. A client that already holds that
etag has nothing to download.

## 2. Five channel classes, chosen by one question

Ask what breaks if a single frame never arrives. The answer picks the class.

<p class="ss-cap" markdown>The loss question, and the five answers that are the five channel classes.</p>

```mermaid
flowchart TD
    Q{"▶ START<br/>One frame is lost.<br/>What breaks?"}:::party

    Q -->|"Nothing. The next one<br/>replaces it whole"| S["STATE<br/>full snapshots of a group of values"]:::truth
    Q -->|"Almost nothing. It was one<br/>timestamped sample of many"| ST["STREAM<br/>bundles of samples, at rate"]:::truth
    Q -->|"The change never happens,<br/>and the sender must find out"| I["INTENT<br/>the only way anything changes"]:::wish
    Q -->|"A moment is missed"| E["EVENT<br/>edges, not levels"]:::truth
    Q -->|"Nothing. The document<br/>is fetched again"| B["STORE<br/>numbered slots holding documents"]:::store

    E -.->|"so anything that matters<br/>also has a STATE twin"| S

    classDef truth fill:#3183cc2e,stroke:#3183cc
    classDef wish fill:#8158d82e,stroke:#8158d8
    classDef store fill:none,stroke:#8a8f98
    classDef party fill:none,stroke:#8a8f98
```

<p class="ss-point" markdown>**The point.** Loss tolerance is designed in per class, not bolted on afterwards. Only intents demand an end-to-end confirmation, and only intents get one.</p>

| Class | Carries | If a frame is lost |
|---|---|---|
| [STATE](../reference/dictionary.md#state) | A full snapshot of a group of values | Harmless. The next snapshot supersedes it. There are no deltas, ever. |
| [STREAM](../reference/dictionary.md#stream) | Timestamped [bundles](../reference/dictionary.md#bundle) of samples | Recoverable. Samples carry time, so a consumer interpolates across the hole. |
| [INTENT](../reference/dictionary.md#intent) | One absolute command | The change does not happen. The hub answers every intent with an [ECHO](../reference/dictionary.md#echo) or an error, so the sender finds out. |
| [EVENT](../reference/dictionary.md#event) | One occurrence, at one moment | A moment is missed. Events are never replayed, so no safety behavior may depend on one. |
| [STORE](../reference/dictionary.md#store) | Numbered slots holding opaque documents | Harmless. The client asks for the document again. |

Two rules follow from that table, and both are load-bearing.

**Every STATE frame is a full snapshot.** A delta would make one lost frame
corrupt everything after it. Full snapshots are also what make
[conflation](../reference/dictionary.md#conflation) safe: when a link is slow
the hub replaces the queued snapshot with the newer one, so a subscriber sees
the freshest truth its link can carry and never a backlog.

**Every event that matters has a STATE twin.** The event says *this just
happened*. The state says *this is still true*. A client that arrives after the
event adopts the state and needs no history.

## 3. Connecting: two paths, one gate

<p class="ss-cap" markdown>A cold client fetches the catalog; a returning client whose cached etag matches skips straight through. Both reach the same gate.</p>

```mermaid
%%{init: {"themeVariables": {"actorLineColor": "#8a8f98"}} }%%
sequenceDiagram
    autonumber
    participant C as Client
    participant H as Hub

    C->>H: HELLO — who I am, what I want,<br/>and which catalog I already hold
    H->>C: WELCOME — session, roles, grants,<br/>the hub's catalog etag

    alt Cached etag matches the hub's
        Note over C,H: Holding the etag proves the client holds the catalog.<br/>Nothing is transferred.
    else No etag, or a different one
        C->>H: BLOB_REQ — send me the catalog
        H-->>C: BLOB_CHUNK, repeated
        Note over C,H: The client hashes what it assembled.<br/>The etag verifies the transfer by itself.
        C->>H: CATALOG_READY — I now operate against this etag
    end

    H->>C: retained STATE, for every granted channel
    Note over C,H: LIVE. Only now may the client act on user input.
```

<p class="ss-point" markdown>**The point.** The hub sends no state at all until the client has confirmed which catalog it decodes against. This is the [ready gate](../reference/dictionary.md#ready-gate). Without it a client would receive packed bytes it cannot interpret, and the only way to read them anyway would be a hardcoded copy of this machine's layouts — the exact coupling a catalog exists to remove.</p>

Two other things happen in that handshake, and both matter later.

**Grants are truth; requests are wishes.** A client asks to receive a channel
at some rate. The hub answers with the rate it will actually deliver, and that
answer is a [grant](../reference/dictionary.md#grant). An interface showing a
rate it was never granted is lying about itself.

**A client stays visibly unready until it is LIVE.** The specification requires
the difference to be visible. Stale data displayed as fresh is the failure this
whole design exists to prevent.

## 4. Ground truth: 420 goes in, 400 comes out everywhere

<p class="ss-cap" markdown>A remote asks for a value above the machine's ceiling. Watch which number every screen ends up showing.</p>

```mermaid
%%{init: {"themeVariables": {"actorLineColor": "#8a8f98"}} }%%
sequenceDiagram
    autonumber
    participant R as Remote
    participant H as Hub
    participant B as Browser

    R->>H: INTENT — set speed to 420
    Note right of H: The ceiling is 400.<br/>The hub applies 400.
    H->>R: ECHO — applied 400
    H->>R: STATE — speed 400
    H->>B: STATE — speed 400
    Note over R,B: Every screen shows 400.<br/>Nobody shows 420, including the remote that asked.
```

<p class="ss-point" markdown>**The point.** The echo reports what the machine applied, never what the client requested. A client's [shadow](../reference/dictionary.md#shadow) updates from the hub's answer, never from its own request. This is why optimistic interface state is prohibited rather than discouraged: on a machine that moves, a screen that lies about state is a safety defect.</p>

A control therefore does not jump to 420 while it waits. It shows the request
as **pending** and adopts the applied value when the echo arrives. Limits are
ceilings, never targets, and a [clamp](../reference/dictionary.md#clamp) is not
an error: 400 is simply the answer.

> DEMO-CANDIDATE: a live slider that lets a reader ask a real simulated hub
> for a value past its ceiling, and watch pending, echo and clamp happen in
> real time.

Two more rules let this survive a bad network.

**Intents are absolute, never relative.** "Set speed to 405" survives a
reconnect, because a client can compare it against the snapshot it adopts. "Add
20" cannot be reconciled against anything.

**A duplicate intent is harmless.** Each intent carries an id. The hub keeps a
small ring of recent ids and re-sends the stored echo for a repeat, instead of
applying it twice.

## 5. Where the data lives

<p class="ss-cap" markdown>The hub owns the only copy that counts. What a client holds is a shadow of it, and a shadow never survives a reconnect.</p>

```mermaid
flowchart TD
    H[("▶ START<br/>Hub<br/>the retained value of<br/>every STATE channel")]:::truth

    H -->|"on subscribe: the retained<br/>value, immediately"| S1[("Shadow<br/>in the browser")]:::truth
    H -->|"and to every other subscriber"| S2[("Shadow<br/>in the remote")]:::truth

    S1 --> V1["Screens read<br/>the shadow"]:::party
    S2 --> V2["Screens read<br/>the shadow"]:::party

    V1 -.->|"a wish, never a write"| I(["INTENT"]):::wish
    I -.-> H

    S1 ==>|"link drops"| X["The shadow is<br/>discarded whole"]:::party
    X ==>|"reconnect: adopt,<br/>never merge"| H

    classDef truth fill:#3183cc2e,stroke:#3183cc
    classDef wish fill:#8158d82e,stroke:#8158d8
    classDef party fill:none,stroke:#8a8f98
```

<p class="ss-point" markdown>**The point.** No client state survives a reconnect on its own authority. The hub keeps the [retained value](../reference/dictionary.md#retained-value) of every STATE channel and pushes it the moment a client is granted the channel, so re-adoption is the ordinary connect path rather than a special recovery path.</p>

A reconnecting client re-sends nothing blindly. It compares what it still wants
against the snapshot it just adopted, and issues an intent only if the two
still differ.

**Reconnecting is not re-taking control.** Subscriptions come back freely. If
the disconnection stopped motion, the returning session must ask for control
again with a fresh intent. Motion never restarts because a socket reopened.

## 6. Safety: the latch is the truth

<p class="ss-cap" markdown>An emergency stop from any endpoint, and why the latched state — not the event — is what every client obeys.</p>

```mermaid
flowchart TD
    A["▶ START<br/>Any endpoint.<br/>Any role. Even with no session."]:::safety
    A -->|"ESTOP frame"| Q["Every queue on the path<br/>admits it at the front"]:::plumb
    Q --> H["The hub stops motion FIRST,<br/>then does protocol bookkeeping"]:::safety
    H --> L[("safety STATE — the latch.<br/>Stays true until it is cleared.")]:::safety
    H --> E(["EVENT — the edge.<br/>A toast. A log line."]):::party
    L -->|"retained: pushed to every client,<br/>including one that arrives later"| N["Every screen agrees<br/>the machine is stopped"]:::party
    E -.->|"never replayed"| N
    N -.->|"repeat until the latch<br/>is observed"| A

    classDef safety fill:#d8434f2e,stroke:#d8434f,stroke-width:2px
    classDef party fill:none,stroke:#8a8f98
    classDef plumb fill:none,stroke:#8a8f98,stroke-dasharray:3 3
```

<p class="ss-point" markdown>**The point.** There is no acknowledgement frame for an emergency stop. The initiator repeats the frame until it observes the [latch](../reference/dictionary.md#latch) in state. An observable latch is the only acknowledgement worth anything, because it is the same latch every other client is reading.</p>

Safety outranks authorization by design. Anyone may stop the machine; not
everyone may start it. Clearing the latch needs the `control` tier, needs the
cause to be resolved, and re-arms motion rather than resuming it.

A machine also notices when nobody is watching it.
[Deadman](../reference/dictionary.md#deadman) binds to the source currently
driving motion. If that source goes silent for its window, the hub releases
its ownership — bookkeeping, not a command. Nothing broadcasts a forced stop:
a vanished streaming client was already the reason no fresh commands were
arriving, so motion settles by physics rather than by a safety action. A
pattern running on the hub keeps running by default, because a locked phone
screen was never what drove it; whether a given source keeps going or stops
when its owner disappears is that source's own declared policy, not a
universal deadman reflex.

## 7. A settings screen that builds itself

This is the payoff of everything above. A machine describes not only its values
but what they mean, so a client renders a complete settings surface it was
never written for.

<p class="ss-cap" markdown>One annotated catalog field becomes one control, and the loop from edit back to displayed value never shortcuts through the client's own guess.</p>

```mermaid
flowchart TD
    CAT[("▶ START<br/>Catalog field<br/>unit · min · max · default · options<br/>role · category · setting_key")]:::store
    CAT --> Q{"Does it carry<br/>a setting_key?"}:::party
    Q -->|"no — it is a reading"| RO["Read-only display,<br/>with its unit"]:::truth
    Q -->|"yes — it is a setting"| WID["A control, chosen from<br/>the type and the constraints"]:::truth
    WID --> U["Somebody edits it"]:::wish
    U --> I(["INTENT on the paired channel"]):::wish
    I --> EC(["ECHO — the applied value"]):::truth
    EC -->|"the control adopts<br/>what was applied"| WID
    EC --> ST(["STATE — to everyone else"]):::truth
    ST --> OTH["Every other client shows<br/>the same value"]:::truth

    classDef store fill:none,stroke:#8a8f98
    classDef party fill:none,stroke:#8a8f98
    classDef truth fill:#3183cc2e,stroke:#3183cc
    classDef wish fill:#8158d82e,stroke:#8158d8
```

<p class="ss-point" markdown>**The point.** A field with a [setting_key](../reference/dictionary.md#setting_key) is a setting, and the key names the command that writes it. A field without one is a reading. That single annotation replaces the hand-written table every client used to keep, pairing each display to its control — the table that goes stale the day the firmware changes.</p>

The catalog says what a value **is**. It never says how a value should
**look**. There is no widget field, deliberately. A phone renders a range as a
slider, a small remote as a click wheel, a desktop plugin as a numeric box:
same bytes, three honest interfaces.

| The annotation | What a client does with it |
|---|---|
| [`setting_key`](../reference/dictionary.md#setting_key) | Writable, and here is the command key that writes it. Absent means read-only. |
| `min`, `max`, `step`, `unit`, `default` | Choose and bound the control. The hub still validates; these are for display. |
| `options` | Name the choices of a single-select, instead of showing raw numbers. |
| [`role`](../reference/dictionary.md#field-role) | Say what the value *is* semantically, so a client that recognizes it **may** upgrade to a purpose-built control. Fallback is mandatory; upgrades are optional. |
| [`category`](../reference/dictionary.md#setting-category) | Which tab it belongs in, from a registered list, so placement stays consistent across different machines. |
| `flags` | `advanced` hides it behind an affordance, never removes it. `secret` means the value never appears in state at all — only whether it is set. |

A setting the machine cannot accept right now is grayed, never hidden. The
machine says so in its own state, and every client grays from that one truth.

The registered vocabularies — every category, role, flag and field type — are
in the [catalog vocabulary reference](../reference/registry/catalog-vocabulary.md),
generated from the registry.

## Where to go next

- [Anatomy of a frame](anatomy.md) — the eight bytes every frame starts with,
  and why there are two payload encodings.
- [Capabilities and custom hardware](capabilities.md) — what your device must
  provide to be a hub.
- [The Dictionary](../reference/dictionary.md) — every term on this page, with
  exactly one definition each.
