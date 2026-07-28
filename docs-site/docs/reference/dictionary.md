---
title: The SlopSync Dictionary
description: Every SlopSync term with exactly one definition: hub, client, session, channel, catalog, etag, grant, shadow, deadman, intent, echo, and the rest.
register: STE
generated: true
---

<!-- ==========================================================
     GENERATED FILE. DO NOT EDIT.
     Source of truth: docs-site/dictionary.yaml
     Generator:       docs-site/tools/gen_docs_tables.py
     Edit the YAML, then regenerate. The tooltip definitions in
     includes/abbreviations.md come from the same source, so a term
     can never have two definitions.
     ========================================================== -->

# The SlopSync Dictionary

One term, one meaning. This page is the vocabulary the specification, the guides and the reference tables all draw from. Where a word appears anywhere on this site, it means what it means here.

The Dictionary keeps industry vocabulary intact. A word like *deadman*, *etag*, *token bucket* or *quintic* is precise, so it stays and gets defined. Simplification applies to sentence structure, never to terminology.

!!! abstract "One term, one meaning"

    Every term below has exactly one definition on this site. If prose
    anywhere uses a word from this page, it means what this page says.
    Hover any defined term anywhere on the site to see its definition.

## Endpoints and roles

SlopSync has one authority per machine and any number of peers talking to it. These are the words for the parties.

### Hub

**The single authoritative endpoint of one machine.**

The hub owns machine state, the catalog, and every grant. There is
exactly one hub per machine. On SlopDrive-32 the hub is the ESP32-S3
main controller.

The hub is the only party that decides anything. A client asks; the
hub answers with what it applied.

See also: [Client](#client), [Relay](#relay), [Catalog](#catalog) · Source: SPEC
{ .ss-termmeta }

### Client

**Any endpoint that establishes a session with the hub.**

A browser UI, a desktop app plugin, a hardware remote, a simulator
and a test probe are all clients. The protocol does not rank them.
What a client may do comes from its access level, never from what
kind of thing it is.

See also: [Session](#session), [Access level](#access-level), [Constrained client](#constrained-client) · Source: SPEC
{ .ss-termmeta }

### Relay

**A forwarding node between the hub and clients on a transport the hub cannot reach directly.**

A relay is not a session peer. It is invisible to the session layer,
except where the specification says otherwise. A relay must forward
an ESTOP ahead of all buffered traffic, on every attached segment,
including upstream.

A relay's death makes its clients silent. That triggers the same
deadman path as a client's death. The hub cannot tell the two apart
and does not need to.

See also: [ESTOP](#estop), [Deadman](#deadman) · Source: SPEC
{ .ss-termmeta }

### Constrained client

**A client that ships a compiled-in catalog and pre-encoded templates instead of a CBOR parser.**

Also called the **static-client profile**. A coin-cell remote or a
minimal BLE device sends its compiled-in etag in HELLO. If the hub's
etag matches, it runs at full speed.

On mismatch it must pick a declared behavior: run degraded, with
every control function it cannot re-verify suppressed; or refuse and
show an "update me" indication. Silent full operation on a
mismatched etag is non-conformant.

See also: [Etag](#etag), [Catalog](#catalog) · Source: SPEC
{ .ss-termmeta }

### Instance id

**Eight bytes a client generates once and persists, saying who it durably is.**

The instance id distinguishes "the same phone reconnecting" from "a
second phone". A client that cannot persist, such as a private
browsing window, generates one per load and accepts weaker reconnect
semantics.

A second HELLO carrying a live instance id evicts the older session.
That rule is why a token re-proof rides its own frame instead of a
repeat handshake.

See also: [Session id](#session-id), [Boot id](#boot-id) · Source: SPEC
{ .ss-termmeta }

### Session id

**A random non-zero 32-bit number the hub assigns to one association, unique within a hub boot.**

The session id names this particular association. It is not a
secret. Authorization lives in tokens, never in the id.

See also: [Instance id](#instance-id), [Boot id](#boot-id), [Session](#session) · Source: SPEC
{ .ss-termmeta }

### Boot id

**A random 32-bit number the hub generates at every boot, naming which incarnation of the hub you are talking to.**

Every hub timestamp, sequence number, session id and idempotency
record is scoped to a boot id. Observing a new one invalidates every
cached assumption, except the catalog etag and pairing tokens.

See also: [Cfg_gen](#cfg_gen), [Etag](#etag), [Session id](#session-id) · Source: SPEC
{ .ss-termmeta }

## Access tiers

A session holds exactly one access level. A channel declares the level a session needs to subscribe to it, or to send on it. The wire values did not change when the tiers were renamed for v1.0.

### Access level

**The tier a session holds, which gates what channels it may read and write.**

The three tiers are `watch`, `control` and `configure`. See the
generated [access level table](registry/channels.md#access-levels)
for the wire values.

Access control acts at subscribe time and at intent time. It never
filters the catalog: every session sees the same entries and the
same etag.

See also: [Watch](#watch), [Control](#control), [Configure](#configure), [Role](#role) · Source: SPEC, RFC-027
{ .ss-termmeta }

### Watch

**The lowest access tier; a watch session observes state and may still stop the machine.**

A watch session subscribes to open channels and renders them. It
cannot own a motion source.

It **can** send `stop` and `estop`. Those two operations are exempt
from access levels, because the person standing in the room must be
able to stop the machine.

See also: [Control](#control), [Configure](#configure), [ESTOP](#estop) · Source: RFC-027
{ .ss-termmeta }

### Control

**The middle access tier; a control session drives the machine and may own a motion source.**

Publishing a motion STREAM is a control act. A motion producer is a
controller.

Earlier drafts called this tier `controller`. The wire value is
unchanged.

See also: [Watch](#watch), [Configure](#configure), [Active source](#active-source) · Source: RFC-027
{ .ss-termmeta }

### Configure

**The highest access tier; a configure session changes device configuration and administers pairing.**

A configure session approves pending pairings, evicts other
sessions, and reads the paired-device roster.

Earlier drafts called this tier `admin`. The wire value is
unchanged. Configure is reachable by pairing ceremony, so it is not
restricted to a hub's own built-in UI.

Firmware update rights are never derivable from any tier.

See also: [Watch](#watch), [Control](#control), [Knock-and-approve](#knock-and-approve) · Source: RFC-027
{ .ss-termmeta }

### Role

**The access level attached to a grant, and the word the wire uses for it.**

The role is an attribute of the **grant**, never of the pairing
ceremony. All pairing modes end in the same grant shape. A hub has
zero or one PIN; it never keeps one secret per tier.

See also: [Access level](#access-level), [Pairing](#pairing), [Token](#token) · Source: RFC-027
{ .ss-termmeta }

## The wire

Everything SlopSync sends is a frame. A frame belongs to one of two planes, and the plane decides how its payload is encoded. These are the words the diagrams in Understand use.

### Frame

**One SlopSync message, an eight-byte header followed by a payload.**

The header names the frame type, the channel, the sequence number
and the payload length. Every frame starts this way, so a receiver
can skip a frame it does not understand instead of disconnecting.

One frame maps to one transport message wherever the transport
allows it. The length field makes frames self-delimiting on
transports that are a byte pipe rather than a message queue.

See also: [Control plane](#control-plane), [Data plane](#data-plane), [Sequence number](#sequence-number) · Source: SPEC
{ .ss-termmeta }

### Control plane

**The frames that negotiate, command and confirm, carrying CBOR map payloads.**

The handshake, subscriptions, grants, intents, echoes, events and
errors are all control plane. These frames are occasional, so they
pay for self-description: each payload is a CBOR map whose integer
keys mean the same thing in every message.

The control plane is where a client may meet a key it has never
seen. It ignores that key and keeps going.

See also: [Data plane](#data-plane), [CBOR](#cbor), [Frame](#frame) · Source: SPEC
{ .ss-termmeta }

### Data plane

**The frames that carry machine values at rate, carrying packed struct payloads.**

STATE snapshots and STREAM bundles are data plane. These frames are
frequent, so they carry no keys and no type tags at all. They carry
only values, in the exact order the catalog's layout declares.

The saving is real. The same content costs roughly a third as many
bytes packed as it does in CBOR, and it decodes with a pointer cast
on hardware that has no room for a decoder.

See also: [Control plane](#control-plane), [Layout](#layout), [Packed field type](#packed-field-type) · Source: SPEC
{ .ss-termmeta }

### CBOR

**A compact binary encoding of maps, arrays and numbers, used here in a deterministic profile.**

CBOR is to binary what JSON is to text: the same shapes, far fewer
bytes, and no parsing ambiguity. SlopSync restricts it further:
definite lengths, shortest-form integers, sorted keys, binary32
floats, no tags.

The restriction buys two things. Any message has exactly one valid
encoding, so test vectors compare byte for byte. And a client too
small for an encoder can ship a canned template and patch values
into it, knowing the bytes are what a real encoder would produce.

Keys are integers from one registry, never strings. A key means the
same thing in every message on the protocol.

See also: [Control plane](#control-plane), [Schema](#schema), [Golden vector](#golden-vector) · Source: SPEC
{ .ss-termmeta }

### Sequence number

**A per-channel, per-direction counter that lets a receiver reject anything older than what it already holds.**

The counter is 16 bits and wraps. Comparisons use serial
arithmetic, so a wrap is not mistaken for a jump backwards.

STATE is newest-wins: a snapshot older than the shadow's is
dropped. This, not arrival order, is what makes state correct on
a transport that reorders. Gaps are normal and mean nothing,
because conflation is allowed by design.

See also: [Frame](#frame), [Conflation](#conflation), [State](#state) · Source: SPEC
{ .ss-termmeta }

## The catalog

The catalog is the hub's machine-readable self-description. It is the datasheet a client reads before it renders anything. Capability discovery is catalog introspection: a feature exists if and only if its channels exist.

### Catalog

**The hub's machine-readable description of every channel it exposes.**

The catalog is an array of channel entries. Each entry carries an
id, a name, a class, a direction, an access level, a maximum rate, a
default priority, and either a `layout` or a `schema`.

The catalog is **client-invariant**. Every session sees the same
entries and the same etag. Per-client catalogs would break etag
caching and the static-client profile.

See also: [Catalog entry](#catalog-entry), [Etag](#etag), [Layout](#layout), [Schema](#schema) · Source: SPEC
{ .ss-termmeta }

### Catalog entry

**One channel's description inside the catalog, encoded as an independent self-delimiting document.**

Each entry individually satisfies the CBOR depth cap, so a decoder
may process entries one at a time with per-entry state. The etag is
computed over these exact concatenated bytes.

See also: [Catalog](#catalog), [Etag](#etag) · Source: SPEC
{ .ss-termmeta }

### Etag

**The first eight bytes of a SHA-256 over the deterministically encoded catalog, naming exactly which catalog a hub exposes.**

Deterministic encoding makes the hash reproducible from catalog
content alone. Any implementation, any language, same bytes, same
etag.

The etag covers ids, names, classes, access levels, rates, layouts
and schemas. It does **not** cover retained values; that is what
`cfg_gen` and sequence numbers are for.

A `cfg_gen` bump must not change the etag. A catalog change must
change it.

See also: [Catalog](#catalog), [Cfg_gen](#cfg_gen), [Constrained client](#constrained-client) · Source: SPEC
{ .ss-termmeta }

### Cfg_gen

**A 16-bit generation counter that increments whenever applied configuration content changes.**

The three version-like tokens answer three different questions and
must not be conflated:

| Token | Question |
|---|---|
| `proto_ver` | What wire grammar are we speaking? |
| `catalog_etag` | What channels and schemas does this hub expose? |
| `cfg_gen` | Which generation of config *content* is current? |

A client guards a read-modify-write against races by sending its
expected `cfg_gen` as a `precondition`. A mismatch returns NACK
`CONFLICT`; the client re-reads and retries.

See also: [Etag](#etag), [Precondition](#precondition), [Intent](#intent) · Source: SPEC
{ .ss-termmeta }

### Layout

**The field-by-field description of a packed payload, in wire order.**

A layout describes STATE and STREAM payloads. Every field is
fixed-width, so every offset is static. Layouts evolve
**append-only**: a changed or removed field means a new channel id,
never a redefined one.

A receiver ignores trailing bytes past the layout it knows. That is
what lets an old client keep parsing a grown snapshot.

See also: [Schema](#schema), [Packed field type](#packed-field-type), [Catalog entry](#catalog-entry) · Source: SPEC
{ .ss-termmeta }

### Schema

**The key-by-key description of a CBOR payload.**

A schema describes INTENT and EVENT payloads. Its integer keys are
the channel's own, taken from the catalog entry and never from the
global CBOR key space.

Exactly one of `layout` or `schema` is present on an entry,
according to its class.

See also: [Layout](#layout), [Intent](#intent), [Event](#event) · Source: SPEC
{ .ss-termmeta }

### Field role

**A text tag on a catalog field that says what the value semantically is.**

A role is a string, not a number, because action roles carry a
device-chosen suffix such as `action.reset_stats`.

The doctrine is binding: **roles are opportunities, never
requirements**. A client that recognizes `telemetry.position` may
render a position scope. A client that does not must fall back to
generic rendering by type and constraints. An unknown role is never
an error.

The registered vocabulary is in the [field role
table](registry/catalog-vocabulary.md#field-roles). The dotted
namespace is open; unregistered roles are legal.

See also: [Setting category](#setting-category), [Schema](#schema), [Layout](#layout) · Source: RFC-006, RFC-009, RFC-019
{ .ss-termmeta }

### Setting category

**The tab a settings field belongs in, chosen from a registered list so placement is consistent across hubs.**

Values 0-127 are registered and ordered. Values 128-255 are
device-defined and the hub supplies the label.

A category **spans channels**: `user` and `user-2` merge into one
tab. That is how a category outgrows one 242-byte snapshot.

There is no widget field, deliberately. SlopSync describes what
things are, never how they look.

See also: [Setting flag](#setting-flag), [Setting_key](#setting_key), [Field role](#field-role) · Source: RFC-009
{ .ss-termmeta }

### Setting flag

**A per-field annotation marking a setting advanced, restart-required, or secret.**

`secret` is normative and load-bearing: the value **never** appears
in STATE. The snapshot carries only a set-or-unset presence bit. A
Wi-Fi password must never ride a retained snapshot that open
`watch` sessions receive.

`advanced` means hide behind an affordance. It never means remove
from the surface.

See also: [Setting category](#setting-category), [Retained value](#retained-value) · Source: RFC-009
{ .ss-termmeta }

### Setting_key

**The catalog annotation naming which key of the paired INTENT channel writes this displayed field.**

A field with a `setting_key` is writable. A field without one is a
read-only display. That single annotation is what lets a generic
client build a working settings form from a catalog it has never
seen.

See also: [Setting category](#setting-category), [Intent](#intent), [Field role](#field-role) · Source: RFC-009
{ .ss-termmeta }

### Packed field type

**One of the fixed-width numeric, bitfield or fixed-width-string types a packed layout may use.**

Fixed width is the whole point. Variable-length fields are banned
from packed layouts because every offset must be static.

String types are zero-padded UTF-8 at 16, 32 or 64 bytes. A reader
stops at the first NUL or at the width, whichever comes first.
STREAM sample layouts stay string-free: the motion path never pays
for text.

See also: [Layout](#layout) · Source: RFC-026
{ .ss-termmeta }

### Stream kind

**The catalog property saying whether a STREAM sample reports a value at an instant, or commands a time extent.**

`samples` is the default. A dropped sample is recoverable by
interpolation from its neighbors, so it may be decimated under
congestion.

`segments` is not. Each sample carries its own duration, so it is
not a point on a continuous curve. A dropped segment is a
permanently lost command. Segment channels shed whole-source or not
at all.

This is an explicit registered property, not a guess from a unit
string. Two conforming hubs reading `ms` versus `msec` would have
shed differently under identical congestion.

See also: [Segment](#segment), [Sample](#sample), [Shedding](#shedding) · Source: RFC-014, RFC-023
{ .ss-termmeta }

### Capability discovery

**Learning what a machine can do by reading its catalog, rather than from a separate feature list.**

A feature exists if and only if its channels exist. A machine with
no current sensor does not declare a power channel, and that
absence **is** the answer to "does it measure current?".

There is no parallel capability list, deliberately. A second list
is a second truth, and a second truth drifts from the first one.
The catalog is already the machine's self-description, so it
answers the question with the same bytes it uses for everything
else.

See also: [Catalog](#catalog), [Catalog entry](#catalog-entry), [Field role](#field-role) · Source: RFC-016
{ .ss-termmeta }

## Channels and their classes

Every flow of data is a channel, and every channel has exactly one class. The class decides the frames it uses and the delivery rules it obeys.

### Channel

**A named, numbered, typed data flow declared in the catalog.**

Ids `0x0001`-`0x007F` are spec-governed. Ids `0x0080`-`0x7FFF` are
allocated by hub firmware and described entirely by the catalog. A
device can therefore add a channel without a specification change.

See also: [Channel class](#channel-class), [Catalog](#catalog) · Source: SPEC
{ .ss-termmeta }

### Channel class

**One of STATE, STREAM, INTENT, EVENT or STORE.**

"STREAM" here is a channel class. Transports are described as
*stream-oriented* or *datagram-oriented*, never as "stream
transports", so the two ideas never collide.

See also: [State](#state), [Stream](#stream), [Intent](#intent), [Event](#event), [Store](#store) · Source: SPEC
{ .ss-termmeta }

### State

**A channel class carrying idempotent full snapshots of a coherent group of fields.**

Every STATE frame contains the complete current value of its
channel. There are no deltas in `slopsync/1`. A delta would make
frame loss corrupting, which destroys the property the whole design
leans on.

A STATE payload must fit 242 bytes unfragmented. That is a catalog
design constraint: a state group that does not fit is split into
two channels at authoring time.

See also: [Retained value](#retained-value), [Conflation](#conflation), [Shadow](#shadow) · Source: SPEC
{ .ss-termmeta }

### Stream

**A channel class carrying timestamped sample bundles, in either direction.**

Position telemetry flows hub to client. Motion input flows client to
hub. STREAM frames are never acknowledged, in either direction.

Grants bound the **sample** rate, not the frame rate. A 240 Hz grant
delivered as roughly 48 bundles per second of five samples each is
conformant and expected.

See also: [Bundle](#bundle), [Sample](#sample), [Segment](#segment), [Stream kind](#stream-kind) · Source: SPEC
{ .ss-termmeta }

### Intent

**A channel class, and the only way a client changes anything.**

An intent carries absolute values only. A schema expresses target
state, such as "set speed 400". It never expresses an operation on
current state, such as "add 20".

That single rule is what makes reconnect sound and two administrators
racing merely annoying instead of corrupting. A client wanting an
increment computes the absolute target from its shadow.

See also: [Echo](#echo), [Precondition](#precondition), [Idempotency ring](#idempotency-ring), [Shadow](#shadow) · Source: SPEC
{ .ss-termmeta }

### Echo

**The hub's mandatory, truthful reply to an intent, carrying the values actually in effect after clamping.**

`applied` may differ from what was requested. The client's shadow
updates from the echo and from the following STATE broadcast, never
from its own request.

An echo goes only to the sender. Every other subscriber learns of
the change from STATE.

This is the ground-truth doctrine on the wire: the reported value is
the post-clamp value the machine holds, not the pre-clamp value
somebody asked for.

See also: [Intent](#intent), [Ground truth](#ground-truth), [Shadow](#shadow) · Source: SPEC
{ .ss-termmeta }

### Event

**A channel class carrying discrete occurrences, edges, not levels.**

Events are best-effort. They are bounded, conflated and **not
replayed** on reconnect.

Therefore the event and state duality rule applies: any event a
client could not afford to miss must have a latched STATE twin. The
event says "this just happened". The state says "this is still
true".

No safety behavior may depend on event delivery. Events are user
interface garnish. States are truth.

See also: [State](#state), [Latch](#latch), [Retained value](#retained-value) · Source: SPEC
{ .ss-termmeta }

### Store

**A channel class declaring a set of numbered slots holding opaque documents.**

Presets, saved positions, limit profiles, recordings and the trust
ledger are all stores. A store is declared as an ordinary catalog
entry, so the catalog root shape, id sort, etag computation and
per-entry depth rules are untouched.

A store's items move over the blob transfer verbs, not over intents.
A store usually comes as a pair: a static STORE descriptor, plus a
tiny dynamic roster STATE whose generation counter means
"re-enumerate".

See also: [Blob](#blob), [Blob namespace](#blob-namespace) · Source: RFC-021
{ .ss-termmeta }

### Blob

**An opaque byte document the protocol transfers in chunks and never decodes.**

One transfer verb serves the whole protocol. The blob namespace says
what is being moved: the catalog, or an item in a store.

The payload is opaque **by rule**. The specification never decodes
it, and the parser-robustness budget explicitly does not extend
inside it. A client that decodes a blob has stepped above the
protocol boundary and owns what it finds there.

See also: [Store](#store), [Blob namespace](#blob-namespace), [Catalog](#catalog) · Source: RFC-021
{ .ss-termmeta }

### Blob namespace

**The selector saying which blob space a transfer addresses.**

Namespace 0 is the catalog. Namespace 1 is store items. The catalog
is the only namespace with a readiness concept, because you cannot
decode STATE without the catalog, and nothing gates on a preset.

See also: [Blob](#blob), [Store](#store), [Ready gate](#ready-gate) · Source: RFC-021
{ .ss-termmeta }

## Session lifecycle

A session is created by a handshake, kept alive by evidence of life, and torn down by exactly one code path no matter what killed it.

### Session

**The stateful association between one client and the hub.**

A session is created by HELLO and WELCOME. It is destroyed by
GOODBYE, by eviction, or by timeout.

A client session runs `CLOSED → CONNECTING → HELLO_SENT → SYNCING →
LIVE`. A client must not act on user input that needs hub state
before it reaches LIVE, and must visually distinguish SYNCING from
LIVE.

See also: [Handshake](#handshake), [Ready gate](#ready-gate), [Teardown](#teardown) · Source: SPEC
{ .ss-termmeta }

### Handshake

**The HELLO and WELCOME exchange that opens a session.**

HELLO carries who the client is, what protocol version it speaks,
what it wants to subscribe to, and what it wishes to publish.

WELCOME carries the session id, the boot id, the catalog etag, the
hub's limits, the hub's identity, the granted subscriptions and the
granted publishes.

The mandatory client floor is small on purpose: no required crypto
and 24 bytes of stored identity. A coin-cell remote omits every
optional map.

See also: [Session](#session), [Grant](#grant), [Publish grant](#publish-grant) · Source: SPEC
{ .ss-termmeta }

### Grant

**The hub's applied answer to a subscription request, which channel, at what rate, at what priority.**

**Grants are truth. Requests are wishes.** A wish is clamped by the
catalog maximum rate, by the session's role, by hub capacity and by
the link estimate.

The hub may also send an unsolicited grant to re-state what it is
actually delivering, after a new client joined, a probe justified a
raise, or congestion forced a cut. A client complies immediately and
should reflect the change in its interface. A scope view showing
60 Hz while granted 20 Hz is lying.

See also: [Publish grant](#publish-grant), [Priority class](#priority-class), [Shedding](#shedding) · Source: SPEC
{ .ss-termmeta }

### Publish grant

**The hub's applied answer to a client's wish to send on an inbound STREAM channel.**

A client states its publish wishes in HELLO, or renegotiates
mid-session. The hub answers with the granted rate and the granted
burst.

A hub accepts an inbound bundle only on a channel the sending
session holds a publish grant for. A bundle on an unknown,
ungranted, wrong-class or wrong-direction channel is dropped
silently and counted, never NACKed.

See also: [Grant](#grant), [Token bucket](#token-bucket), [Burst](#burst), [Bundle](#bundle) · Source: RFC-013, SPEC
{ .ss-termmeta }

### Ready gate

**The rule that a session's frames are refused until it confirms which catalog it has adopted.**

After the catalog transfers, the client sends a readiness frame
carrying the etag it now operates against. The frame is idempotent
and is re-sent until retained STATE arrives.

The gate closes **both planes**. A pre-ready intent is refused, not
queued, because a client acting before it has adopted the retained
safety latch breaks the invariant that every arriving client adopts
the latch before it can act.

A session that never becomes ready is closed with a readiness
timeout. Liveness reaping alone never fires on a client that pings
happily forever.

See also: [Retained value](#retained-value), [Latch](#latch), [Session](#session) · Source: RFC-015
{ .ss-termmeta }

### Retained value

**The latest value of a STATE channel, which the hub keeps and pushes immediately on grant.**

This is the device-shadow primitive. It is what "page load adopts
device state" compiles to.

A hub seeds its retained safety snapshot at construction. A freshly
booted hub that retained nothing would hand a connecting client an
empty latch, which is exactly the lie the doctrine forbids.

See also: [Shadow](#shadow), [State](#state), [Latch](#latch), [Ground truth](#ground-truth) · Source: SPEC
{ .ss-termmeta }

### Shadow

**The client-side replica of subscribed state, maintained exclusively from hub frames.**

A shadow never updates from a local request. It updates from an echo
and from STATE.

The pattern a control follows is desired-and-reported with a pending
state that resolves on echo. Optimistic local state is prohibited: a
user interface that shows a value the machine does not hold is a
safety defect on a machine that moves.

See also: [Ground truth](#ground-truth), [Echo](#echo), [Retained value](#retained-value) · Source: SPEC
{ .ss-termmeta }

### Conflation

**Keeping at most one queued unsent frame per channel and subscriber, so a newer snapshot replaces an older one.**

A subscriber therefore sees the freshest state its link can carry,
never a backlog. Under congestion, conflation tightens: periodic
pushes stretch toward on-change-only.

Conflation is only safe because STATE frames are full snapshots. It
is the direct payoff of banning deltas.

See also: [State](#state), [Shedding](#shedding), [Retained value](#retained-value) · Source: SPEC
{ .ss-termmeta }

### Liveness ping

**A tiny frame a client sends during silence to prove it is still there.**

Any received frame is proof of life. A client streaming motion needs
no separate ping.

A client sending sparse commands does. That is how a segment sender
that emits two packets per second still holds a 600 ms deadman
window: the segments stop during a pause, the pings continue, and
the machine settles instead of tripping.

See also: [Deadman](#deadman), [Idle reaping](#idle-reaping) · Source: SPEC
{ .ss-termmeta }

### Idle reaping

**Closing a non-owning session that has gone silent, to reclaim its slot.**

Two liveness regimes exist, deliberately different. A session that
owns a motion source gets the deadman window and its loss policy. A
session that owns nothing gets idle reaping, with no motion
consequence at all.

Without idle reaping a viewer that went dark held a slot forever.

See also: [Deadman](#deadman), [Liveness ping](#liveness-ping), [Teardown](#teardown) · Source: RFC-024
{ .ss-termmeta }

### Teardown

**The single code path every session end runs, whatever killed the session.**

A voluntary goodbye, a rude disconnect, an administrative eviction,
a slow-consumer eviction, a readiness timeout and a slot reuse all
run the same loss policy.

This is not tidiness. Ownership release used to hang off the deadman
pump alone, which needs an occupied slot. Every other exit path
cleared the slot first, so a departed streamer's dead session owned
the motion channel forever and silently refused every later client
until reboot.

The regression test for anything touching session lifecycle is two
back-to-back sessions **with no reboot between them**.

See also: [Session](#session), [Deadman](#deadman), [Active source](#active-source) · Source: RFC-005, SPEC
{ .ss-termmeta }

### Eviction

**The hub closing a session it has decided it cannot keep.**

A slow consumer is evicted when even its never-shed queue cannot
drain. One incurable client must not consume hub memory or airtime
indefinitely.

A configure session may also evict another session administratively.

See also: [Teardown](#teardown), [Shedding](#shedding), [Configure](#configure) · Source: SPEC, RFC-018
{ .ss-termmeta }

### Synthetic session

**A hub-side session object that wraps a legacy text-protocol edge so it obeys the same rules as a native client.**

A TCode edge is not a SlopSync client and receives no SlopSync
frames. The hub still wraps it in a session: it appears in the
roster, it owns its arbiter source, and it carries the deadman its
existing silence timeout implies.

The point is a rule, not tidiness: there is no unmonitored path to
motion. A legacy transport that could move the machine outside the
safety machinery would be a hole in it.

See also: [Session](#session), [Deadman](#deadman), [Source ownership](#source-ownership) · Source: SPEC
{ .ss-termmeta }

## The data plane

Motion data is high-rate, loss-tolerant and never acknowledged. These are the words for how it is packed, paced and dropped.

### Bundle

**One STREAM frame carrying a base timestamp and up to 32 samples.**

A bundle's samples carry offsets from the base timestamp. The
offsets increase strictly, start at zero, and span at most 20 ms.

A bundle that violates any cap is dropped **whole**. It is never
parsed half-way.

See also: [Sample](#sample), [Segment](#segment), [Stream](#stream) · Source: SPEC
{ .ss-termmeta }

### Sample

**One STREAM data point reporting a value at an instant.**

A dropped sample is recoverable by interpolating from its
neighbors. That is why sample channels may be decimated under
congestion.

See also: [Segment](#segment), [Stream kind](#stream-kind), [Shedding](#shedding) · Source: SPEC, RFC-014
{ .ss-termmeta }

### Segment

**One STREAM data point that commands a time extent, carrying its own duration.**

A segment is not a point on a continuous curve. A dropped segment is
a permanently lost **command**, not a recoverable interpolation gap.

On SlopDrive-32 one segment becomes one quintic waveform command,
which is roughly two to four packets per second for a scripted
session, instead of a dense sample stream.

See also: [Sample](#sample), [Stream kind](#stream-kind), [Quintic](#quintic) · Source: RFC-014
{ .ss-termmeta }

### Token bucket

**The ingress rate limiter, a bucket that refills at the granted sample rate and holds one burst's worth of tokens.**

Enforcement is on samples per second, not bundles per second,
because one bundle batches up to 32 samples.

Each accepted bundle spends its sample count. A bundle that would
overdraw is dropped whole and the session gets a rate-limited NACK.
That NACK is throttled, because it is back-pressure feedback and not
a per-drop echo. Unthrottled it would mirror the very flood it
reports.

See also: [Burst](#burst), [Publish grant](#publish-grant), [Bundle](#bundle) · Source: SPEC
{ .ss-termmeta }

### Burst

**The token bucket's capacity in samples, declared separately from the rate.**

Burst exists because rate used to double as bucket depth. A sender
emitting two to four segments per second with a 25 per second peak
had to declare 30 Hz, lying to admission control to buy headroom.

Burst is clamped to a multiple of the granted rate and echoed like
every other wish. An unbounded client-declared burst would
reintroduce the flood the bucket exists to stop.

See also: [Token bucket](#token-bucket), [Publish grant](#publish-grant) · Source: RFC-013
{ .ss-termmeta }

### Shedding

**What the hub drops, and in what order, when a link cannot carry everything granted.**

The order is fixed: decimate STREAM lowest-priority-first, conflate
STATE harder, drop oldest from bounded EVENT queues, then evict a
subscriber whose never-shed queue itself cannot drain.

Shedding is newest-biased and never delays-and-bursts. A stale
motion sample is worse than a missing one: the timestamps make a
dropped sample recoverable, while a late delivery is a lie.

Segment-class channels are never decimated. They shed whole-source
or not at all.

See also: [Conflation](#conflation), [Priority class](#priority-class), [Never-shed set](#never-shed-set), [Stream kind](#stream-kind) · Source: SPEC, RFC-023
{ .ss-termmeta }

### Priority class

**The shedding rank of a subscription, from background up to critical.**

The lower number sheds first. `background` goes first, then `normal`,
then `elevated`. `critical` is the never-shed set.

See also: [Never-shed set](#never-shed-set), [Shedding](#shedding), [Grant](#grant) · Source: SPEC
{ .ss-termmeta }

### Never-shed set

**The traffic that is never dropped under congestion, intents, echoes, NACKs, grants, ESTOP, and safety state.**

Never-shed traffic is tiny by design. If even that cannot drain for
two seconds, the subscriber is broken and is evicted.

ESTOP is exempt from even that: it is written ahead of every queue
at the binding layer, and it is 12 bytes. A link that cannot carry
12 bytes is a dead link.

See also: [Shedding](#shedding), [ESTOP](#estop), [Priority class](#priority-class) · Source: SPEC
{ .ss-termmeta }

## Control and arbitration

Two clients may hold the same access level and still not both drive the machine. Arbitration is the layer that decides which one does.

### Active source

**The motion arbiter input currently driving motion.**

Sources are things like manual jogging, a TCode transport, the
on-hub pattern generator, and a paired remote. The arbiter
arbitrates *between* source types by priority.

See also: [Arbiter](#arbiter), [Source ownership](#source-ownership), [Takeover](#takeover) · Source: SPEC
{ .ss-termmeta }

### Arbiter

**The single component that commands the motor driver for positioning.**

Nothing else calls the driver. Manual input, transports, the pattern
engine and remotes all submit intents to the arbiter, which owns
arbitration, limit-set selection and every safety gate.

SlopSync submits through the arbiter like everything else. The
protocol never bypasses the sole-caller rule.

See also: [Active source](#active-source), [Source ownership](#source-ownership) · Source: SPEC
{ .ss-termmeta }

### Source ownership

**The rule that each arbiter source has at most one owning session at a time.**

The first authorized session to activate a source owns it. A second
session's activating intent is refused with a source conflict.

For an inbound motion STREAM, the **first accepted bundle** acquires
the source, and every later accepted bundle refreshes the deadman
window. A bundle from a non-owner is dropped: data-plane bundles
carry no takeover flag, so a would-be taker must acquire through an
intent first.

See also: [Takeover](#takeover), [Deadman](#deadman), [Active source](#active-source) · Source: SPEC
{ .ss-termmeta }

### Takeover

**Re-issuing an activating intent with the takeover flag set, to transfer source ownership.**

Ownership transfers if the requester's role is at least the
incumbent's. The hub emits a takeover event and updates the
control-owner state.

The dispossessed session's interface must reflect loss of control
immediately. It is subscribed to the control-owner channel like
everyone else, so it finds out the same way.

See also: [Source ownership](#source-ownership), [Active source](#active-source) · Source: SPEC
{ .ss-termmeta }

### Precondition

**The expected cfg_gen a client attaches to an intent, turning it into a compare-and-swap.**

A mismatch returns NACK `CONFLICT`. The client re-reads and retries.
This is how a client turns "absolute values only" into a safe
read-modify-write.

See also: [Cfg_gen](#cfg_gen), [Intent](#intent) · Source: SPEC
{ .ss-termmeta }

### Idempotency ring

**The hub's per-session record of the last 32 intent ids and the echoes they produced.**

A duplicate intent id re-emits the stored echo and does not re-apply
the intent. The ring dies with the session, which is safe precisely
because intents carry absolute values.

See also: [Intent](#intent), [Echo](#echo), [Session](#session) · Source: SPEC
{ .ss-termmeta }

## Safety

Safety outranks authorization. You may always stop the machine. You may not always start it.

### ESTOP

**Emergency stop, an immediate driver-level stop that latches and prohibits motion until explicitly cleared.**

Any endpoint may initiate an ESTOP, at any role, in any session
state, including with no session at all.

**The latch is the acknowledgement.** There is no ESTOP-ACK frame.
The initiator repeats the frame until it observes the safety state
with the ESTOP bit latched, or it exhausts retries and surfaces a
loud local failure.

Preemption is a per-hop guarantee, not magic end-to-end latency.
Bytes already in flight still drain first. **The hardware e-stop
path remains the guarantee of last resort. The protocol's ESTOP is
a software convenience layered above it, never a substitute.**

See also: [Latch](#latch), [Stop](#stop), [Safety cause](#safety-cause) · Source: SPEC
{ .ss-termmeta }

### Stop

**A controlled stop, decelerate to zero at the configured rate and deactivate the source.**

A stop clears on the next motion intent from an authorized source.
It is the deadman's default consequence, and it is role-exempt: any
session may send it.

See also: [ESTOP](#estop), [Hold](#hold), [Pause](#pause), [Deadman](#deadman) · Source: SPEC
{ .ss-termmeta }

### Hold

**Decelerate, then actively hold position, with the source suspended.**

A hold clears on a resume intent from the owning session.

See also: [Stop](#stop), [Pause](#pause) · Source: SPEC
{ .ss-termmeta }

### Pause

**Suspend the pattern generator at a safe phase and park position.**

A pause clears on a resume intent.

See also: [Stop](#stop), [Hold](#hold) · Source: SPEC
{ .ss-termmeta }

### Latch

**A safety condition that stays true in state until something explicitly clears it.**

Latched safety state survives every reconnect. Every arriving client
adopts it before it can act.

Clearing an ESTOP latch never restarts motion. It only re-arms the
ability to start.

See also: [ESTOP](#estop), [Retained value](#retained-value), [Event](#event) · Source: SPEC
{ .ss-termmeta }

### Deadman

**The silence window bound to an active source, after which the source's loss policy fires.**

The deadman binds to the **active source**, not to sessions in
general. Silence longer than the window fires the policy.

An initiator-bound source, such as a motion stream or manual
jogging, defaults to STOP. The machine must not keep executing a
stream whose author is gone.

A hub-autonomous source, such as the on-hub pattern generator,
defaults to continue with ownership released. The vanished client
was merely the finger that pressed start; a phone screen-lock must
not interrupt a self-driving session.

There is no unmonitored path to motion. Legacy text-protocol edges
get synthetic sessions with equivalent timeouts.

See also: [Safety cause](#safety-cause), [Source ownership](#source-ownership), [Teardown](#teardown), [Liveness ping](#liveness-ping) · Source: SPEC
{ .ss-termmeta }

### Safety cause

**Why a stop or an e-stop is latched, user, deadman, fault, relay, or session loss.**

`deadman` means the silence window actually elapsed. Every other
way a session ends latches `session_loss`.

The distinction is not cosmetic. Before `session_loss` existed, an
operator closing a browser tab was reported as a deadman **timeout**
in every subscriber's interface and log line.

See also: [Deadman](#deadman), [Teardown](#teardown), [ESTOP](#estop) · Source: RFC-022
{ .ss-termmeta }

### Ground truth

**The doctrine that a client never displays machine state that differs from the device's, in either direction.**

Page load adopts device state. It never pushes defaults onto a live
session. Echoes report applied post-clamp values from the driver,
never pre-clamp requests. Optimistic local state is prohibited.

A control that renders but drives nothing is a defect. **An
interface that lies about machine state is a safety defect on a
machine that moves.**

See also: [Shadow](#shadow), [Echo](#echo), [Retained value](#retained-value), [Clamp](#clamp) · Source: SPEC
{ .ss-termmeta }

### Clamp

**Replacing a requested value with the nearest value the machine's limits allow.**

Limits are ceilings, never targets. A client may ask for anything;
the hub applies what is safe and reports what it applied.

Clamping is why the echo exists. A request of 420 against a ceiling
of 400 is not an error and gets no error: it is applied as 400, and
400 is the number every screen then shows, including the screen
that asked for 420.

See also: [Echo](#echo), [Ground truth](#ground-truth), [Intent](#intent) · Source: SPEC
{ .ss-termmeta }

## Trust and pairing

The v1 threat model is casual and drive-by prevention on a trusted local network. Where a defense is weak, the specification says so plainly rather than implying strength it does not have.

### Pairing

**The ceremony that proves a client should be trusted, ending in a token and a role.**

Three modes exist: knock-and-approve, PIN proof, and push-to-pair.
All three end in the same grant shape. The role is an attribute of
the grant, never of the ceremony.

See also: [Knock-and-approve](#knock-and-approve), [Token](#token), [Role](#role) · Source: RFC-027
{ .ss-termmeta }

### Knock-and-approve

**The primary pairing mode, a client knocks with no proof, and any configure session approves it.**

The knock lands in a bounded pending list that is exposed as
ordinary protocol state, so any configure client can render it and
approve or deny.

The joiner needs one button and no display. The approval surface
shows the knocker's identity on hardware the attacker does not
control.

It also kills the circular dependency where a built-in web interface
is trusted because it is the built-in web interface. The trusted
surface is *any* configure client.

The pending list is bounded because it is an unauthenticated queue,
the one surface a stranger can fill.

See also: [Pairing](#pairing), [Configure](#configure), [Pending pairing](#pending-pairing) · Source: RFC-027
{ .ss-termmeta }

### Pending pairing

**The bounded list of unanswered pairing knocks, published as ordinary protocol state.**

Because it is state and not a private hub structure, any configure
client can render the queue, and a late-joining client learns the
current contents without needing missed events.

See also: [Knock-and-approve](#knock-and-approve), [State](#state) · Source: RFC-027
{ .ss-termmeta }

### Token

**The 16-byte secret a paired client presents to claim its granted role.**

A client presents it in one of two modes. **Bearer** puts the raw
bytes in the handshake; it is legal and keeps the minimum client a
memory copy. **Proof** sends an HMAC over the hub's nonce in a
separate frame; it costs one extra round trip.

v1 transports are cleartext, so a bearer token is sniffable by a
passive observer on the same network. The trust ledger records which
mode a device uses, which makes security posture visible instead of
assumed.

See also: [Trust ledger](#trust-ledger), [Pairing](#pairing), [Role](#role) · Source: RFC-029
{ .ss-termmeta }

### Trust ledger

**The hub's stored list of paired devices, their roles, and how each one presents its token.**

The ledger is a store, not a packed roster: an entry does not fit a
242-byte snapshot. It follows the standard store shape: a static
descriptor plus a tiny roster state whose generation bump means
"re-enumerate".

Only a configure session may read it. A paired-device list is not
open reading.

See also: [Store](#store), [Token](#token), [Configure](#configure) · Source: RFC-027, RFC-029
{ .ss-termmeta }

### Change tripwire

**The rule that an observed client-version change drops a paired device to recognized-pending until re-approved.**

A recognized-pending device is admitted at `watch`; its granted role
is suspended and re-approval is surfaced to configure sessions.

**Honesty clause:** the version is self-reported. This is a
tripwire, not attestation. A deliberately malicious update lies and
keeps its token. The real bounds are role scoping, instant
revocation, roster visibility, and the role-exempt safety
operations.

See also: [Trust ledger](#trust-ledger), [Watch](#watch) · Source: RFC-029
{ .ss-termmeta }

### Welcome signature

**An optional hub signature over client-supplied entropy, proving you reached the machine you paired with.**

The hub signs the client's nonce with the session id and boot id.
Client entropy is mandatory to the scheme: without it, a captured
handshake replays and an evil twin passes verification.

Signing is on request, because software elliptic-curve signing costs
tens of milliseconds and must never sit inline in a socket handler
by default. A client paired by physical ceremony may skip
verification entirely.

See also: [Pairing](#pairing), [Token](#token) · Source: RFC-029
{ .ss-termmeta }

### Push-to-pair

**The pairing mode where physical presence, rather than a proof or an approval, opens a short single-grant window.**

The specification requires the **proof of presence**, never a
particular button. The minimum hardware is none, because the power
cord is the button.

A factory-fresh hub holding no configure token boots claimable, and
the first knock gets `configure`. Whoever unboxed the machine and
powered it possesses it. Later, three consecutive boots that each
last under about ten seconds open the window on the next boot. That
gesture needs one counter in non-volatile storage and cannot
collide with a live session, because any power loss already stops
motion and forces a re-home.

A hub with a real button may bind it instead. That is a usability
upgrade, never a requirement. Factory reset must be a deliberately
harder gesture than opening pairing.

See also: [Pairing](#pairing), [Knock-and-approve](#knock-and-approve), [Configure](#configure) · Source: RFC-027
{ .ss-termmeta }

### Served-page token

**A single-use token the hub mints for its own served web page, so that page reaches control without a pairing ceremony.**

The page fetches the token from the machine that served it. The
endpoint sets no cross-origin headers, so the browser's same-origin
policy is the boundary: a page from anywhere else can send the
request and cannot read the answer. That closes the browser-borne
class of attack, which is the automatable one.

The caps are deliberate. It grants `control`, never `configure`.
Configure always pairs. A native process on the same network can
request the token. That attacker already defeats the cleartext
ceiling, so nothing is newly lost.

**It is never a prerequisite.** A web interface with no token
endpoint is an ordinary client: watch by default, and control or
configure through any pairing mode.

See also: [Pairing](#pairing), [Token](#token), [Control](#control) · Source: RFC-029
{ .ss-termmeta }

## Motion vocabulary

SlopSync carries motion, so it inherits motion words. These are defined here rather than softened, because they are precise and implementers need them.

### Quintic

**A fifth-order polynomial trajectory matched to position, velocity and acceleration at both ends.**

Matching all three at both ends makes the joins continuous in
acceleration, so the boundary jerk spikes of a lower-order fit
disappear.

On SlopDrive-32 each commanded segment becomes exactly one quintic
over the commanded duration, which reproduces the sender's own
spline rather than approximating it with a stretched
accelerate-cruise-decelerate profile.

See also: [Segment](#segment), [Jerk](#jerk) · Source: Roadmap
{ .ss-termmeta }

### Jerk

**The rate of change of acceleration.**

A jerk ceiling is what makes a move feel smooth rather than merely
fast. It is a ceiling, never a target.

See also: [Quintic](#quintic), [Limit set](#limit-set)
{ .ss-termmeta }

### Limit set

**A named group of speed, acceleration and jerk ceilings the arbiter selects per source.**

Manual input uses the user limit set. Everything machine-driven
(patterns, streams, transports) uses the input limit set.

**Limits are ceilings, never targets.** A plan derives its speed
from what the intent requires, then clamps at the ceiling.

See also: [Arbiter](#arbiter), [Stroke window](#stroke-window), [Jerk](#jerk) · Source: SPEC, RFC-006
{ .ss-termmeta }

### Stroke window

**The operator-set lower and upper position bounds that motion stays inside.**

Limits normalized against the window are window-relative, so they
move when the window moves. That is exactly why the window is
published as state and not handed out once at connect time.

See also: [Limit set](#limit-set), [Field role](#field-role) · Source: RFC-006
{ .ss-termmeta }

### Waveform

**The planner mode that turns one timed segment into one quintic over exactly the commanded duration.**

A sender that knows where the motion goes and how long it may take
sends a segment. The planner reproduces that shape rather than
approximating it, and the deadline is met by construction.

A segment the machine cannot serve, too fast or outside the
window, falls back to a point-to-point plan and raises a
[motion anomaly](#motion-anomaly) saying so.

See also: [Segment](#segment), [Quintic](#quintic), [Chase](#chase), [Motion anomaly](#motion-anomaly) · Source: Roadmap
{ .ss-termmeta }

### Chase

**The planner mode that replans to each newly arrived point from the machine's own sampled state.**

A stream of bare points carries no duration, so there is no shape to
reproduce. The planner replans per point instead, from live position
and velocity.

A dense stream gets predictive aim: the plan targets where the stream
will be, at the velocity the stream is running, rather than the point
that already arrived. Chasing the newest stale point costs several
intervals of lag.

See also: [Sample](#sample), [Waveform](#waveform), [Limit set](#limit-set) · Source: Roadmap
{ .ss-termmeta }

### Asked

**The demand as it arrived over the wire, mapped into the stroke window, before the planner shaped it.**

Asked is the sender's own number. It is not a request the machine has
agreed to, and it is not clamped yet.

See also: [Planned](#planned), [Achieved](#achieved), [Stroke window](#stroke-window) · Source: Roadmap
{ .ss-termmeta }

### Planned

**The position the motion core is driving to right now, after arbitration, clamping and the window.**

Planned is the machine's own promise. It has already crossed the
boundary from intent to accepted, which is why the machine failing to
reach it means something different from the machine not going where
the app asked.

See also: [Asked](#asked), [Achieved](#achieved), [Clamp](#clamp) · Source: Roadmap
{ .ss-termmeta }

### Achieved

**Where the carriage actually is, as measured and reported by the machine.**

Achieved is the only one of the three positions that is measured
rather than computed. It is [ground truth](#ground-truth) for
position.

See also: [Asked](#asked), [Planned](#planned), [Ground truth](#ground-truth) · Source: Roadmap
{ .ss-termmeta }

### Motion anomaly

**A device-authored event naming what the motion core had to do differently, and why.**

Anomalies are not errors. They are the planner saying it clamped an
end velocity, stretched a duration to a deadline, fell back from a
waveform, or braked to rest with no fresh command.

They are the fastest explanation of a trace that looks wrong: the
count tells you the machine is being asked for something it cannot
serve, and the kind tells you which ceiling it met.

See also: [Waveform](#waveform), [Chase](#chase), [Event](#event), [Clamp](#clamp) · Source: Roadmap
{ .ss-termmeta }

## Tooling

Three programs do the work nobody should do by hand: prove a hub, pretend to be a machine, and show what the motion actually did.

### SlopScope

**The motion-pipeline oscilloscope, which graphs asked against planned against achieved.**

SlopScope subscribes at the [watch](#watch) tier and nothing else. It
sends no [intent](#intent), publishes no [stream](#stream), and
carries no publish wish. It is structurally unable to command motion.

It resolves every series from the catalog it is served: by
[field role](#field-role) where one exists, by declared field names
otherwise. It graphs a hub whose channel numbers are not this
firmware's.

See also: [Scope trace](#scope-trace), [Asked](#asked), [Planned](#planned), [Achieved](#achieved), [Watch](#watch) · Source: Roadmap
{ .ss-termmeta }

### Scope trace

**A self-describing JSONL capture of one motion session, plus the header that makes it readable forever.**

The header carries the catalog [etag](#etag), the firmware version,
the [stroke window](#stroke-window), the [limit set](#limit-set), and
the full layout of every recorded channel: names, units, scales,
roles and option labels.

So a trace stays interpretable months later, against a firmware that
has since changed, and rendering it is a pure offline function of the
file. A trace on a bug report is evidence, not an anecdote.

See also: [SlopScope](#slopscope), [Etag](#etag), [Limit set](#limit-set) · Source: Roadmap
{ .ss-termmeta }

### Probe

**The reference verifier, which runs a scripted session against a hub and prints a pass-or-fail transcript per stage.**

The probe hand-rolls its own encoder against the
[registry](#registry) instead of importing the library. A hub that
passes it has therefore agreed with an independent implementation,
not with itself.

It is also a complete, readable v1.0 client, short enough to read in
one sitting.

See also: [Simulator](#simulator), [Golden vector](#golden-vector), [Conformance profile](#conformance-profile) · Source: SPEC
{ .ss-termmeta }

### Simulator

**A desktop binary that behaves like a machine, embedding the real hub, motion engine and catalog behind a real transport.**

It is not a mock. A bug found against the simulator is a bug in the
same code the device runs.

Its actuator is the honest exception: an ideal follower with no step
quantisation, current limit, encoder lag or compliance. Trust it for
protocol, planning and shaping. Confirm tracking numbers on hardware.

See also: [Probe](#probe), [SlopScope](#slopscope), [In-process binding](#in-process-binding) · Source: Roadmap
{ .ss-termmeta }

## Specification and process

The specification is the product. The library is its reference implementation. These words describe how the two stay honest.

### Registry

**The single machine-readable file that decides every number SlopSync puts on the wire.**

Frame types, CBOR keys, NACK codes, channel ids, limits, roles and
categories all live there. The C++ constants and every table on this
site are generated from it.

On any conflict, the registry wins. Released numbers are never
reused and never renumbered.

See also: [Wire number](#wire-number), [Golden vector](#golden-vector) · Source: SPEC
{ .ss-termmeta }

### Wire number

**Any value that appears on the wire and must therefore mean the same thing to every implementation.**

A wire number is never invented in code. When an implementation
needs one the specification lacks, the fix is to add it to the
registry first, regenerate, and then write code against the
generated constant.

See also: [Registry](#registry) · Source: SPEC
{ .ss-termmeta }

### Golden vector

**A byte-exact recorded frame that every implementation must encode and decode identically.**

Determinism is a conformance requirement, which is why time and
randomness are injected rather than read from the platform. A
reference implementation that cannot reproduce a vector byte for
byte has a bug, not a variation.

See also: [Conformance profile](#conformance-profile), [Registry](#registry) · Source: SPEC
{ .ss-termmeta }

### Conformance profile

**A named subset of the specification an implementation may claim to satisfy.**

Profiles exist so a coin-cell remote and a full desktop client can
both be conformant while implementing very different amounts of the
protocol.

See also: [Golden vector](#golden-vector), [Constrained client](#constrained-client) · Source: SPEC
{ .ss-termmeta }

### Fuzz gate

**The continuous-integration job that feeds mutated input to every parser and fails on any memory-safety error.**

Its first campaign ran 2.29 billion executions and found three
memory-safety bugs, all fixed before release. One was a CBOR integer
overflow reachable from every message decoder.

Publishing found-and-fixed bugs is a credibility asset. A parser
that has never been fuzzed is not known to be safe; it is only
untested.

See also: [Conformance profile](#conformance-profile) · Source: RFC-028
{ .ss-termmeta }

### RFC process

**The way a change to SlopSync is proposed, argued and either bound into the specification or refused.**

A number is added by a pull request against the registry, and
appears in the next tagged specification version.

Breaking the wire grammar needs a protocol version bump, which needs
exceptional justification. The intended lifetime of `slopsync/1` is
the lifetime of the hardware.

See also: [Registry](#registry), [Errata](#errata) · Source: SPEC
{ .ss-termmeta }

### Errata

**A correction to a published specification version that does not change the wire grammar.**

Errata fix wording, close ambiguities and record clarifications.
They never renumber anything.

See also: [RFC process](#rfc-process)
{ .ss-termmeta }

### Parser totality

**The requirement that every parser maps any byte string to accept or reject, with no crash in between.**

No out-of-bounds read. No unbounded allocation. No unbounded
recursion. No undefined behavior. The obligation is symmetric: a
hostile hub must not be able to crash a conforming client, exactly
as a hostile client must not be able to crash a hub.

Golden vectors prove correctness. Totality is a different property,
and the fuzz gate is what proves it.

See also: [Fuzz gate](#fuzz-gate), [Golden vector](#golden-vector), [Conformance profile](#conformance-profile) · Source: RFC-028
{ .ss-termmeta }

### In-process binding

**The transport binding that connects a hub and a client inside one process, with injected faults and a deterministic mode.**

It is a conformance instrument, not a convenience. It must support
a configurable MTU, injected loss, reorder and duplication,
injected latency and jitter, and a seeded mode in which a run
reproduces bit for bit.

The behavioral checklists run against it. An implementation with
no fault injection cannot claim to have tested conformance.

See also: [Golden vector](#golden-vector), [Conformance profile](#conformance-profile) · Source: SPEC
{ .ss-termmeta }

