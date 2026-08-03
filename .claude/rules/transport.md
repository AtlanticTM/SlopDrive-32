---
paths:
  - "src/comms/**"
  - "include/comms/**"
  - "src/c5_probe/**"
---

# Transport constraints (pointers; SPEC §13/§14 and the headers own the story)

## UART bridge profile (S3 <-> C5, SlopSyncUartTransport)

- Serial2 only, TX GPIO43 / RX GPIO44, 2 Mbaud. Serial1 is the Modbus servo
  bus and must never be touched from here (SlopSyncUartTransport.h:16-17,86).
- Buffers sized BEFORE begin() (HardwareSerial refuses to resize running):
  TX 4096, RX 16384. The RX buffer must outlast the 5 ms DRAIN INTERVAL, not
  the frame: 2 Mbaud lands ~5,000 B between hub ticks; 4096 overflowed by
  construction (SlopSyncUartTransport.h:100-111, c5-comms-offload.md §4.6).
- Bulk reads only: readBytes() into a 512 B chunk. Per-byte read() takes the
  UART mutex per call and measurably cannot keep up
  (src/comms/SlopSyncUartTransport.cpp:149-157).
- RX ring depth 64 per slot, sized for ~1 s of hub stall, not steady rate.
  16 (copied from BLE) measured drops live (SlopSyncUartTransport.h:143-165).
- pumpRx() stays in TASK context, never an ISR: the service is PSRAM-resident
  and PSRAM faults during flash-cache-disabled windows
  (SlopSyncUartTransport.h:47-52). SPSC + deferred attach/detach discipline
  kept identical to the other ports even though producer==consumer here
  (TRAPS T5; SlopSyncUartTransport.h:28-46).
- write() checks availableForWrite() and refuses instead of blocking; false
  is flow control (§13.1), not an error. No per-session teardown over shared-
  wire backpressure (SlopSyncUartTransport.h:54-63).

## COBS + CRC

- Wire: COBS([slot_id][slopsync frame]) + 0x00 delimiter (SPEC §13.5).
  Delimiter-finding is the transport's job; the codec is the sibling lib's
  wire/serial_cobs.hpp, used identically by both ends. Never reimplement
  COBS here.
- Ordinary frames carry no CRC; the binding declares reliable=true (wired
  point-to-point). The ESTOP frame is the one exception: 12 bytes,
  E5 E5 E5 E5 | cause origin seq:u16 | crc32 (IEEE, first 8 bytes).

## ESTOP raw-scan duty

- A receiver in unsynced/corrupt state MUST scan raw bytes for the 4x0xE5
  magic before and regardless of COBS decode (SPEC §13.5). COBS passes a
  zero-free 0xE5 run through unchanged; that property is load-bearing.
  Implemented live in src/c5_probe/main.cpp pumpFromS3(): scan first, fan
  out under the slot lock, no session required.
- On CRC failure resume scanning at i+1, not i+4 (overlapping candidates).

## Relay semantics (SPEC §14), for anything relay-shaped

- Frames, not sessions; a relay holds no grants and parses no control CBOR.
- Priority-aware dual queues; may decimate samples-kind streams and conflate
  STATE, MUST NOT decimate segments-kind streams (a segment is a schedule).
  Blind FIFO is non-conformant (§14.1).
- Reliability is hop-by-hop (Honesty Clause H10). ESTOP fast path: forward on
  all attached segments ahead of every queued frame, at most one frame-time
  of added latency (§14.2). CLOCK frames: correct, be transparent (<1 ms), or
  drop; exactly one (§14.3). One relay hop maximum in v1.
- The C5 bridge is NOT a §14 relay: it terminates the WS session and forwards
  raw frames over UART; hub/session state lives on the S3. It implements the
  ESTOP raw-scan duty; it does not implement §14.1 shedding
  (docs/c5-comms-offload.md, "bridge, not relocation").
