/**
 * catalog.js — SlopSync blob transfer + catalog decode + packed-STATE decoding.
 *
 * The catalog is the self-describing channel list (§8.1) and, since RFC-009,
 * the self-describing SETTINGS model too. A client fetches it with BLOB_REQ
 * (0x1A) — an empty CBOR map means "the whole of blob namespace 0", the
 * catalog — and the hub streams it back as BLOB_CHUNK (0x1B) raw frames, each
 * carrying a fixed 14-byte identity header (ns/store/slot/reserved/generation/
 * chunk_index/chunk_count/total_bytes) followed by up to 192 catalog bytes.
 *
 * v1.0 RETIRED CATALOG_REQ/CATALOG_CHUNK: transfer is ONE verb for the whole
 * protocol now, with the catalog as namespace 0 and preset/trust stores as
 * namespace 1 (RFC-021).
 *
 * THERE IS NO FALLBACK LAYOUT TABLE, DELIBERATELY. Until v1.0 this file
 * carried `FALLBACK_LAYOUTS`, a hand-copied mirror of THIS device's packed
 * layouts, because retained STATE used to race the catalog transfer. RFC-015's
 * readiness gate removed the race — the hub emits NO data-plane frame to a
 * session that has not declared CATALOG_READY — so a state frame can no longer
 * arrive without its decoder ring. A hand-copied table is exactly the coupling
 * the self-describing catalog exists to prevent (and this one had already gone
 * stale: it described 0x0080 as 7 bytes after the firmware appended raw_10um,
 * silently truncating the "asked" line). Decode from the received catalog,
 * always.
 */

import { cbMap, cbArray, cbUint, cbDecode } from './cbor.js';
import {
  PACKED, PACKED_NAME, PACKED_SIZE, CBOR_FIELD_NAME,
  CHANNEL_CLASS_NAME, DIRECTION_NAME, ACCESS_NAME,
  SETTING_CATEGORY_NAME, SETTING_FLAG,
  K, BLOB_K, BLOB_NS, LIMITS,
} from './frames.js';

// ============================================================================
// BLOB_REQ payload builders (§8.4 / RFC-021)
// ============================================================================

/**
 * Build a BLOB_REQ body.
 *
 * A bare catalog request is the EMPTY CBOR map (0xA0): namespace 0 is the
 * default and store_id/slot are absent by rule, so generalizing transfer cost
 * the common case exactly zero bytes. The `blob` (38) sub-map is emitted ONLY
 * when it says something a default does not.
 *
 * @param {Object} [o]
 * @param {number} [o.ns] BLOB_NS.catalog (default) | BLOB_NS.store
 * @param {number} [o.storeId] store selector (namespace 1 only)
 * @param {number} [o.slot] item index (namespace 1 only)
 * @param {number} [o.generation] roster generation this request is consistent with
 * @param {number[]} [o.chunks] selective-repair indices (omit for a full transfer)
 * @returns {Uint8Array}
 */
export function buildBlobReq(o = {}) {
  const ns = o.ns == null ? BLOB_NS.catalog : o.ns;
  const isCatalog = ns === BLOB_NS.catalog;
  if (isCatalog && (o.storeId != null || o.slot != null)) {
    throw new Error('buildBlobReq: the catalog namespace takes no store_id/slot (§8.4)');
  }
  if (o.chunks && o.chunks.length === 0) {
    throw new Error('buildBlobReq: a repair naming zero chunks is MALFORMED (RFC-022.6)');
  }

  const blobPairs = [];
  if (!isCatalog) blobPairs.push([BLOB_K.ns, cbUint(ns)]);
  if (o.storeId != null) blobPairs.push([BLOB_K.store_id, cbUint(o.storeId)]);
  if (o.slot != null) blobPairs.push([BLOB_K.slot, cbUint(o.slot)]);
  if (o.generation != null) blobPairs.push([BLOB_K.generation, cbUint(o.generation)]);

  const pairs = [];
  if (o.chunks) pairs.push([K.chunks, cbArray(o.chunks.map((i) => cbUint(i)))]); // 27 < 38
  if (blobPairs.length) pairs.push([K.blob, cbMap(blobPairs)]);
  return cbMap(pairs);
}

/** Full transfer of blob namespace 0 (the catalog): the empty map. */
export function buildCatalogRequest() { return buildBlobReq(); }

/** Selective repair of the catalog: {27:[indices]}. @param {number[]} indices */
export function buildCatalogRepair(indices) { return buildBlobReq({ chunks: indices }); }

// ============================================================================
// BLOB_CHUNK parsing + reassembly (§8.4)
// ============================================================================

/** Fixed binary identity header on every BLOB_CHUNK payload (blob_chunks.hpp). */
export const BLOB_CHUNK_HEADER_BYTES = 14;
export const BLOB_CHUNK_PAYLOAD = LIMITS.catalog_chunk_payload; // 192

/**
 * Parse a BLOB_CHUNK raw payload's 14-byte identity header.
 * Layout (little-endian): ns u8 | store_id u8 | slot u8 | reserved u8 |
 * generation u16 | chunk_index u16 | chunk_count u16 | total_bytes u32.
 * The reserved byte is IGNORED, never validated — that is what makes it usable
 * later without a wire break.
 * @param {Uint8Array} payload
 * @returns {?{ns:number,storeId:number,slot:number,generation:number,
 *             chunkIndex:number,chunkCount:number,totalBytes:number,bytes:Uint8Array}}
 */
export function parseBlobChunk(payload) {
  if (payload.length < BLOB_CHUNK_HEADER_BYTES) return null;
  const dv = new DataView(payload.buffer, payload.byteOffset, BLOB_CHUNK_HEADER_BYTES);
  return {
    ns: dv.getUint8(0),
    storeId: dv.getUint8(1),
    slot: dv.getUint8(2),
    generation: dv.getUint16(4, true),
    chunkIndex: dv.getUint16(6, true),
    chunkCount: dv.getUint16(8, true),
    totalBytes: dv.getUint32(10, true),
    bytes: payload.subarray(BLOB_CHUNK_HEADER_BYTES),
  };
}

/** Do two chunk identities name the same blob? `generation` is deliberately excluded. */
export function sameBlobTarget(a, b) {
  return a.ns === b.ns && a.storeId === b.storeId && a.slot === b.slot;
}

/**
 * Receiver-side blob reassembler. Mirrors ChunkReassembler<> in
 * lib/slopsync/.../wire/blob_chunks.hpp: begun from the first chunk's own
 * header (which declares total_bytes, so the size is known before anything is
 * allocated — RFC-028), and REJECTS chunks belonging to a different blob so a
 * catalog transfer and a store fetch cannot corrupt each other.
 */
export class BlobReassembler {
  // 64 chunks (12,288 B) -> 341 chunks (65,472 B), M5c.
  //
  // THIS CAP SILENTLY BROKE THE CLIENT. The device catalog was 11,659 B, i.e.
  // 629 bytes under the old ceiling, and nothing anywhere was watching that
  // margin. Adding the slopmotion tuning cards took it to 15,817 B, the
  // reassembler refused the transfer header, and the session then went LIVE
  // WITH NO CATALOG -- so every STATE frame arrived undecodable and the UI
  // simply showed nothing. No error, no NACK, no dropped-frame warning: a
  // refused blob header just stops.
  //
  // The cap is a DoS guard (a peer declares total_bytes and we allocate it),
  // NOT a statement about how big a legitimate catalog is -- RFC-009's own
  // sizing note projects 15-25 KB for a fully annotated one, which the old
  // value was already below. Sized generously now because a browser can
  // trivially afford 64 KB and being stingy here costs a silent outage.
  constructor(maxTotalBytes = 341 * BLOB_CHUNK_PAYLOAD) {
    this.maxTotalBytes = maxTotalBytes;
    this.reset();
  }

  reset() {
    this.active = false;
    this.target = null;
    this.chunkCount = 0;
    this.totalBytes = 0;
    this.received = null;
    this.receivedCount = 0;
    this.buf = null;
    this.startMs = 0;
    this.lastProgressMs = 0;
  }

  /** Start (or restart) a transfer from a parsed chunk header. */
  begin(h, nowMs) {
    const ok = h.chunkCount > 0 && h.totalBytes > 0 && h.totalBytes <= this.maxTotalBytes;
    this.reset();
    if (!ok) return false; // refuse the NUMBERS too, not just the transfer
    this.active = true;
    this.target = { ns: h.ns, storeId: h.storeId, slot: h.slot };
    this.chunkCount = h.chunkCount;
    this.totalBytes = h.totalBytes;
    this.received = new Array(h.chunkCount).fill(false);
    this.receivedCount = 0;
    this.buf = new Uint8Array(h.totalBytes);
    this.startMs = nowMs;
    this.lastProgressMs = nowMs;
    return true;
  }

  /**
   * Accept one parsed BLOB_CHUNK. Returns true when it was valid, addressed to
   * THIS transfer and copied in (duplicates are idempotent and still true).
   */
  insert(h, nowMs) {
    if (!this.active) return false;
    if (!sameBlobTarget(h, this.target)) return false;
    if (h.chunkCount !== this.chunkCount || h.chunkIndex >= this.chunkCount) return false;
    const offset = h.chunkIndex * BLOB_CHUNK_PAYLOAD;
    if (offset + h.bytes.length > this.buf.length) return false;
    this.buf.set(h.bytes, offset);
    if (!this.received[h.chunkIndex]) {
      this.received[h.chunkIndex] = true;
      this.receivedCount++;
    }
    this.lastProgressMs = nowMs;
    return true;
  }

  complete() { return this.active && this.receivedCount === this.chunkCount; }

  /** @returns {number[]} not-yet-received chunk indices (ascending) */
  missingIndices() {
    if (!this.active) return [];
    const out = [];
    for (let i = 0; i < this.chunkCount; i++) if (!this.received[i]) out.push(i);
    return out;
  }

  /** §8.4: no progress for catalog_chunk_gap_timeout_ms while still incomplete. */
  gapElapsed(nowMs) {
    return this.active && !this.complete() &&
      nowMs - this.lastProgressMs >= LIMITS.catalog_chunk_gap_timeout_ms;
  }

  /** Arm the next gap interval (call after emitting a repair BLOB_REQ). */
  noteRepairSent(nowMs) { this.lastProgressMs = nowMs; }

  /** §8.4: total-abandon threshold since the transfer began. */
  timedOut(nowMs) {
    return this.active && nowMs - this.startMs >= LIMITS.frag_reassembly_timeout_ms;
  }

  /** @returns {Uint8Array} the reassembled bytes (valid when complete()) */
  assembled() { return this.buf.subarray(0, this.totalBytes); }
}

// ============================================================================
// Catalog CBOR decode (catalog_codec.hpp / schema/catalog.cddl)
// ============================================================================

/**
 * Decode the reassembled catalog encoding into channel entries.
 *
 * catalog = [ *channel-entry ]
 * channel-entry = {1:id, 2:name, 3:class, 4:dir, 5:access, 6:maxRateHz f32,
 *                  7:priority, ?8:[layout-field], ?9:{key=>schema-field},
 *                  ?10:category, ?11:category_label, ?12:store-descriptor,
 *                  ?13:replay_depth, ?14:setting_channel, ?15:stream_kind}
 * layout-field = {1:name, 2:packedType, 3:unit, 4:scale f32, ?5:min, ?6:max,
 *                 ?7:{bits}, ?8:setting_key, ?9:default, ?10:[options],
 *                 ?11:group, ?12:desc, ?13:role, ?14:step, ?15:flags, ?18:size}
 * schema-field = {1:name, 2:cborType, 3:unit, ?5:min, ?6:max, ?9:default,
 *                 ?10:[options], ?11:group, ?12:desc, ?13:role, ?14:step,
 *                 ?15:flags, ?16:access, ?17:[option_access]}
 *
 * Unknown keys are ignored per §4.3 — forward compatibility is mandatory.
 * @param {Uint8Array} bytes
 * @returns {Array<Object>} channel entries
 */
export function decodeCatalog(bytes) {
  const [entries] = cbDecode(bytes, 0);
  if (!Array.isArray(entries)) throw new Error('decodeCatalog: top-level must be an array');
  return entries.map(decodeEntry);
}

/** @param {Map} m @param {number} k @param {*} d @returns {*} */
function mget(m, k, d) {
  return m instanceof Map && m.has(k) ? m.get(k) : d;
}

function decodeEntry(m) {
  const cls = mget(m, 3, 0);
  const dir = mget(m, 4, 0);
  const access = mget(m, 5, 0);
  const entry = {
    id: mget(m, 1, 0),
    name: mget(m, 2, ''),
    cls,
    clsName: CHANNEL_CLASS_NAME[cls] || String(cls),
    dir,
    dirName: DIRECTION_NAME[dir] || String(dir),
    access,
    accessName: ACCESS_NAME[access] || String(access),
    maxRateHz: mget(m, 6, 0),
    priority: mget(m, 7, 0),
    layout: null,
    schema: null,
    store: null,
    category: null,
    categoryName: null,
    categoryLabel: null,
    replayDepth: null,
    settingChannel: null,
    streamKind: 0,
  };
  if (m.has(8)) entry.layout = m.get(8).map(decodeLayoutField);
  if (m.has(9)) {
    const sm = m.get(9); // Map<key, field-map>
    entry.schema = [];
    for (const [key, fm] of sm) entry.schema.push(decodeSchemaField(key, fm));
    entry.schema.sort((a, b) => a.key - b.key);
  }
  if (m.has(10)) {
    entry.category = m.get(10);
    entry.categoryName = SETTING_CATEGORY_NAME[entry.category] || null;
  }
  if (m.has(11)) entry.categoryLabel = m.get(11);
  if (m.has(12)) entry.store = decodeStoreDescriptor(m.get(12));
  if (m.has(13)) entry.replayDepth = m.get(13);
  if (m.has(14)) entry.settingChannel = m.get(14);
  if (m.has(15)) entry.streamKind = m.get(15);
  return entry;
}

/** RFC-009 annotations shared by layout- and schema-fields (keys 9..15). */
function decodeSharedAnnotations(fm, f) {
  if (fm.has(9)) f.default = fm.get(9);
  if (fm.has(10)) f.options = fm.get(10); // [tstr], wire value = index
  if (fm.has(11)) f.group = fm.get(11);
  if (fm.has(12)) f.desc = fm.get(12);
  if (fm.has(13)) f.role = fm.get(13);
  if (fm.has(14)) f.step = fm.get(14);
  if (fm.has(15)) {
    f.flags = fm.get(15);
    f.flagBits = {
      advanced: (f.flags & SETTING_FLAG.advanced) !== 0,
      restart_required: (f.flags & SETTING_FLAG.restart_required) !== 0,
      secret: (f.flags & SETTING_FLAG.secret) !== 0,
    };
  }
}

function decodeLayoutField(fm) {
  const type = mget(fm, 2, 0);
  const f = {
    name: mget(fm, 1, ''),
    type,
    typeName: PACKED_NAME[type] || String(type),
    unit: mget(fm, 3, ''),
    scale: mget(fm, 4, 1),
  };
  if (fm.has(5)) f.min = fm.get(5);
  if (fm.has(6)) f.max = fm.get(6);
  if (fm.has(7)) {
    const bm = fm.get(7); // Map<bitIndex, name>
    const bits = new Array(8).fill('');
    for (const [bit, nm] of bm) bits[bit] = nm;
    f.bits = bits;
  }
  // RFC-009: PRESENT setting_key = this field is a stored SETTING and key
  // `settingKey` on the entry's paired INTENT channel writes it. ABSENT =
  // read-only effective/telemetry truth — display it, NEVER write it back into
  // a setting's shadow. That presence test IS RFC-003's stored/effective
  // distinction, and stomping a stored value with an effective one is the
  // original slopsync-js window bug.
  if (fm.has(8)) f.settingKey = fm.get(8);
  decodeSharedAnnotations(fm, f);
  // RFC-037: the field's packed width, stated explicitly rather than derived
  // from `type`. Absent on every catalog this reference device currently
  // emits (the encoder side is a named follow-up) — this is forward-looking
  // decode support, exercised so far only by this file's own tests. See
  // decodePacked() for what it buys: an unknown FUTURE packed type becomes a
  // skippable hole in the layout instead of truncating every field after it.
  if (fm.has(18)) f.declaredSize = fm.get(18);
  return f;
}

function decodeSchemaField(key, fm) {
  const type = mget(fm, 2, 0);
  const f = {
    key,
    name: mget(fm, 1, ''),
    type,
    typeName: CBOR_FIELD_NAME[type] || String(type),
    unit: mget(fm, 3, ''),
  };
  if (fm.has(5)) f.min = fm.get(5);
  if (fm.has(6)) f.max = fm.get(6);
  decodeSharedAnnotations(fm, f);
  if (fm.has(16)) f.access = fm.get(16); // per-field minimum role
  // RFC-009/025b: index-aligned with `options` — element i is the minimum role
  // for wire value i. A client that reads this GRAYS the ops it cannot use
  // instead of discovering them by NACK (gray, never hide).
  if (fm.has(17)) f.optionAccess = fm.get(17);
  return f;
}

function decodeStoreDescriptor(dm) {
  return {
    storeId: mget(dm, 1, 0),
    kind: mget(dm, 2, ''),
    capacity: mget(dm, 3, 0),
    perItemMax: mget(dm, 4, 0),
    nameMax: mget(dm, 5, 0),
  };
}

/**
 * Build a Map<channelId, entry> from a decoded catalog array.
 * @param {Array<Object>} entries
 * @returns {Map<number, Object>}
 */
export function catalogChannelMap(entries) {
  const map = new Map();
  for (const e of entries) map.set(e.id, e);
  return map;
}

// ============================================================================
// Catalog introspection helpers (RFC-009 / RFC-016)
// ============================================================================

/**
 * INTENT value encoding, resolved FROM THE CATALOG. Replaces the hand-copied
 * INTENT_SCHEMAS table that used to live in session.js and had already drifted
 * (it encoded a float as an integer for months). The device publishes a
 * per-key CBOR type on every INTENT channel; this reads it.
 * @param {Object} entry a decoded catalog entry (class INTENT)
 * @returns {Map<number, Object>} field key -> schema field
 */
export function schemaByKey(entry) {
  const out = new Map();
  if (entry && entry.schema) for (const f of entry.schema) out.set(f.key, f);
  return out;
}

/**
 * The minimum role required to send `optionValue` on schema field `key` of
 * `entry`, or the field/channel floor when the catalog says nothing.
 * @returns {number} an ACCESS level
 */
export function optionAccessFor(entry, key, optionValue) {
  if (!entry) return 0;
  const f = schemaByKey(entry).get(key);
  const floor = (f && f.access != null) ? f.access : entry.access;
  if (!f || !f.optionAccess) return floor;
  const a = f.optionAccess[optionValue];
  return a == null ? floor : a;
}

/**
 * May a session holding `roles` send `optionValue` on this field? RFC-009's
 * gray-never-hide input: a client grays what this returns false for, and the
 * hub gates on the very same catalog data (Hub::requiredAccessFor), so the two
 * cannot disagree.
 */
export function canUseOption(entry, key, optionValue, roles) {
  return (roles | 0) >= optionAccessFor(entry, key, optionValue);
}

/** All channel entries a client may see for a given class (e.g. CHANNEL_CLASS.STORE). */
export function entriesOfClass(entries, cls) {
  return (entries || []).filter((e) => e.cls === cls);
}

/** Does this hub advertise `channelId`? (RFC-016: capability discovery IS catalog introspection.) */
export function hasChannel(channelMap, channelId) {
  return !!(channelMap && channelMap.has(channelId));
}

// ============================================================================
// Packed STATE decode (SPEC §5.4/§8.2)
// ============================================================================

const _dec = new TextDecoder();

/**
 * Decode a packed STATE payload against a layout descriptor array. Wire =
 * physical * scale for scaled numeric fields, so decoded = raw / scale. A
 * bitfield8 passes through as a raw byte (scale ignored) plus, if named, a
 * `<name>_bits` object of {bitName:boolean}. RFC-026 str16/32/64 fields are
 * fixed-width zero-padded UTF-8 and decode to a string.
 *
 * A SHORT payload stops gracefully (append-only layout evolution means an
 * older hub's shorter snapshot is a correct PREFIX of a newer layout), and a
 * LONGER payload than the layout describes is ignored past the end — the same
 * append-only rule from the other side.
 *
 * RFC-037 (`f.declaredSize`, catalog key 18): a field's byte width is
 * otherwise derivable ONLY from `type` — so the day the registry adds packed
 * type 11, every client that meets it on the wire has to stop decoding the
 * layout THERE, not just at that field: every later field's offset becomes
 * unknowable too, and the whole tail goes dark. A declared size fixes that:
 *   - KNOWN type, declared size present and it disagrees with the type's own
 *     width: that is a CATALOG AUTHORING error (conformance is supposed to
 *     catch it before it ships). We trust the TYPE, not the declaration —
 *     the type is what tells us how to actually decode the bytes — and warn
 *     once per field so the mismatch doesn't decode-loop silently forever.
 *   - UNKNOWN type, declared size present: a SKIPPABLE HOLE. We don't know
 *     what the bytes mean, but we know how many there are, so we step over
 *     them and keep decoding every field that follows — the whole point of
 *     RFC-037.
 *   - UNKNOWN type, no declared size: offsets are genuinely unknowable past
 *     here — stop, exactly as before this RFC (a pre-RFC-037 hub, or a hub
 *     that forgot to state it, gets today's behavior, not a crash).
 *
 * @param {Uint8Array} payload
 * @param {Array<{name,type,scale,bits?,declaredSize?}>} layout
 * @returns {Object} field name -> value (+ optional _bits maps)
 */
export function decodePacked(payload, layout) {
  const dv = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  const out = {};
  let off = 0;
  for (const f of layout) {
    const derivedSize = PACKED_SIZE[f.type];
    if (derivedSize == null) {
      // Unknown packed type (a future registry addition this build predates).
      if (f.declaredSize == null) break; // offsets unknowable past here — stop (pre-RFC-037 behavior)
      if (off + f.declaredSize > payload.byteLength) break; // short frame — stop gracefully
      off += f.declaredSize; // skippable hole: no value to produce, but decoding continues
      continue;
    }
    if (f.declaredSize != null && f.declaredSize !== derivedSize && !f._sizeMismatchWarned) {
      f._sizeMismatchWarned = true; // warn once per field, not once per STATE push
      // eslint-disable-next-line no-console
      console.warn('slopsync: catalog field "' + f.name + '" (type ' + f.typeName +
        ') declares size ' + f.declaredSize + ' B but the type is ' + derivedSize +
        ' B wide — trusting the type (RFC-037: this is a catalog authoring error)');
    }
    const size = derivedSize; // known type: TRUST THE TYPE, never the declaration
    if (off + size > payload.byteLength) break; // short frame — stop gracefully
    let raw;
    switch (f.type) {
      case PACKED.u8: raw = dv.getUint8(off); break;
      case PACKED.i8: raw = dv.getInt8(off); break;
      case PACKED.u16: raw = dv.getUint16(off, true); break;
      case PACKED.i16: raw = dv.getInt16(off, true); break;
      case PACKED.u32: raw = dv.getUint32(off, true); break;
      case PACKED.i32: raw = dv.getInt32(off, true); break;
      case PACKED.f32: raw = dv.getFloat32(off, true); break;
      case PACKED.bitfield8: raw = dv.getUint8(off); break;
      case PACKED.str16:
      case PACKED.str32:
      case PACKED.str64: {
        const bytes = payload.subarray(off, off + size);
        let end = bytes.length;
        while (end > 0 && bytes[end - 1] === 0) end--; // zero-padded, register-map style
        raw = _dec.decode(bytes.subarray(0, end));
        break;
      }
      default: raw = 0;
    }
    off += size;
    if (f.type === PACKED.bitfield8) {
      out[f.name] = raw;
      if (f.bits) {
        const bitsOut = {};
        for (let b = 0; b < 8; b++) if (f.bits[b]) bitsOut[f.bits[b]] = (raw & (1 << b)) !== 0;
        out[f.name + '_bits'] = bitsOut;
      }
    } else if (typeof raw === 'string') {
      out[f.name] = raw;
    } else {
      const scale = f.scale || 1;
      out[f.name] = scale !== 1 ? raw / scale : raw;
    }
  }
  return out;
}

/**
 * Decode an EVENT's scoped `body` (key 40) sub-map against the channel's OWN
 * catalog schema (§9.4 + the v1.0 grammar fix). Kind-specific fields used to
 * sit in the global CBOR key space, which meant every device-authored EVENT
 * channel needed a registry PR to name its own fields — precisely the coupling
 * the self-describing catalog exists to prevent. They now ride `body`, keyed
 * exactly as an INTENT's `value` (20) is.
 *
 * @param {Map} bodyMap the decoded key-40 sub-map
 * @param {Object} entry the EVENT channel's catalog entry (may be null)
 * @returns {Object} field name -> value (unknown keys kept as `key<N>`)
 */
export function decodeEventBody(bodyMap, entry) {
  const out = {};
  if (!(bodyMap instanceof Map)) return out;
  const byKey = schemaByKey(entry);
  for (const [k, v] of bodyMap) {
    const f = byKey.get(k);
    out[f ? f.name : 'key' + k] = v;
  }
  return out;
}
