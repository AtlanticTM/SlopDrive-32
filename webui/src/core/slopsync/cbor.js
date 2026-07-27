/**
 * cbor.js — minimal deterministic CBOR codec for SlopSync (SPEC §5.3).
 *
 * Deterministic profile ONLY: definite-length containers, shortest-form ints,
 * map keys ascending (caller-supplied order, asserted), float32-only (0xFA +
 * big-endian binary32 — NEVER float16/64), no tags, no indefinite forms, no
 * simple values beyond false/true/null.
 *
 * This is a direct port of the encoder/decoder shape in
 * clients/mfp-slopsync/SlopSync.cs (CborWriter/CborReader) and
 * tools/slopsync_probe.py (cb_* / cb_decode). Golden-byte verified against
 * WireSelfTest.cs — do NOT "optimize" the byte layout, it is wire-visible.
 *
 * Types map: uint/negative-int -> number, byte string -> Uint8Array,
 * text string -> string, array -> Array, map(int keys) -> Map<number,value>,
 * f32 -> number, bool -> boolean, null -> null.
 */

// ---- low-level: initial byte + argument, shortest form ---------------------

/**
 * Encode a CBOR head (major type + argument value), shortest form.
 * @param {number} major 0..7
 * @param {number} v non-negative integer argument
 * @returns {Uint8Array}
 */
export function head(major, v) {
  const ib0 = major << 5;
  if (v <= 23) return Uint8Array.of(ib0 | v);
  if (v <= 0xff) return Uint8Array.of(ib0 | 24, v);
  if (v <= 0xffff) return Uint8Array.of(ib0 | 25, (v >> 8) & 0xff, v & 0xff);
  if (v <= 0xffffffff) {
    return Uint8Array.of(ib0 | 26, (v >>> 24) & 0xff, (v >>> 16) & 0xff, (v >>> 8) & 0xff, v & 0xff);
  }
  // 8-byte argument (rare on this wire; ids/timestamps are u32). Split hi/lo.
  const hi = Math.floor(v / 0x100000000);
  const lo = v >>> 0;
  return Uint8Array.of(
    ib0 | 27,
    (hi >>> 24) & 0xff, (hi >>> 16) & 0xff, (hi >>> 8) & 0xff, hi & 0xff,
    (lo >>> 24) & 0xff, (lo >>> 16) & 0xff, (lo >>> 8) & 0xff, lo & 0xff
  );
}

/** Concatenate Uint8Arrays into one. @param {Uint8Array[]} parts */
export function concatBytes(parts) {
  let total = 0;
  for (const p of parts) total += p.length;
  const out = new Uint8Array(total);
  let off = 0;
  for (const p of parts) { out.set(p, off); off += p.length; }
  return out;
}

// ---- encoders (each returns a Uint8Array of that value's encoding) ---------

/** @param {number} v non-negative integer */
export function cbUint(v) { return head(0, v); }

/** @param {number} v signed integer */
export function cbInt(v) { return v >= 0 ? head(0, v) : head(1, -1 - v); }

/** @param {boolean} v */
export function cbBool(v) { return Uint8Array.of(v ? 0xf5 : 0xf4); }

export function cbNull() { return Uint8Array.of(0xf6); }

/** float32: major 7 / ai 26, big-endian binary32. @param {number} v */
export function cbF32(v) {
  const buf = new Uint8Array(5);
  buf[0] = 0xfa;
  new DataView(buf.buffer).setFloat32(1, v, false); // big-endian
  return buf;
}

/** UTF-8 text string. @param {string} s */
export function cbTstr(s) {
  const b = new TextEncoder().encode(s);
  return concatBytes([head(3, b.length), b]);
}

/** byte string. @param {Uint8Array|number[]} bytes */
export function cbBstr(bytes) {
  const b = bytes instanceof Uint8Array ? bytes : Uint8Array.from(bytes);
  return concatBytes([head(2, b.length), b]);
}

/** definite-length array. @param {Uint8Array[]} elems already-encoded elements */
export function cbArray(elems) {
  return concatBytes([head(4, elems.length), ...elems]);
}

/**
 * definite-length map. `pairs` = [[intKey, encodedValueBytes], ...] the caller
 * supplies ALREADY sorted ascending by key (§5.3). Asserted so a mistake fails
 * loudly instead of silently producing non-canonical bytes.
 * @param {Array<[number, Uint8Array]>} pairs
 */
export function cbMap(pairs) {
  const keys = pairs.map((p) => p[0]);
  for (let i = 1; i < keys.length; i++) {
    if (keys[i] <= keys[i - 1]) {
      throw new Error('cbMap: keys must be strictly ascending (SPEC §5.3): ' + keys.join(','));
    }
  }
  const parts = [head(5, pairs.length)];
  for (const [k, v] of pairs) { parts.push(cbUint(k), v); }
  return concatBytes(parts);
}

// ---- decoder ---------------------------------------------------------------

/**
 * Decode one CBOR item from `buf` at `pos`. Returns [value, newPos].
 * Maps decode to Map<number,value>, arrays to Array. Throws on anything
 * outside the deterministic profile.
 * @param {Uint8Array} buf
 * @param {number} [pos]
 * @returns {[*, number]}
 */
export function cbDecode(buf, pos = 0) {
  const ib = buf[pos];
  const major = ib >> 5;
  const ai = ib & 0x1f;

  if (major === 7) {
    if (ai === 20) return [false, pos + 1];
    if (ai === 21) return [true, pos + 1];
    if (ai === 22) return [null, pos + 1];
    if (ai === 26) {
      const v = new DataView(buf.buffer, buf.byteOffset + pos + 1, 4).getFloat32(0, false);
      return [v, pos + 5];
    }
    throw new Error('cbDecode: major7 ai=' + ai + ' outside deterministic profile (no float16/64/simple)');
  }
  if (major === 6) throw new Error('cbDecode: CBOR tags forbidden (§5.3)');

  let val;
  let p;
  if (ai <= 23) { val = ai; p = pos + 1; }
  else if (ai === 24) { val = buf[pos + 1]; p = pos + 2; }
  else if (ai === 25) { val = (buf[pos + 1] << 8) | buf[pos + 2]; p = pos + 3; }
  else if (ai === 26) {
    val = (buf[pos + 1] * 0x1000000) + (buf[pos + 2] << 16) + (buf[pos + 3] << 8) + buf[pos + 4];
    p = pos + 5;
  } else if (ai === 27) {
    // 8-byte: assemble as a JS number (safe for u32 ids/timestamps on this wire)
    let hi = 0;
    let lo = 0;
    for (let i = 0; i < 4; i++) hi = hi * 256 + buf[pos + 1 + i];
    for (let i = 0; i < 4; i++) lo = lo * 256 + buf[pos + 5 + i];
    val = hi * 0x100000000 + lo;
    p = pos + 9;
  } else {
    throw new Error('cbDecode: indefinite/reserved ai=' + ai + ' forbidden');
  }

  if (major === 0) return [val, p];
  if (major === 1) return [-1 - val, p];
  if (major === 2) return [buf.slice(p, p + val), p + val];
  if (major === 3) return [new TextDecoder().decode(buf.subarray(p, p + val)), p + val];
  if (major === 4) {
    const arr = [];
    for (let i = 0; i < val; i++) { const [v, np] = cbDecode(buf, p); arr.push(v); p = np; }
    return [arr, p];
  }
  if (major === 5) {
    const m = new Map();
    for (let i = 0; i < val; i++) {
      const [k, kp] = cbDecode(buf, p);
      const [v, vp] = cbDecode(buf, kp);
      m.set(k, v);
      p = vp;
    }
    return [m, p];
  }
  throw new Error('cbDecode: unreachable major ' + major);
}

/** Decode a whole buffer as one CBOR item. @param {Uint8Array} buf */
export function cbDecodeFull(buf) {
  const [val] = cbDecode(buf, 0);
  return val;
}
