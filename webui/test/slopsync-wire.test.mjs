/**
 * slopsync-wire.test.mjs — golden-byte + round-trip test for slopsync-js.
 *
 * Golden hex is lifted verbatim from clients/mfp-slopsync/WireSelfTest.cs, whose
 * expected values are themselves derived by RUNNING tools/slopsync_probe.py's
 * builders (the live-verified reference). If slopsync-js produces byte-identical
 * output, it is on the same wire as the probe and the C# plugin.
 *
 * Run:  node webui/test/slopsync-wire.test.mjs   (exits 1 on any failure)
 *
 * NOTE: the browser session's own HELLO deliberately omits the publish wish
 * (key 11) — the WebUI SUBSCRIBEs h2c channels, it does not publish a stream.
 * To prove the CBOR codec against the C# golden HELLO (which DOES carry a
 * publish wish), we reconstruct that with-publish HELLO from the same cbor.js
 * primitives here. SUBSCRIBE / GOODBYE / PING / CLOCK use the real builders.
 */

import { readFileSync, existsSync } from 'node:fs';
import { webcrypto } from 'node:crypto';
import {
  cbUint, cbF32, cbTstr, cbBstr, cbBool, cbInt, cbNull, cbArray, cbMap,
  cbDecodeFull, concatBytes,
} from '../src/core/slopsync/cbor.js';
import {
  K, PRIORITY, FRAME, ACCESS, PACKED, PACKED_SIZE, GOODBYE_CODE, NACK,
  SAFETY_OP, BLOB_NS, CH_SAFETY, LIMITS,
  encodeFrame, encodeEstopFrame, crc32, ESTOP_FRAME_BYTES,
} from '../src/core/slopsync/frames.js';
import {
  buildBlobReq, buildCatalogRequest, buildCatalogRepair,
  parseBlobChunk, BlobReassembler, BLOB_CHUNK_HEADER_BYTES,
  decodeCatalog, catalogChannelMap, decodePacked, decodeEventBody,
  schemaByKey, optionAccessFor, canUseOption,
} from '../src/core/slopsync/catalog.js';
import { sha256, catalogEtag, toHex, bytesEqual } from '../src/core/slopsync/sha256.js';

let failures = 0;

function hex(bytes) {
  return Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('').toUpperCase();
}
function check(name, actual, expectedHex) {
  const got = hex(actual);
  const want = expectedHex.replace(/\s+/g, '').toUpperCase();
  const ok = got === want;
  console.log('  [' + (ok ? 'PASS' : 'FAIL') + '] ' + name);
  if (!ok) {
    console.log('        expected: ' + want);
    console.log('        actual:   ' + got);
    failures++;
  }
}
function assert(name, cond) {
  console.log('  [' + (cond ? 'PASS' : 'FAIL') + '] ' + name);
  if (!cond) failures++;
}

console.log('slopsync-js wire self-test (golden bytes from WireSelfTest.cs / slopsync_probe.py):');

const inst = Uint8Array.of(0, 1, 2, 3, 4, 5, 6, 7);

// ---- HELLO (with publish wish) — CBOR codec proof --------------------------
// keys ascending 1<2<3<4<11; publishes array element {12:rate, 15:channel}.
function buildHelloWithPublish(kind, name, instanceId, ch, rate) {
  return cbMap([
    [K.proto_ver, cbUint(1)],
    [K.client_kind, cbTstr(kind)],
    [K.client_name, cbTstr(name)],
    [K.instance_id, cbBstr(instanceId)],
    [K.publishes, cbArray([cbMap([[K.rate_hz, cbF32(rate)], [K.channel_id, cbUint(ch)]])])],
  ]);
}
// RFC-047 (Phase C2): the probe wishes for motion-input, now 0x2100 (was
// 0x0084) — golden bytes cross-checked byte-for-byte against
// clients/mfp-slopsync/WireSelfTest.cs's own regenerated golden (same
// payload string), which is itself derived by running slopsync_probe.py's
// builders. 0x2100 needs CBOR's 2-byte uint form (0x19 0x21 0x00) where
// 0x0084 fit in the 1-byte form (0x18 0x84) — the payload grows 1 byte.
const hello = buildHelloWithPublish('probe', 'slopsync_probe.py', inst, 0x2100, 100.0);
check('HELLO payload (CBOR)', hello,
  'A50101026570726F62650371736C6F7073796E635F70726F62652E7079044800010203040506070B81A20CFA42C800000F192100');
check('HELLO frame', encodeFrame(FRAME.HELLO, 0, hello, 0),
  '0000000000003400A50101026570726F62650371736C6F7073796E635F70726F62652E7079044800010203040506070B81A20CFA42C800000F192100');

// ---- SUBSCRIBE — the real browser builder ----------------------------------
// {10:[{12:rate,13:prio,15:channel}]} keys ascending 12<13<15.
function buildSubscribe(wishes) {
  const entries = wishes.map(([ch, rate, prio]) =>
    cbMap([[K.rate_hz, cbF32(rate)], [K.priority, cbUint(prio)], [K.channel_id, cbUint(ch)]]));
  return cbMap([[K.subscriptions, cbArray(entries)]]);
}
// RFC-047 (Phase C2): motion is now 0x1100 (was 0x0080) -- CBOR needs its
// 3-byte uint form (0x19 0x11 0x00) where 0x0080 fit the 2-byte form
// (0x18 0x80).
const sub = buildSubscribe([[0x0003, 0.0, PRIORITY.critical], [0x1100, 20.0, PRIORITY.elevated]]);
check('SUBSCRIBE payload (CBOR)', sub, 'A10A82A30CFA000000000D030F03A30CFA41A000000D020F191100');

// ---- GOODBYE (NORMAL_CLOSURE 0x0107) ---------------------------------------
// RFC-022.2: GOODBYE has NO code space of its own — its codes are DRAWN FROM
// nack_codes, which is why GOODBYE_CODE is an alias table and not new numbers.
// (Two clients had independently hand-written this literal before it existed.)
check('GOODBYE payload (CBOR)', cbMap([[K.code, cbUint(GOODBYE_CODE.NORMAL_CLOSURE)]]), 'A110190107');
assert('GOODBYE_CODE.NORMAL_CLOSURE is nack_codes 0x0107', GOODBYE_CODE.NORMAL_CLOSURE === NACK.NORMAL_CLOSURE);
assert('v1.0 NACK codes present (REBOOTING/READY_TIMEOUT/NOT_READY)',
  NACK.REBOOTING === 0x0109 && NACK.READY_TIMEOUT === 0x010a && NACK.NOT_READY === 0x010b);

// ---- CLOCK request (raw u32 LE) --------------------------------------------
const clk = new Uint8Array(4);
new DataView(clk.buffer).setUint32(0, 0x11223344, true);
check('CLOCK request (raw u32 LE)', clk, '44332211');

// ---- PING frame (raw, empty payload; §6.5) ---------------------------------
check('PING frame (raw, empty)', encodeFrame(FRAME.PING, 0, new Uint8Array(0), 0), '0300000000000000');

// ---- STREAM bundle payload (packed, §5.4) — proves the packed layout -------
// [t_base:u32 LE][n:u8][rsv:u8][off:u16 LE]  then {target u16 ×10000, vel i16 ×1000}
function buildStreamBundle(tBase, samples) {
  const n = samples.length;
  const buf = new Uint8Array(6 + n * 2 + n * 4);
  const dv = new DataView(buf.buffer);
  let p = 0;
  dv.setUint32(p, tBase, true); p += 4;
  dv.setUint8(p++, n); dv.setUint8(p++, 0);
  for (const [off] of samples) { dv.setUint16(p, off, true); p += 2; }
  for (const [, target, vel] of samples) {
    dv.setUint16(p, Math.max(0, Math.min(65535, Math.round(target * 10000))), true); p += 2;
    dv.setInt16(p, Math.max(-32768, Math.min(32767, Math.round(vel * 1000))), true); p += 2;
  }
  return buf;
}
check('STREAM bundle payload (packed)', buildStreamBundle(0x00010203, [[0, 0.5, 1.7592918]]),
  '03020100010000008813DF06');

// ---- CBOR round-trip (encode → decode → compare) ---------------------------
const roundtrip = cbMap([
  [1, cbUint(1)],
  [6, cbUint(0xdeadbeef)],       // u32 session-id-scale value
  [9, cbInt(-42)],               // negative int
  [12, cbF32(100.0)],            // f32
  [16, cbBool(true)],
  [17, cbTstr('slopsync.v1')],
  [19, cbBstr(Uint8Array.of(0x21, 0xcb, 0x26, 0xc9))], // etag-like bstr
  [20, cbArray([cbUint(3), cbBool(false), cbNull()])],
  [22, cbMap([[1, cbUint(242)], [2, cbUint(64)]])],     // nested map (welcome-limits shape)
]);
const dec = cbDecodeFull(roundtrip);
assert('round-trip: is a Map', dec instanceof Map);
assert('round-trip: uint key 1', dec.get(1) === 1);
assert('round-trip: u32 key 6', dec.get(6) === 0xdeadbeef);
assert('round-trip: negative int key 9', dec.get(9) === -42);
assert('round-trip: f32 key 12 ≈ 100', Math.abs(dec.get(12) - 100.0) < 1e-4);
assert('round-trip: bool key 16', dec.get(16) === true);
assert('round-trip: tstr key 17', dec.get(17) === 'slopsync.v1');
assert('round-trip: bstr key 19', hex(dec.get(19)) === '21CB26C9');
assert('round-trip: array key 20 [3,false,null]',
  Array.isArray(dec.get(20)) && dec.get(20)[0] === 3 && dec.get(20)[1] === false && dec.get(20)[2] === null);
assert('round-trip: nested map key 22', dec.get(22) instanceof Map && dec.get(22).get(1) === 242 && dec.get(22).get(2) === 64);

// f32 exact round-trip through the wire
const f = cbDecodeFull(cbF32(1.7592918));
assert('round-trip: f32 1.7592918 (within f32 eps)', Math.abs(f - 1.7592918) < 1e-6);

// map ascending-key guard fires
let guardFired = false;
try { cbMap([[2, cbUint(0)], [1, cbUint(0)]]); } catch (e) { guardFired = true; }
assert('cbMap enforces ascending keys (§5.3)', guardFired);

// concatBytes sanity
assert('concatBytes joins', hex(concatBytes([Uint8Array.of(0xaa), Uint8Array.of(0xbb, 0xcc)])) === 'AABBCC');

// ============================================================================
// v1.0 wire additions (M5c)
// ============================================================================
console.log('');
console.log('v1.0 framing (BLOB transfer, readiness gate, estop, packed strings):');

// ---- Frame numbers ---------------------------------------------------------
assert('new frame types (PUBLISH/CATALOG_READY/BLOB_REQ/BLOB_CHUNK/AUTH/HUB_SIG)',
  FRAME.PUBLISH === 0x18 && FRAME.CATALOG_READY === 0x19 && FRAME.BLOB_REQ === 0x1a &&
  FRAME.BLOB_CHUNK === 0x1b && FRAME.AUTH === 0x1c && FRAME.HUB_SIG === 0x1d);
assert('CATALOG_REQ/CATALOG_CHUNK are RETIRED (absent, never reused)',
  FRAME.CATALOG_REQ === undefined && FRAME.CATALOG_CHUNK === undefined);
assert('safety_ops::estop = 6 (RFC-010) + override/bypass 7..10 (RFC-025c)',
  SAFETY_OP.estop === 6 && SAFETY_OP.override_on === 7 && SAFETY_OP.override_off === 8 &&
  SAFETY_OP.bypass_on === 9 && SAFETY_OP.bypass_off === 10);
assert('access tiers renamed, wire values unchanged (watch0/control1/configure2)',
  ACCESS.watch === 0 && ACCESS.control === 1 && ACCESS.configure === 2);

// ---- BLOB_REQ (0x1A) — the ONE transfer verb -------------------------------
// A bare catalog request is the EMPTY map: namespace 0 is the default and
// store_id/slot are absent by rule, so generalizing transfer cost the common
// case exactly zero bytes (blob_req.hpp's own worked example).
check('BLOB_REQ full catalog (empty map)', buildCatalogRequest(), 'A0');
check('BLOB_REQ catalog repair {27:[4,9]}', buildCatalogRepair([4, 9]), 'A1181B820409');
// {38: {1:1, 2:3, 3:7}} -> full transfer of store 3, slot 7
check('BLOB_REQ store 3 slot 7 {38:{1:1,2:3,3:7}}',
  buildBlobReq({ ns: BLOB_NS.store, storeId: 3, slot: 7 }), 'A11826A3010102030307');
// {27:[4,9], 38:{...}} -> repair of that item; keys ascending 27 < 38
check('BLOB_REQ store repair {27:[4,9],38:{1:1,2:3,3:7}}',
  buildBlobReq({ ns: BLOB_NS.store, storeId: 3, slot: 7, chunks: [4, 9] }),
  'A2181B8204091826A3010102030307');
let blobGuard = 0;
try { buildBlobReq({ chunks: [] }); } catch (e) { blobGuard++; }           // RFC-022.6
try { buildBlobReq({ storeId: 1 }); } catch (e) { blobGuard++; }            // catalog ns takes no store
assert('BLOB_REQ refuses an empty repair and a catalog-ns store_id', blobGuard === 2);

// ---- BLOB_CHUNK (0x1B) — 14-byte binary identity header --------------------
// ns|store_id|slot|reserved | generation:u16 | chunk_index:u16 | chunk_count:u16
// | total_bytes:u32, all LE. The header GREW 4 -> 14 bytes at v1.0.
assert('BLOB_CHUNK header is 14 bytes', BLOB_CHUNK_HEADER_BYTES === 14);
function buildBlobChunk(ns, storeId, slot, generation, index, count, total, body) {
  const out = new Uint8Array(14 + body.length);
  const dv = new DataView(out.buffer);
  dv.setUint8(0, ns); dv.setUint8(1, storeId); dv.setUint8(2, slot); dv.setUint8(3, 0);
  dv.setUint16(4, generation, true);
  dv.setUint16(6, index, true);
  dv.setUint16(8, count, true);
  dv.setUint32(10, total, true);
  out.set(body, 14);
  return out;
}
const chunk0 = buildBlobChunk(0, 0, 0, 0, 0, 2, 5, Uint8Array.of(0xaa, 0xbb, 0xcc));
check('BLOB_CHUNK header bytes (catalog, chunk 0 of 2, total 5)',
  chunk0.subarray(0, 14), '00000000000000000200 05000000');
const ph = parseBlobChunk(chunk0);
assert('parseBlobChunk: ns/index/count/total',
  ph.ns === 0 && ph.chunkIndex === 0 && ph.chunkCount === 2 && ph.totalBytes === 5);
assert('parseBlobChunk: body slice', hex(ph.bytes) === 'AABBCC');
assert('parseBlobChunk: rejects a short payload', parseBlobChunk(new Uint8Array(13)) === null);

// Reassembly out of order, with a duplicate. Chunk payloads are 192 B except
// the last (blob_chunks.hpp: per-chunk offset = index * catalog_chunk_payload),
// so a 195-byte blob is exactly two chunks.
const wholeBlob = new Uint8Array(195);
for (let i = 0; i < wholeBlob.length; i++) wholeBlob[i] = (i * 31 + 7) & 0xff;
const c0 = parseBlobChunk(buildBlobChunk(0, 0, 0, 0, 0, 2, 195, wholeBlob.subarray(0, 192)));
const c1 = parseBlobChunk(buildBlobChunk(0, 0, 0, 0, 1, 2, 195, wholeBlob.subarray(192)));
const ra = new BlobReassembler();
ra.begin(c1, 0); // begun from whichever chunk arrives first — it declares total_bytes
ra.insert(c1, 1);
assert('BlobReassembler: incomplete with a hole', !ra.complete());
assert('BlobReassembler: missingIndices names the hole', ra.missingIndices().join(',') === '0');
ra.insert(c0, 2);
ra.insert(c0, 3); // duplicate is idempotent
assert('BlobReassembler: complete after the hole is filled', ra.complete());
assert('BlobReassembler: assembled bytes in index order', hex(ra.assembled()) === hex(wholeBlob));
assert('BlobReassembler: rejects another blob\'s chunk',
  !ra.insert(parseBlobChunk(buildBlobChunk(1, 3, 7, 0, 0, 2, 195, wholeBlob.subarray(0, 192))), 4));
const raRefuse = new BlobReassembler(1024);
assert('BlobReassembler: refuses a transfer larger than its capacity (RFC-028)',
  raRefuse.begin({ ns: 0, storeId: 0, slot: 0, chunkCount: 9999, totalBytes: 4000000 }, 0) === false &&
  raRefuse.missingIndices().length === 0);

// ---- CATALOG_READY (0x19) — raw plane, the 8 etag bytes, nothing else ------
const etagDemo = Uint8Array.of(0xf4, 0xa2, 0x8f, 0xbb, 0x58, 0xce, 0xd1, 0x6a);
check('CATALOG_READY frame (raw 8-byte etag)',
  encodeFrame(FRAME.CATALOG_READY, 0, etagDemo, 0), '19000000000008 00 F4A28FBB58CED16A');
assert('etag is limits::etag_bytes (8)', LIMITS.etag_bytes === 8 && etagDemo.length === 8);

// ---- SHA-256 (etag verification, §8.3) -------------------------------------
// NIST FIPS 180-4 published vectors — an INDEPENDENT check that the hand-rolled
// hash is right, because crypto.subtle is unavailable on a plain-http LAN page.
check('SHA-256("abc") — NIST vector', sha256(new TextEncoder().encode('abc')),
  'BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD');
check('SHA-256("") — NIST vector', sha256(new Uint8Array(0)),
  'E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855');
check('SHA-256 448-bit vector', sha256(new TextEncoder().encode(
  'abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq')),
  '248D6A61D20638B8E5C026930C3E6039A33CE45964FF2167F6ECEDD419DB06C1');
// A 1 KB pseudo-random blob cross-checked against node's own WebCrypto.
const blobBytes = new Uint8Array(1024);
for (let i = 0; i < blobBytes.length; i++) blobBytes[i] = (i * 167 + 13) & 0xff;
const nodeDigest = new Uint8Array(await webcrypto.subtle.digest('SHA-256', blobBytes));
assert('SHA-256 agrees with node WebCrypto over 1 KB', hex(sha256(blobBytes)) === hex(nodeDigest));
assert('catalogEtag truncates to 8 bytes of the digest',
  bytesEqual(catalogEtag(blobBytes), sha256(blobBytes).subarray(0, 8)));

// ---- CRC-32 + the raw ESTOP frame (§5.5) -----------------------------------
assert('CRC-32/IEEE("123456789") = 0xCBF43926',
  crc32(new TextEncoder().encode('123456789')) === 0xcbf43926);
const est = encodeEstopFrame(0 /* cause=user */, 1 /* origin=control */, 0x0102);
assert('ESTOP frame is 12 bytes', est.length === ESTOP_FRAME_BYTES);
assert('ESTOP magic is 4x0xE5 (recognizable WITHOUT deframing)',
  est[0] === 0xe5 && est[1] === 0xe5 && est[2] === 0xe5 && est[3] === 0xe5);
assert('ESTOP cause/origin/seq (LE)', est[4] === 0 && est[5] === 1 && est[6] === 0x02 && est[7] === 0x01);
const estCrc = new DataView(est.buffer).getUint32(8, true);
assert('ESTOP crc32 covers bytes [0,8)', estCrc === crc32(est.subarray(0, 8)));

// ---- packed decode: RFC-026 str16 + the 9-byte RFC-025c safety snapshot ----
assert('str16/str32/str64 wire sizes', PACKED_SIZE[PACKED.str16] === 16 &&
  PACKED_SIZE[PACKED.str32] === 32 && PACKED_SIZE[PACKED.str64] === 64);
const strPayload = new Uint8Array(16 + 4);
new TextEncoder().encodeInto('SlopDrive', strPayload.subarray(0, 16)); // zero-padded
new DataView(strPayload.buffer).setFloat32(16, 12.5, true);
const strDec = decodePacked(strPayload, [
  { name: 'hub_name', type: PACKED.str16, scale: 1 },
  { name: 'volts', type: PACKED.f32, scale: 1 },
]);
assert('decodePacked: str16 strips zero padding', strDec.hub_name === 'SlopDrive');
assert('decodePacked: field after a str16 is at the right offset', strDec.volts === 12.5);

// safety 0x0003 grew `modes` (manual_override + bypass_limits): 8 -> 9 B.
const safetyLayout = [
  { name: 'word', type: PACKED.bitfield8, scale: 1, bits: ['estop', 'stop', 'hold', 'pause'] },
  { name: 'cause', type: PACKED.u8, scale: 1 },
  { name: 'owner_session', type: PACKED.u32, scale: 1 },
  { name: 'estop_seq', type: PACKED.u16, scale: 1 },
  { name: 'modes', type: PACKED.bitfield8, scale: 1, bits: ['override', 'bypass'] },
];
const safety9 = Uint8Array.of(0x03, 0x00, 0xef, 0xbe, 0xad, 0xde, 0x07, 0x00, 0x03);
const sd = decodePacked(safety9, safetyLayout);
assert('safety decode: word bits (estop+stop latched)',
  sd.word_bits.estop === true && sd.word_bits.stop === true && sd.word_bits.hold === false);
assert('safety decode: owner_session u32 LE', sd.owner_session === 0xdeadbeef);
assert('safety decode: estop_seq', sd.estop_seq === 7);
assert('safety decode: NEW modes byte (override+bypass)',
  sd.modes_bits.override === true && sd.modes_bits.bypass === true);
// An OLD 8-byte snapshot must still parse its prefix (append-only evolution).
const sd8 = decodePacked(safety9.subarray(0, 8), safetyLayout);
assert('safety decode: 8-byte prefix still decodes, modes simply absent',
  sd8.owner_session === 0xdeadbeef && sd8.modes === undefined);

// ---- catalog decode: RFC-009 annotations + option_access -------------------
// A hand-built entry in the exact shape catalog_codec.hpp emits: one INTENT
// channel whose enum-valued `op` field carries options (10) and an
// index-aligned per-option minimum role (17).
const miniCatalog = cbArray([
  cbMap([
    [1, cbUint(CH_SAFETY + 2)], // 0x0005 safety-intents
    [2, cbTstr('safety-intents')],
    [3, cbUint(2)],             // class INTENT
    [4, cbUint(1)],             // dir c2h
    [5, cbUint(ACCESS.watch)],  // access floor: watch — and that is the point
    [6, cbF32(20.0)],
    [7, cbUint(PRIORITY.critical)],
    [9, cbMap([[1, cbMap([
      [1, cbTstr('op')],
      [2, cbUint(0)],           // cbor type uint_t
      [3, cbTstr('')],
      [10, cbArray([cbTstr('reserved'), cbTstr('estop_clear'), cbTstr('stop'),
        cbTstr('hold'), cbTstr('pause'), cbTstr('resume'), cbTstr('estop')])],
      [12, cbTstr('What to do about safety.')],
      [17, cbArray([cbUint(1), cbUint(1), cbUint(0), cbUint(1), cbUint(1), cbUint(1), cbUint(0)])],
    ])]])],
    [10, cbUint(2)],            // category: limits
  ]),
]);
const miniEntries = decodeCatalog(miniCatalog);
const miniMap = catalogChannelMap(miniEntries);
const si = miniMap.get(0x0005);
assert('catalog decode: entry basics', si && si.name === 'safety-intents' && si.clsName === 'INTENT');
assert('catalog decode: access floor is watch', si.access === ACCESS.watch && si.accessName === 'watch');
assert('catalog decode: category resolves to a registry name', si.categoryName === 'limits');
const opField = schemaByKey(si).get(1);
assert('catalog decode: schema field options[6] === estop', opField.options[6] === 'estop');
assert('catalog decode: desc annotation', opField.desc === 'What to do about safety.');
assert('catalog decode: option_access decoded (key 17)', Array.isArray(opField.optionAccess));
assert('option_access: stop(2) and estop(6) are ROLE-EXEMPT (watch)',
  optionAccessFor(si, 1, SAFETY_OP.stop) === ACCESS.watch &&
  optionAccessFor(si, 1, SAFETY_OP.estop) === ACCESS.watch);
assert('option_access: hold(3) needs control',
  optionAccessFor(si, 1, SAFETY_OP.hold) === ACCESS.control);
assert('gray-never-hide: a watch session may estop but not hold',
  canUseOption(si, 1, SAFETY_OP.estop, ACCESS.watch) === true &&
  canUseOption(si, 1, SAFETY_OP.hold, ACCESS.watch) === false &&
  canUseOption(si, 1, SAFETY_OP.hold, ACCESS.control) === true);

// ---- EVENT `body` (key 40) decoded against the channel's OWN schema --------
// v1.0 moved kind-specific fields OFF the global key space into this scoped
// sub-map; without it every device-authored EVENT channel would need a registry
// PR to name its own fields.
const evtEntry = decodeCatalog(cbArray([cbMap([
  [1, cbUint(0x0089)], [2, cbTstr('motion-anomaly')], [3, cbUint(3)], [4, cbUint(0)],
  [5, cbUint(ACCESS.watch)], [6, cbF32(0)], [7, cbUint(PRIORITY.normal)],
  [9, cbMap([
    [1, cbMap([[1, cbTstr('kind')], [2, cbUint(0)], [3, cbTstr('')]])],
    [2, cbMap([[1, cbTstr('detail')], [2, cbUint(0)], [3, cbTstr('')]])],
  ])],
])]))[0];
const body = decodeEventBody(new Map([[1, 5], [2, 42], [9, 'unknown']]), evtEntry);
assert('EVENT body: schema field names resolved', body.kind === 5 && body.detail === 42);
assert('EVENT body: unknown key kept, never dropped (§4.3)', body.key9 === 'unknown');

// ---- Golden fixture: the REAL catalog fetched from slopsim ------------------
// Written by webui/test/slopsync-sim.mjs. Hashing the real hub's real catalog
// bytes and matching the etag IT declared is a cross-implementation check of
// this file's SHA-256 against lib/slopsync's C++ Sha256.
const FIXTURE = new URL('./fixtures/slopsim-catalog.bin', import.meta.url);
const FIXTURE_ETAG = new URL('./fixtures/slopsim-catalog.etag', import.meta.url);
if (existsSync(FIXTURE) && existsSync(FIXTURE_ETAG)) {
  const catBytes = new Uint8Array(readFileSync(FIXTURE));
  const hubEtag = readFileSync(FIXTURE_ETAG, 'utf8').trim();
  assert('fixture: JS SHA-256 etag == the etag the hub declared (' + hubEtag + ')',
    toHex(catalogEtag(catBytes)) === hubEtag);
  const realEntries = decodeCatalog(catBytes);
  const realMap = catalogChannelMap(realEntries);
  assert('fixture: real catalog decodes (' + realEntries.length + ' channels)', realEntries.length > 5);
  const safety = realMap.get(0x0003);
  assert('fixture: 0x0003 safety carries the APPENDED modes bitfield (8 -> 9 B)',
    !!safety && safety.layout.some((f) => f.name === 'modes'));
  const realSi = realMap.get(0x0005);
  assert('fixture: 0x0005 advertises option_access with estop role-exempt',
    optionAccessFor(realSi, 1, SAFETY_OP.estop) === ACCESS.watch &&
    optionAccessFor(realSi, 1, SAFETY_OP.hold) === ACCESS.control);

  // ---- GAP CLOSED (SlopDeck milestone 1, sim fidelity) -------------------
  // Was an [SKIP-EXPECTED-GAP]: sim/slopsim's DEFAULT catalog used to be
  // benchrig::buildDivergentCatalog() (a deliberately different third-party
  // catalog, SlopSimCatalog.h), which never had a channel shaped like the
  // real device's motion telemetry (0x1100, RFC-047; was 0x0080). DESIGN.md's
  // catalog-profiles ruling made benchrig `--profile alien` instead and
  // restored `--profile device` (now the default) to literal device-catalog
  // fidelity — buildSlopDriveCatalog() from include/comms/SlopSyncCatalog.h,
  // the SAME definition the firmware ships — so this fixture (captured from
  // the `device` profile) now always carries the real 0x1100 shape.
  const motion = realMap.get(0x1100);
  assert('fixture: 0x1100 motion carries the raw_10um field (7 -> 9 B)',
    !!motion && motion.layout.some((f) => f.name === 'raw_10um'));
} else {
  console.log('  [SKIP] real-catalog fixture (run: node webui/test/slopsync-sim.mjs)');
}

console.log('');
if (failures === 0) { console.log('ALL PASS'); process.exit(0); }
console.log(failures + ' FAILED');
process.exit(1);
