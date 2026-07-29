# SlopDrive-32

Open-source ESP32-S3 / ESP32-C5 firmware for a capstan-drum linear stroke
machine. The custom controller PCB carries a main ESP32-S3 brain plus an
onboard ESP32-C5-Zero coprocessor. Clients drive it over **SlopSync**, this
project's own CBOR wire protocol, across WebSocket and BLE GATT, and a
catalog-driven web UI served off LittleFS handles configuration and real-time
telemetry.

> **This project was vibecoded.** Treat it as such. It moves a heavy carriage
> under power — read the code before you trust it with anything attached to
> you.

## Documentation

This README is a stub, pending a rewrite. The authoritative docs are:

| Where | What |
|---|---|
| `docs/canon/CANON.md` | Governance law (C-1..C-12). Read first. |
| `docs/canon/DOCTRINE.md` | Engineering rules: architecture, motion, build/test/OTA. |
| `docs/canon/LEDGER.md` | Current volatile truth — versions, deploy state, milestones. |
| `docs/canon/TRAPS.md` | Field-bug mechanism lessons. |
| `docs/webui-architecture.md` | The catalog-driven web UI. |
| SlopSync repo | The protocol spec, library, and clients — a sibling repo. |

Build and deployment procedure lives in DOCTRINE (build/test/OTA sections);
`tools/canon_lint.py` must report zero findings before work is called done.

## License

SlopDrive-32 is licensed under the **CERN Open Hardware License Version 2 —
Strongly Reciprocal (CERN-OHL-S v2)**. See [`LICENSE`](LICENSE) and
[`NOTICE`](NOTICE). Provided **as-is, with no warranty**.

The license is not optional or incidental: this firmware incorporates the
Advanced Penetration pattern engine ported from
[fray-d/OSSM-Lite](https://github.com/fray-d/OSSM-Lite), which is CERN-OHL-S
v2. That license is *strongly reciprocal*, so anyone distributing SlopDrive-32,
or hardware and firmware based on it, must make the Complete Source available
under the same license.

Vendored components keep their original permissive licenses and notices — the
StrokeEngine pattern math (theelims / KinkyMakers, MIT), TempestMAx's `Axis`
Hermite interpolation (MIT), and jcfain's TCode ramp struct (MIT). The project
also links third-party Arduino libraries and the Espressif Arduino core, each
under its own license. Full attribution is in
[`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md).
