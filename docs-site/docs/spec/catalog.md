---
title: Catalog
description: >-
  SlopSync clause 8: the channel entry, the schema language, etag computation,
  blob transfer, the static-client profile, stores, and the settings
  metamodel.
register: IEEE
generated: true
---

<!-- ==========================================================
     GENERATED FILE — DO NOT EDIT.
     Source of truth: docs/slopsync/SPEC.md
     Generator:       docs-site/tools/gen_spec_pages.py
     Regenerate:      python docs-site/tools/gen_spec_pages.py
     CI gate:         python docs-site/tools/gen_spec_pages.py --check
     Normative text is copied verbatim. Hand edits are overwritten
     and fail the docs build. Edit the specification instead.
     ========================================================== -->

# 8. Catalog *(normative)* {#s8}

The catalog is the hub's machine-readable self-description. It is the load-bearing artifact of the whole protocol: everything a generic client knows about a hub it learns here, and every "how does a client discover X" question in this document resolves to "the catalog says".

## 8.1 The channel entry {#s8-1}

The catalog is an array of channel entries. The **normative encoding is CDDL-defined** in [`schema/catalog.cddl`](schema.md) ([Appendix C](appendices.md#appendix-c)); this section is its prose companion and the CDDL wins on any disagreement.

An entry carries: `id` (u16), `name`, `class` (STATE/STREAM/INTENT/EVENT/STORE), `dir` (h2c/c2h), `access` (the **floor** tier required to subscribe, or for INTENT to send), `max_rate_hz` (f32; 0.0 = on-change only), `default_priority`, and **exactly one** of:

- `layout` — an ordered array of packed fields, for STATE and STREAM;
- `schema` — a map of integer key → field, for INTENT (the fields of `value`) and EVENT (the fields of `body`);
- `store` — a store descriptor, for STORE ([§8.7](#s8-7)).

Plus these optional entry-level keys:

| Key | Meaning |
|---|---|
| `category` | `setting_categories` value; 0–127 registered, 128–255 device-defined |
| `category_label` | REQUIRED iff `category ≥ 128` |
| `replay_depth` | entries the hub MAY replay on grant — presence is **the** exception to [§9.4](channels.md#s9-4)'s no-replay rule |
| `setting_channel` | the u16 INTENT channel that writes this entry's setting-annotated fields; REQUIRED iff any field carries `setting_key` |
| `stream_kind` | `stream_kinds` value; STREAM class only; **absent means `samples` (0)** |

**Encoding structure rule.** The catalog on the wire is its outer array header followed by each entry encoded as an independent, self-delimiting document. Every entry document individually satisfies the [§5.3](wire-format.md#s5-3) depth-4 cap; decoders MAY — and depth-4 decoders MUST — process entries one at a time with per-entry decoder state. The etag ([§8.3](#s8-3)) is computed over exactly these concatenated bytes.

**Entry size bound.** A single encoded entry MUST NOT exceed `catalog_max_entry_bytes` (4096). A fully-annotated 50-field entry can encode to 8–10 KB, which would violate [§5.8](wire-format.md#s5-8)'s no-unbounded-allocation rule for a per-entry decode buffer. Oversize is a **catalog-authoring error** caught by conformance tooling, not a runtime surprise: the author splits the entry across channels or trims descriptions. Total entries are bounded by `catalog_max_entries` (256), a conformance floor rather than a wire cap.

**Depth budget, stated as a design constraint.** Counting from an entry map: `entry → layout array → field map → options array` = 4, at the cap; likewise `entry → layout → field → bits` and `entry → schema → field → options`. The leaves of those containers are scalars by construction. **Any future annotation that wants a map or array *inside* a field map is therefore blocked and must ride the entry level instead.** This is a real constraint, not a formality.

## 8.2 Schema language scope {#s8-2}

The layout/schema vocabulary is deliberately small: fixed-width numeric types and fixed-width strings (`packed_field_types`), a scale factor (wire = physical × scale), short unit strings (`mm`, `mm/s`, `mA`, `degC`, `%`, `count`, `flag`, ""), min/max, and the [§8.8](#s8-8) annotation block. It describes *values and their meaning*, not behavior and not appearance. Nesting, variable-length fields, and conditionals are out of scope by design; a channel that seems to need them is two channels.

## 8.3 Etag computation {#s8-3}

`catalog_etag` = the first `etag_bytes` (8) of SHA-256 over the catalog encoded in the [§5.3](wire-format.md#s5-3) deterministic profile, entries sorted ascending by `id`. Deterministic encoding makes the hash reproducible from the catalog *content* alone — any implementation, any language, same bytes, same etag.

The etag covers everything in [§8.1](#s8-1): ids, names, classes, directions, access, rates, priorities, layouts, schemas, store descriptors, and every annotation. It does **not** cover retained *values* — that is `cfg_gen`'s and seq's job.

Because the etag covers annotations, adding a tooltip changes it. That is correct and intended: a client caching by etag would otherwise render a stale label forever.

## 8.4 Transfer: the catalog is blob namespace 0 {#s8-4}

Chunked transfer is **one verb for the whole protocol** ([§8.7](#s8-7)). The catalog is simply blob namespace 0.

- **BLOB_REQ** (`0x1A`, c2h, CBOR): `blob` (38) selects what — for the catalog, `ns = 0` and no `store_id`/`slot`. An empty selection means "send everything"; `chunks` (27) at the top level makes it a **selective repair** request listing missing indices. **Carrying both a full request and `chunks` is MALFORMED** and MUST be rejected.
- **BLOB_CHUNK** (`0x1B`, h2c, raw): a fixed header naming the same identity fields as `blob_keys` (namespace, store, slot, generation, `chunk_index`, `chunk_count`, `total_bytes`), followed by up to `catalog_chunk_payload` (192) bytes of the deterministic encoding. 192 fits every binding unfragmented; WS MAY carry multiple chunks back-to-back.
- The receiver reassembles by index, requests missing indices after `catalog_chunk_gap_timeout_ms` (500 ms, SHOULD), and abandons after `frag_reassembly_timeout_ms` (5 s) total — then either retries from scratch or falls back to the static profile ([§8.5](#s8-5)). `total_bytes` lets a receiver size or refuse a transfer **before** assembling it ([§5.8-1](wire-format.md#s5-8)).
- A hub bounds concurrent transfers by its RAM; beyond that, BLOB_REQ gets NACK `BUSY`. A request naming a namespace, store or slot that does not exist gets NACK `CHUNK_UNAVAILABLE`.
- The hub MUST gate BLOB_REQ on the declaring entry's `access` exactly as it gates SUBSCRIBE.

**Only the catalog namespace has a readiness concept** ([§6.4](session.md#s6-4)). You cannot decode STATE without the catalog; nothing gates on a preset.

## 8.5 The static-client profile (etag-pinned) {#s8-5}

A constrained client MAY ship with a **compiled-in catalog** and pre-encoded CBOR templates instead of a CBOR stack. Requirements:

- It sends its compiled-in etag in HELLO. If the hub's etag matches: full speed ahead, ready immediately.
- On mismatch it MUST choose a **declared** behavior: **(a)** proceed **degraded** — the [§5.4](wire-format.md#s5-4) append-only rule guarantees its known prefix of every layout still parses; it MUST suppress any *control* function whose schema it cannot re-verify; or **(b)** refuse with a user-visible "update me" indication. **Silent full operation on a mismatched etag is non-conformant.**
- A degraded client sends CATALOG_READY with its stale etag ([§6.4](session.md#s6-4)).
- The hub treats static clients identically to dynamic ones; the profile is client-internal except for the etag check. NACK `ETAG_MISMATCH` exists for hubs configured to refuse degraded operation outright — a hub policy, not the default.

## 8.6 Catalog invariance and mid-session change {#s8-6}

The catalog is **client-invariant**: every session sees the same entries and the same etag. Access control acts at SUBSCRIBE/PUBLISH/INTENT/BLOB_REQ time (NACK `ACCESS_DENIED`), **never** by filtering the catalog. Per-client catalogs would fracture etag caching and static profiles, and would make a generic renderer's "grey, never hide" rule ([§8.9](#s8-9)) impossible to honor.

**Settings are fixed per firmware.** The set of channels and fields is enumerated at connect and is never created or destroyed at runtime. A change is a catalog change, which changes the etag, which is already the resync trigger. Mid-session catalog change is signaled by the `catalog` channel's STATE update ([§4.2-3](foundations.md#s4-2)); clients re-enter SYNCING.

## 8.7 STORE channels and the blob verb {#s8-7}

A **STORE**-class catalog entry declares a collection of slot-addressed items: `{store_id, kind, capacity, per_item_max, name_max}`. `kind` is a namespaced string (e.g. `"pattern.frayd"`, `"trust.ledger"`). Presets, saved positions, limit profiles, recordings and the trust ledger are all the same machinery.

- **Why an ordinary catalog entry:** a parallel top-level array would break the catalog root shape, the id sort, the etag computation and the per-entry depth rules — all four.
- **The dynamic half is a separate tiny STATE channel** carrying `{generation, count, capacity}`, on-change and retained. A generation bump means "re-enumerate". This keeps the catalog invariant per firmware ([§8.6](#s8-6)) while the roster changes freely. Every store in the protocol is this pair of entries.
- **Items** are `{slot, name, kind, payload}` and move over BLOB_REQ/BLOB_CHUNK with `ns = 1 (store)`, `store_id` selecting the store and `slot` the item.
- **`payload` is OPAQUE.** The protocol layer never decodes it, and [§5.8](wire-format.md#s5-8)'s depth and allocation budget explicitly does not extend inside it. A client that decodes a preset has stepped above the protocol boundary.
- **CRUD rides INTENT** on a device-declared channel: `save` (the hub captures **current live state** by default; a client MAY supply a `payload`, which is an *import*), `load` (the hub applies; the resulting truth arrives via the normal STATE broadcasts — ground truth, no special echo), `delete`, `rename`. The hub validates `kind` and size on import and NACKs `INVALID_VALUE`; it never inspects the payload.
- **Caps are hub-declared with generous spec floors:** `capacity ≥ preset_capacity_min` (32) as a conformance floor, `per_item_max` defaulting to `preset_item_max_bytes` (4096). Small hubs declare less and the catalog says so.

**The one carve-out.** The store whose `kind` is `"trust.ledger"` has a **registered item grammar** (`trust_ledger_keys`, [§12.6](security.md#s12-6)). Presets are device content and are genuinely opaque; the trust ledger is *protocol* content whose fields this document names, which every `configure` client must render and act on, and where "revoke device 3" has to mean the same thing on every hub. The store *machinery* is reused verbatim — chunking, repair, generation, caps, all free — and only this one store's payload grammar is agreed centrally. Opacity is the default and stays the default.

## 8.8 The settings metamodel *(the annotation block)* {#s8-8}

Every layout and schema field MAY carry an annotation block. **All of it is optional and all of it is ignorable**: a client that reads none of it behaves exactly as a v1-draft client did. A client that reads it can build its entire settings and control surface from the hub — a control added in firmware populates on every client's next connect, with its label, grouping, units, constraints and explanation coming from the machine rather than from each client developer's guesswork.

| Annotation | Applies to | Meaning |
|---|---|---|
| `setting_key` | layout fields | the CBOR key in the entry's `setting_channel` that **writes** this field. **Present = this field is a setting (stored config): adopt it into a control. Absent = read-only** (effective state or telemetry): display it, and **never** write it back into a setting's shadow |
| `default` | both | the factory value, same type as the field |
| `options` | both | labels for a single-select; **the wire value is the array index** |
| `group` | both | a free-form card heading within the category tab |
| `desc` | both | user-facing description, ≤ `desc_max_bytes` (128). Flash-resident on the hub, travels once, etag-cached |
| `role` | both | a `field_roles` string — see below |
| `step` | both | range granularity hint |
| `flags` | both | `setting_flags` bitmask: `advanced`, `restart_required`, `secret` |
| `access` | **schema fields only** | per-**op** minimum tier; overrides the entry's `access` floor upward or downward |
| `option_access` | **schema fields only** | per-**option** minimum tier, index-aligned with `options` |

**`setting_key` presence is the stored-vs-effective distinction**, and it needs no separate flag. A machine's stroke window may lawfully report a *stored* configured value on the write plane and a different *effective* value on the state plane — on an unhomed machine, for example, stored `[5, 495]` and effective `[0, max_rail]` are both true. A client that adopts the effective value into the stored control stomps operator input; the presence test is what tells it not to.

**`access` and `option_access` are schema-field annotations only.** A layout field is the **read** side — a STATE snapshot value — and *all* write authorization flows through the paired INTENT channel named by `setting_channel` + `setting_key`. A client needing per-option gating resolves that join (which it must do anyway in order to encode a write) and reads `option_access` on the schema field there. This also keeps the field map inside the depth-4 cap, which is already at its limit.

`option_access` exists because an op-style INTENT carries its verb as one **enum-valued field** — the safety-intents channel does exactly this — and per-*field* access cannot vary across the values of one field. Without it, the role-exempt safety ops ([§11.2](safety.md#s11-2)) would force their whole channel down to `watch` access, and a generic renderer would then offer hold/pause/takeover to every watcher, discovering otherwise only by NACK. That violates grey-never-hide.

**Field roles** are the semantic vocabulary that lets a client find a thing on *any* hub without hardcoding a channel number: `limit.user.speed`, `limit.input.jerk`, `window.min`, `telemetry.position`, `identity.name`, `meta.enabled_mask`, `meta.reset_gen`, and so on (registry `field_roles`). Two conventions rather than entries: `<role>.peak` is the peak companion of any telemetry role, and `action.<name>` marks a schema field as a **verb** rather than a value ([§9.3](channels.md#s9-3)).

`role` is a **string**, not a number, because `action.<name>` carries a device-chosen suffix no integer enum could express. Unregistered roles are legal. **Nothing is hardcoded as a requirement; roles are hardcoded as opportunities.** A client that recognizes a registered role MAY upgrade to a bespoke widget; a client that does not MUST fall back to generic rendering. Fallback is mandatory, upgrades are optional, an unknown role is never an error.

**Categories** organize the surface. Entry-level `category` values 0–127 are spec-registered with a canonical order (`device`, `user`, `limits`, `tuning`, `diagnostics`) so placement, iconography and translation are consistent across every hub a client ever meets; 128–255 are device-defined and MUST carry `category_label`, rendered as additional tabs after the spec set. **A category spans channels** — two channels in the same category merge into one tab, which is the answer to a category outgrowing one 242-byte snapshot (about 58 f32 or 115 u16 fields, inclusive of mask bytes).

**Dynamic enablement.** "Greyed out right now" depends on live machine state and therefore cannot live in static metadata at all. A settings STATE channel carries one or more `bitfield8` fields tagged `meta.enabled_mask`; bit *i* gates the *i*-th setting-annotated field of that layout. On-change, retained, conflated — every client greys from the same ground truth.

**Secrets (normative).** A `secret`-flagged field's value **NEVER** appears in STATE. The snapshot carries only a set/unset presence bit. Writes ride the paired INTENT normally, and ECHO confirms application **without echoing the value**. A WiFi password must never ride a retained snapshot that open-access `watch` sessions receive. A secret *string* SHOULD be `str16` or write-only with a presence bit: a `secret str32` burns 13 % of a snapshot to communicate one bit.

**Validation is hub-side.** `min`/`max`/`step`/width are UI hints; the hub is the referee and NACKs `INVALID_VALUE`. There is **no regex requirement on clients** — an optional pattern hint MAY be included and MAY be ignored. A constrained client must never need a regex engine to render a settings page.

**Applied values stay inside advertised ranges.** An ECHO `applied` value, and the value of any `setting_key`-bearing field, MUST lie within that field's declared `min`/`max`. A hub whose internal clamp can exceed its advertised range MUST widen the advertised range, not lie past it — generic renderers depend on it. Read-only *effective* fields lawfully exceed a paired setting's range and declare their own display bounds; that is not a violation, it is the stored-vs-effective distinction doing its job.

## 8.9 Normative rendering checklist *(what a compliant client library means)* {#s8-9}

This is spec text, not wire. A client claiming generic-settings support MUST:

1. build tabs from the spec categories present, in registry order, then device categories by id using their `category_label`;
2. build cards from `group` strings in authoring order; ungrouped fields go to a default card;
3. choose the widget from **type + constraints, never from a hint** — there is no widget field, deliberately: `bool`/u8→toggle, u8 + `options`→select, `bitfield8`→checkbox group, numeric + min/max→slider or numeric entry, `str<N>`→text, **no `setting_key`→read-only display with unit**;
4. order presentation as **authoring order**: tabs in registry order then device ids; within a tab, channels ascending by id and fields in layout order. No ordering metadata is carried on the wire;
5. render a disabled field **grey, never hidden**;
6. surface `desc` through a help affordance appropriate to the form factor;
7. show writes as **pending until ECHO**, and display **applied** values only ([§1.2-1](foundations.md#s1-2) restated for settings);
8. render an unknown role, flag, category, or annotation key **generically** — fallback is mandatory.

A phone renders a range as a slider, a remote as a click-wheel value, a plugin side panel as a numeric box. Same bytes, three honest UIs.
