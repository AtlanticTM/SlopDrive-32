/**
 * ble-ws.js — SlopSync-over-BLE-GATT as a WebSocket duck (RFC-043).
 *
 * Constraints:
 * - SHELL ONLY. Imports @mnlphlp/plugin-blec (Tauri); never reachable from
 *   the embedded build (main.js's SHELL branch is compiled away there).
 * - Session.js owns the protocol; this file is ONLY framing transport. It
 *   implements the exact WebSocket subset session.js touches (binaryType,
 *   send, close, onopen/onmessage/onclose/onerror, readyState) so it can be
 *   passed as createSession({ WebSocketImpl }).
 * - One GATT notification = one SlopSync frame; one write = one frame
 *   (SPEC §13.4: no fragmentation — an oversized frame is a hard error,
 *   never split).
 * - Writes are chained on a queue: frame ORDER is protocol-critical and
 *   concurrent plugin calls must not reorder HELLO/SUBSCRIBE/INTENT.
 * - UUIDs are registry `ble_identity` values (same source as the firmware's
 *   SlopSyncBleTransport.h — transcribed, the codegen does not emit JS).
 */

import {
  connect as blecConnect,
  disconnect as blecDisconnect,
  subscribe as blecSubscribe,
  send as blecSend,
  getMtu,
} from '@mnlphlp/plugin-blec';

export const BLE_SERVICE = '534c4f50-5359-4e43-8000-000000000001';
const CHAR_C2H_WRITE = '534c4f50-5359-4e43-8000-000000000002';
const CHAR_H2C_NOTIFY = '534c4f50-5359-4e43-8000-000000000003';

/**
 * Build a WebSocket-duck class bound to one BLE peripheral address.
 * session.js instantiates it per (re)connect; the url argument is ignored.
 * blec holds ONE connection at a time — fine for a single-hub session.
 */
export function makeBleWebSocket(address) {
  return class BleWebSocket {
    constructor(_url, _protocols) {
      this.binaryType = 'arraybuffer';
      this.readyState = 0; // CONNECTING
      this.onopen = null;
      this.onmessage = null;
      this.onclose = null;
      this.onerror = null;
      this._dead = false;
      this._writeQ = Promise.resolve();
      this._open();
    }

    async _open() {
      try {
        await blecConnect(address, () => this._dropped());
        await blecSubscribe(CHAR_H2C_NOTIFY, BLE_SERVICE, (data) => {
          if (this._dead || !this.onmessage) return;
          this.onmessage({ data: new Uint8Array(data).buffer });
        });
        this.mtu = await getMtu().catch(() => 0);
        if (this._dead) { blecDisconnect().catch(() => {}); return; }
        this.readyState = 1; // OPEN
        if (this.onopen) this.onopen();
      } catch (e) {
        this._fail(e);
      }
    }

    _dropped() {
      if (this._dead) return;
      this._dead = true;
      this.readyState = 3;
      if (this.onclose) this.onclose({ code: 1006, reason: 'ble link lost' });
    }

    _fail(e) {
      if (this._dead) return;
      this._dead = true;
      this.readyState = 3;
      if (this.onerror) this.onerror(e);
      if (this.onclose) this.onclose({ code: 1006, reason: String(e) });
    }

    send(buf) {
      if (this._dead || this.readyState !== 1) return;
      const bytes = buf instanceof ArrayBuffer ? new Uint8Array(buf)
        : ArrayBuffer.isView(buf) ? new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength)
        : null;
      if (!bytes) { this._fail(new Error('ble-ws: non-binary send')); return; }
      const arr = Array.from(bytes);
      this._writeQ = this._writeQ.then(
        () => blecSend(CHAR_C2H_WRITE, arr, 'withResponse', BLE_SERVICE),
      ).catch((e) => this._fail(e));
    }

    close() {
      if (this._dead) { return; }
      this._dead = true;
      this.readyState = 2; // CLOSING
      blecDisconnect().catch(() => {}).then?.(() => {});
      Promise.resolve().then(() => {
        this.readyState = 3;
        if (this.onclose) this.onclose({ code: 1000, reason: 'client close' });
      });
    }
  };
}

/** u32 big-endian (WELCOME key 47) → dotted quad, or null. */
export function ipv4ToString(v) {
  if (!v) return null;
  return [(v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255].join('.');
}
