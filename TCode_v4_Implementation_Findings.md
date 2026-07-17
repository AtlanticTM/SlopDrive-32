# TCode v0.4 Implementation — Recce Findings & Implementation Plan

**Date**: 2026-07-15  
**Repo**: SlopDrive-32 (origin: AtlanticTM/SlopDrive-32)  
**Reference**: [jcfain/TCodeESP32](https://github.com/jcfain/TCodeESP32) commit `5303fb60`, path `ESP32/src/TCode/v0.4`  
**Status**: Read-only analysis — no code edited. Handoff to implementation model.

---

## 1. Reference v0.4 Architecture (jcfain/TCodeESP32)

The reference codebase layers TCode v0.4 on top of an external `<TCode.h>` library which provides `TCodeManager`, `TCodeAxis`, `AxisData`, `AxisRampData`, `AxisExtentionType`, `AxisType`, and the observer/event framework. The hierarchy:

```
TCodeBase                       (abstract: setup, read(byte), read(String), setMessageCallback, sendMessage)
  └─ TCodeBaseV4                (abstract: RegisterAxis, setAxisData×3, getChannelPosition,
                                  getAxisLastCommandTime, updateInterfaces, getDeviceSettings)
       └─ TCode0_4              (concrete: owns TCodeManager m_tcode, EventHandler, OutputStream)
```

### 1.1 Files in `v0.4/`

| File | Role |
|------|------|
| `TCodeBaseV4.h` | Abstract interface extending `TCodeBase` with v0.4-specific pure virtuals. Uses `namespace TCode`, `TCode::Axis::TCodeAxis`, `TCode::Datatypes::AxisRampData`, etc. |
| `TCode0_4.h` | Concrete v0.4 implementation. Owns `TCodeManager m_tcode`, wires `OutputStream` (output), `EventHandler` (observer). Delegates `read(byte)`/`read(String)` to `m_tcode.read()`. |
| `OutputStream.h` | Implements `TCode::Output::TCodeIOutputStream` — all writes are stubbed (`// Serial.print(value)` commented out). Bridge to transport layer. |
| `EventHandler.h` | Implements `TCode::Observer::TCodeIObserver<TCodeEvent>`. Dispatches `DeviceCommandEvent` (StopDevice, GetSoftwareVersion, GetTCodeVersion, GetAssignedAxisValues) and `FirmwareCommandEvent` via a registered `message_callback`. |
| `MotorHandler0_4.h` | Base handler (`MotorHandler` → `MotorHandler0_4`). Owns `TCode0_4* m_tcode`. Registers aux/vibe/twist/squeeze/valve/lube axes as `TCodeAxis` objects. Reads positions via `m_tcode->getChannelPosition()`. Sets initial data via `m_tcode->setAxisData()`. |
| `BLDCHandler0_4.h` | BLDC motor variant. Registers a linear stroke axis `TCodeAxis("Stroke", {AxisType::Linear, 0}, 0.5f)`, reads position, controls motor via SimpleFOC. **Note: marked as broken — `setup()` logs error and returns false.** |
| `ServoHandler0_4.h` | Servo variant (OSR2/SR6 kinematics). Registers stroke (L0), surge (L1), sway (L2), roll (R1), pitch (R2) axes. Per-axis `channelRead()` → kinematics → `ledcWrite()`. |

### 1.2 Key v0.4 Concepts vs v0.3

| Concept | v0.3 (Reference) | v0.4 |
|---------|------------------|------|
| Axis model | Fixed arrays `TCodeAxis0_3 Linear[11]`, `Rotation[11]`, etc. | Dynamic `TCodeAxis*` pointers registered with `TCodeManager`. Named axes (e.g., "Stroke", "Twist"). Default position value on registration (0.5f = centered). |
| Magnitude decode | Fixed 4-digit pad: `magString.substring(0,4)` padded with zeros → `/10000` range 0-9999. | Delegated to `<TCode.h>` `TCodeManager` — variable-digit fractional decode (spec-correct v0.3). |
| Ramp | Only I (interval ms) and S (speed per 100ms) modifiers. | I/S modifiers PLUS `AxisRampData` struct with ramp-in/ramp-out acceleration profiles. |
| Position query | `AxisRead("L0")` → 0-9999 float. | `getChannelPosition(TCodeAxis*)` → uint16_t = `value * 10000` (0-9999). |
| Last-commanded | `AxisLast("L0")` → `unsigned long`. | `getAxisLastCommandTime(TCodeAxis*)` → `unsigned long`. |
| Device info | `D0`/`D1`/`D2` hardcoded in `deviceCmd()`. | Dispatched via `EventHandler` observer → `message_callback`. |
| Device settings | `D2` → hardcoded axis enumeration from EEPROM ranges. | `D2` → `getDeviceSettings(char* buf)` — dynamic axis enumeration (stub in v0.4 reference). |
| Stop | `DSTOP` → loops all axes calling `.Stop()` / `.Set(0,...)`. | `DSTOP` → dispatched by `EventHandler` as `DeviceCommandType::StopDevice`. |
| Setup commands | `$L0-xxxx-xxxx` for per-axis range config, saved to EEPROM. | Not yet implemented in v0.4 reference (commented out `Setup` case in `EventHandler`). |
| Output stream | `sendMessage()` → `Serial.println()` (hardcoded Serial). | `OutputStream` → `TCodeIOutputStream` interface (stubbed — pluggable transport). |
| Axis data set | `AxisInput("L0", mag, ext, extMag)` → `Linear[0].Set()`. | `setAxisData(TCodeAxis*, value, extType, extValue)` + `setAxisData(TCodeAxis*, AxisData)` — structured data with optional `AxisRampData`. |

### 1.3 External `<TCode.h>` Library Dependency

The reference v0.4 does **NOT** include the `<TCode.h>` library source. It `#include <TCode.h>` and uses:
- `TCode::TCodeManager` — the parser engine
- `TCode::Axis::TCodeAxis` — axis object (constructor takes name, `AxisId {AxisType, channel}`, default value)
- `TCode::Datatypes::AxisData` — struct { float value; unsigned long commandExtension; AxisExtentionType extentionType; AxisRampData rampIn; AxisRampData rampOut; }
- `TCode::Datatypes::AxisRampData` — struct { float rampStartMultiplier; float rampEndMultiplier; bool rampStartEnable; bool rampEndEnable; bool rampEndVariable; }
- `TCode::Datatypes::AxisExtentionType` — enum { None, Speed, Time }
- `TCode::Datatypes::AxisType` — enum { None, Linear, Rotation, Vibration, Auxiliary }
- `TCode::Observer::TCodeIObserver<TCodeEvent>` — observer interface
- `TCode::Observer::TCodeEvent` — event struct
- `TCode::Output::TCodeIOutputStream` — output stream interface
- `TCode::Datatypes::CommandType` — enum { None, Axis, Device, Firmware, Setup }
- `TCode::Datatypes::DeviceCommandType` — enum { None, StopDevice, GetSoftwareVersion, GetAssignedAxisValues, GetTCodeVersion }

This library appears to be a separate Arduino/PlatformIO library developed by jcfain/Khrull as part of the TCodeESP32 project. It is **not** available as a public PlatformIO library ID. The reference codebase's `platformio.ini` likely lists it as a local `lib_deps` path or git submodule.

### 1.4 Data Flow in v0.4 Reference

```
Transport (Serial/BLE)
  │ bytes / string
  ▼
TCode0_4::read(byte) / read(String)
  │
  ▼
TCodeManager::read(byte) / read(String)   ← external <TCode.h> library
  │
  ├─▶ Axis command → updates registered TCodeAxis objects internally
  │                   (stores value, extension, ramp data, timestamp)
  │
  ├─▶ Device command → TCodeIObserver::notify(TCodeEvent)
  │                      └─ EventHandler::notify()
  │                           ├─ DeviceCommandType::StopDevice → callback("DSTOP\n")
  │                           ├─ GetSoftwareVersion → callback("Firmware v...\n")
  │                           ├─ GetTCodeVersion → callback("TCode v0.4\n")
  │                           └─ GetAssignedAxisValues → (stub)
  │
  └─▶ Output writes → OutputStream::write/print/println
                       └─ (stubbed — intended for transport response)
```

**Execution loop** (periodic, on Core 1 in the reference):
```
BLDCHandler0_4::execute() / ServoHandler0_4::execute()
  │
  ├─ channelRead(stroke_axis)  → m_tcode->getChannelPosition(axis) → 0-9999
  ├─ channelRead(twist_channel) → 0-9999
  ├─ channelRead(vibe_channel)  → 0-9999
  ├─ (kinematics / motor control from position values)
  └─ executeCommon(xLin) → twist, valve, squeeze, vibe, lube sub-functions
```

---

## 2. Current SlopDrive-32 TCode Architecture (v0.3)

### 2.1 Files

| File | Role |
|------|------|
| `include/comms/TCodeParser.h` | Transport-agnostic v0.3 line parser class. |
| `src/comms/TCodeParser.cpp` | Parser implementation: `feedLine()` with `strtok` on whitespace, handles `D0`/`D1`/`D2`/`DSTOP`, `L0` magnitude (digit-count fractional decode OR Intiface `/999` compat). `I` and `S` modifiers. Callbacks: `onLinearCmd`, `onStop`, `onResponse`, `onUnknownCmd`. |
| `src/main.cpp` | Wires `buttplugLinearCmd(float pos, uint32_t dur)` and `buttplugStop()` as TCodeParser callbacks. Only L0 is processed — R/V/A are silently skipped. |
| `include/comms/SerialTransport.h/.cpp` | USB Serial transport — feeds bytes into `TCodeParser::feedLine()`. |
| `include/comms/WebSocketTransport.h/.cpp` | WS TCode server + Intiface WSDM client — feeds bytes into `TCodeParser::feedLine()`. |
| `include/comms/BleTransport.h/.cpp` | Nordic UART BLE transport — feeds bytes into `TCodeParser::feedLine()`. |
| `include/comms/DongleTransport.h/.cpp` | T-Dongle C5 UART relay — feeds bytes into `TCodeParser::feedLine()`. |
| `include/comms/OssmBleService.h/.cpp` | **Not TCode** — separate OSSM protocol (set:/go:/stream:). OSSM GATT service masquerade for app compatibility. |
| `include/comms/TransportManager.h/.cpp` | Orchestrates exactly-one-active-transport. Installs `onResponse` hooks per transport for D0/D1/D2 replies. |

### 2.2 Current TCode v0.3 Limitations

1. **Single axis (L0 only)**: R/V/A tokens are recognized but skipped (no callback). No rotation/vibration/aux channel support.
2. **No axis abstraction**: A single `LinearCmdCallback` for all L0 commands. No named axes, no per-axis state objects. The callback receives raw `(position, duration_ms)`.
3. **No ramp profiles**: Only `I` (interval ms) and `S` (speed) modifiers are parsed. No acceleration ramp-in/ramp-out data.
4. **Hardcoded D0/D1/D2 responses**:
   - `D0` → `"D0 SlopDrive-32 1.0\n"`
   - `D1` → `"D1 TCode v0.3\n"` (needs to become `"TCode v0.4\n"`)
   - `D2` → `"D2 L0 0 9999 Up\n"` (hardcoded — no dynamic axis enumeration)
5. **No `GetAssignedAxisValues`** device command (new in v0.4).
6. **No axis position query API**: The callback gives position once; there's no persistent axis-state object to poll for current position.
7. **No last-commanded-time query**: Vibe timeout logic would need to be built from scratch.
8. **No output-stream abstraction**: Each transport installs its own `ResponseCallback` directly on the parser. There's no intermediate output stream interface.
9. **No observer/event pattern**: Device commands are handled inline in `feedLine()` via hardcoded `strncasecmp`.
10. **No `#`/`$` setup commands**: Axis range configuration (`$L0-xxxx-xxxx`) is not parsed (these characters don't match D/L/R/V and fall through to `onUnknownCmd`).

### 2.3 What Currently Works Well

- **Transport-agnostic design**: `TCodeParser` has zero hardware dependencies — transports inject themselves via callbacks.
- **Spec-correct v0.3 magnitude decode**: `mag_value / 10^digits` + Intiface compat toggle.
- **Streaming performance**: The waypoint queue + `motionConsumerTask` pipeline handles 100+ Hz streams.
- **D0/D1/D2/DSTOP**: Basic device commands already parse and fire callbacks.
- **Extensibility hook**: `onUnknownCmd` for sideband commands.

---

## 3. Gap Analysis: v0.3 → v0.4

### 3.1 What v0.4 Adds Over v0.3 (per the TCode Specification)

| Feature | v0.3 Status | v0.4 Required |
|---------|-------------|---------------|
| L0 linear position + I/S ramp | ✅ Implemented | ✅ Keep |
| R0–R9 rotation channels | ❌ Skipped | 🟡 Register R0 (twist), R1 (roll), R2 (pitch) as available axes |
| V0–V9 vibration channels | ❌ Skipped | 🟡 Register V0–V3 as available axes |
| A0–A9 auxiliary channels | ❌ Skipped | 🟡 Register A0–A3 as available axes |
| Ramp-in/ramp-out acceleration profiles | ❌ Not supported | 🔴 New — `AxisRampData` struct per axis command |
| `GetAssignedAxisValues` device command | ❌ Not supported | 🔴 New — `EventHandler` → `DeviceCommandType::GetAssignedAxisValues` |
| Dynamic D2 axis enumeration | ❌ Hardcoded | 🔴 Must enumerate all registered axes with their channel ranges |
| Per-axis state objects (`TCodeAxis`) | ❌ Single callback | 🔴 Need axis objects with `getName()`, `getPosition()`, `getLastCommandTime()`, `getId()` |
| Output stream abstraction | ❌ Direct callback | 🟡 `OutputStream` implementing `TCodeIOutputStream` (or equivalent transport bridge) |
| Observer/event dispatch for device commands | ❌ Inline if-else | 🟡 `EventHandler` pattern (or equivalent callback dispatch) |
| `D1` reports v0.4 | `"TCode v0.3"` | `"TCode v0.4"` |
| Backward compatibility | ✅ v0.3 wire format | ✅ v0.4 must parse all v0.3 frames identically |
| `#`/`$` setup commands | ❌ Not parsed | 🟡 (Optional — v0.4 reference doesn't implement them either; commented out) |

### 3.2 Architectural Mismatches

**Biggest gap**: SlopDrive-32 has NO axis state model. The parser fires a callback once per command with `(position, duration_ms)` — that's it. The v0.4 model requires persistent axis objects that hold:
- Current commanded position (updated by parser on each axis command)
- Ramp profile data (rampIn, rampOut)
- Last command timestamp
- Channel ID (`{AxisType::Linear, 0}`)
- Name (e.g., "Stroke", "Twist")

The execution loop then POLLS these axis objects each tick for their current interpolated position via `getChannelPosition()`. This is fundamentally different from the current push-with-duration model.

**Second gap**: The v0.4 reference delegates ALL parsing to the external `<TCode.h>` library. SlopDrive-32 has its own hand-rolled parser. Options:
1. Integrate the `<TCode.h>` library as a PlatformIO dependency
2. Extend the existing parser with v0.4 features
3. Port the `<TCode.h>` library's relevant logic into the project

---

## 4. Implementation Options

### Option A: Integrate `<TCode.h>` Library (Recommended)

**Approach**: Add the external `<TCode.h>` library (from jcfain/TCodeESP32 or its upstream) as a PlatformIO `lib_deps` entry (git submodule or local path). Implement `TCodeBaseV4` and `TCode0_4` as adapters that bridge the library's `TCodeManager` to SlopDrive-32's transport and motion layers.

**Pros**:
- Full v0.4 parser including ramp profiles, multi-axis, observer events
- Backward compatibility is guaranteed by the library
- Matches the reference architecture exactly (easier to track upstream changes)
- `EventHandler` observer pattern cleanly separates device command dispatch from transport

**Cons**:
- External dependency on a library that may not be on the PlatformIO registry
- The library's internal API may change
- Adds a C++ namespace (`TCode`) that may conflict (unlikely — no `TCode` namespace currently in SlopDrive-32)

**Required changes**:
1. **Add `<TCode.h>` to `platformio.ini`** as a git-based `lib_deps` entry (pointing at jcfain/TCodeESP32 or the library's own repo).
2. **Create `include/tcode/TCodeV4Adapter.h` and `src/tcode/TCodeV4Adapter.cpp`**: A class that:
   - Owns `TCodeManager m_manager`
   - Owns `EventHandler m_eventHandler` (bridges device commands → transport response callback)
   - Owns `OutputStream m_outputStream` (bridges library output → transport send)
   - Exposes `feedByte(uint8_t)` / `feedString(const char*, size_t)` — feeds into `m_manager.read()`
   - Exposes `registerAxis(TCodeAxis*)` — registers axis with manager
   - Exposes `getChannelPosition(TCodeAxis*)`, `getAxisLastCommandTime(TCodeAxis*)`
   - Exposes `setAxisData(...)` for initial axis state
   - Exposes `setMessageCallback()` — wires firmware/device notification callbacks
   - Exposes `getDeviceSettings(char* buf)` — enumerates all axes
3. **Create `include/tcode/TcodeAxisRegistry.h`**: A registry of all axis objects the device exposes:
   ```cpp
   TCodeAxis* stroke_axis;   // L0 — linear stroke
   TCodeAxis* twist_axis;    // R0 — twist rotation
   TCodeAxis* roll_axis;     // R1 — roll
   TCodeAxis* pitch_axis;    // R2 — pitch
   TCodeAxis* vibe0_axis;    // V0 — vibration channel 0
   // etc.
   ```
   Each axis is created with name, `AxisId`, and default position (0.5f for centered, 0.0f for off).
   Axes are ONLY created if the corresponding hardware pin is configured (per `#if defined` guards).
4. **Refactor `TCodeParser`**: Either:
   - **Replace it** entirely with the adapter (the `<TCode.h>` library IS the parser), OR
   - **Keep it as a thin shim** that wraps the adapter and preserves existing callback interfaces for the motion layer
5. **Update `main.cpp`**:
   - Replace `buttplugLinearCmd` with per-axis position polling in `motionConsumerTask` (or keep the callback but source it from axis positions)
   - Register all available axes with the adapter during setup
   - Wire D0/D1/D2 responses through the adapter's event handler
   - Update the D1 response to `"TCode v0.4\n"`
6. **Update transports** (`SerialTransport`, `WebSocketTransport`, `BleTransport`, `DongleTransport`):
   - Feed bytes into the adapter instead of `TCodeParser`
   - Install response hooks on the adapter's output stream
7. **Update `TransportManager`**: Wire response hooks to the adapter instead of `TCodeParser`.
8. **Update `config_api.h`**: Update `INTIFACE_IDENTIFIER` to `"tcode-v04"` (or keep `"tcode-v03"` for backward compat — v0.4 is wire-compatible).

**Risk**: The `<TCode.h>` library source is not included in the reference repo. It must be located (likely a submodule of jcfain/TCodeESP32 or a separate repo by the same author). If unavailable, fall back to Option B.

---

### Option B: Extend Existing `TCodeParser` (Fallback)

**Approach**: Keep the hand-rolled parser and add v0.4 features incrementally:
1. Add `TCodeAxis`-like state objects (named, typed, with position/timestamp tracking)
2. Add axis registration API
3. Parse R/V/A tokens and dispatch to registered axes
4. Add `AxisRampData` support (parse ramp parameters from the wire format — **note: v0.4 ramp data is NOT on the wire** — it's set programmatically via `setAxisData()`; the wire format is identical to v0.3)
5. Add `GetAssignedAxisValues` device command handling
6. Make D2 response dynamic
7. Add output stream abstraction

**Pros**:
- No external dependency
- Full control over the implementation
- Can be done incrementally

**Cons**:
- Significantly more code to write and maintain
- Must independently verify backward compatibility
- The `<TCode.h>` library may have edge-case handling not obvious from the reference
- Reinventing what the reference already solves

---

### Option C: Port `<TCode.h>` Library Internally

**Approach**: Extract the relevant classes from `<TCode.h>` and incorporate them into SlopDrive-32's own source tree (e.g., `lib/TCode/`), with proper attribution.

**Pros**:
- No external dependency at build time
- Can customize/extend the library for SlopDrive-32's specific needs

**Cons**:
- Forking an external library creates maintenance burden
- Must track upstream changes manually
- License compliance (MIT — attribution required, redistribution OK)

---

## 5. Recommended Implementation Plan (Option A)

### Phase 1: Library Integration & Axis Model

1. Locate the `<TCode.h>` library source (likely a git submodule in jcfain/TCodeESP32 or a standalone repo).
2. Add it to `platformio.ini` as a `lib_deps` entry.
3. Create `include/tcode/TCodeV4Adapter.h` — the bridge class wrapping `TCodeManager`.
4. Create `src/tcode/TCodeV4Adapter.cpp` — implementation.
5. Create `include/tcode/TcodeAxisRegistry.h` — the axis registry struct.
6. Wire the adapter into `main.cpp`:
   - Instantiate `TCodeV4Adapter` (replaces or wraps `TCodeParser`)
   - Create all available axis objects during setup
   - Register axes with the adapter

### Phase 2: Transport Bridge

1. Implement `OutputStream` (or equivalent) that calls the active transport's response callback.
2. Implement `EventHandler` bridge that routes device commands to the response callback.
3. Update all transports to feed bytes into the adapter instead of `TCodeParser`.
4. Update `TransportManager` to install response hooks on the adapter.
5. Update D1 response to `"TCode v0.4"`.
6. Make D2 response dynamic via `getDeviceSettings()`.

### Phase 3: Motion Integration

1. Create per-axis position polling in the motion execution path:
   - L0 (stroke) → continues to drive the primary linear axis (existing pipeline)
   - R0 (twist) → if pin configured, drive twist servo
   - V0–V3 (vibration) → if pins configured, drive vibration PWM
   - A0–A3 (auxiliary) → if pins configured, drive aux outputs
2. Wire `getChannelPosition()` and `getAxisLastCommandTime()` for vibe timeout logic.
3. Preserve the existing waypoint queue pipeline for the primary L0 axis (it handles the high-rate streaming with OSSM-exact pacing and latency compensation — do NOT lose this).

### Phase 4: Backward Compatibility Verification

1. Test with MultiFunPlayer (spec-correct v0.3 frames) — verify identical behavior.
2. Test with Intiface (buttplug bridge, potentially non-spec frames) — verify Intiface compat flag still works.
3. Test DSTOP, D0, D1, D2 responses.
4. Verify that v0.4 devices advertising `"TCode v0.4"` can still be controlled by v0.3-only hosts (v0.4 is wire-compatible with v0.3).

### Phase 5: OSSM BLE Considerations

The OSSM BLE service (`OssmBleService`) is a completely separate protocol — it does NOT use TCode. It implements the OSSM command set (`set:`, `go:`, `stream:`) for third-party app compatibility. This service is UNCHANGED by the TCode v0.4 upgrade. The `TransportManager::applyTransport(OSSM_BLE)` path remains independent.

---

## 6. Files That Need Changes (Full Inventory)

### New Files to Create

| File | Purpose |
|------|---------|
| `include/tcode/TCodeV4Adapter.h` | Bridge class wrapping `TCodeManager` from `<TCode.h>` |
| `src/tcode/TCodeV4Adapter.cpp` | Implementation |
| `include/tcode/TcodeAxisRegistry.h` | Axis registry struct (all available axes) |
| `include/tcode/OutputStreamBridge.h` | OutputStream implementation that routes to transport callback |
| `include/tcode/EventHandlerBridge.h` | EventHandler implementation that routes device commands to callback |

### Files to Modify

| File | Change |
|------|--------|
| `platformio.ini` | Add `<TCode.h>` library dependency |
| `include/system/config_api.h` | Update `INTIFACE_IDENTIFIER` from `"tcode-v03"` to `"tcode-v04"` (or keep both for compat) |
| `include/comms/TCodeParser.h` | Either extend with v0.4 axis model OR deprecate in favor of adapter |
| `src/comms/TCodeParser.cpp` | Either extend OR deprecate |
| `src/main.cpp` | Replace/refactor TCode wiring: axis registration, callback wiring, D1/D2 updates |
| `include/comms/TransportManager.h` | Wire response hooks to adapter instead of TCodeParser |
| `src/comms/TransportManager.cpp` | Updated `applyTransport()` |
| `include/comms/SerialTransport.h` | Feed bytes to adapter |
| `src/comms/SerialTransport.cpp` | Implementation |
| `include/comms/WebSocketTransport.h` | Feed bytes to adapter |
| `src/comms/WebSocketTransport.cpp` | Implementation |
| `include/comms/BleTransport.h` | Feed bytes to adapter |
| `src/comms/BleTransport.cpp` | Implementation |
| `include/comms/DongleTransport.h` | Feed bytes to adapter |
| `src/comms/DongleTransport.cpp` | Implementation |
| `include/motion/MotorDriver.h` | Potentially add per-axis position hooks (if multi-axis motor control is desired) |
| `include/motion/PatternEngine.h` | May need axis channel references for vibe/twist control |
| `reference/ossm/` (or new `reference/tcode/`) | Add v0.4 reference documentation |

### Files That Do NOT Change

| File | Reason |
|------|--------|
| `include/comms/OssmBleService.h/.cpp` | Separate protocol, unaffected |
| `lib/SharedProtocol/SharedProtocol.h` | Internal node-network protocol, unrelated to TCode |
| `lib/StrokeEnginePatterns/` | Pattern generation, unrelated |
| `include/motion/57AIMServoDriver.h` / `TMC2160StepperDriver.h` | Driver-level, unchanged |
| `src/motion/PatternEngine.cpp` | Pattern logic, unchanged (patterns generate positions — those feed the L0 axis) |
| `include/ui/` | WebUI, unchanged (D1 version reported via API) |
| `webui/` | Frontend, unchanged (TCode version is opaque to the UI) |
| `test/` | Test suite, may need new tests for v0.4 parsing |

---

## 7. Key Design Decisions to Make Before Implementation

1. **`<TCode.h>` library availability**: Can the library be found and added as a dependency? If not, fall back to Option B or C.

2. **Parser replacement vs. extension**: Do we replace `TCodeParser` entirely with the `<TCode.h>` adapter, or keep `TCodeParser` as a thin shim that wraps the adapter? Recommendation: **Replace** — the `<TCode.h>` library IS the parser; a wrapper shim adds indirection without benefit.

3. **Axis registration granularity**: Which axes does SlopDrive-32 expose?
   - **Always**: L0 (stroke) — the primary linear axis
   - **Conditional** (based on pin config / build flags):
     - R0 (twist) — if twist servo pin configured
     - R1 (roll) — if roll servo configured (SR6/OSR kinematics)
     - R2 (pitch) — if pitch servo configured
     - V0–V3 (vibration) — if vibration PWM pins configured
     - A0 (valve) — if valve servo configured
     - A1 (suck) — if valve pump configured
     - A2 (lube) — if lube pump configured
     - A3 (squeeze) — if squeeze servo configured

4. **Motion execution model**: Keep the current push-through-waypoint-queue model for L0 (it's battle-tested and performs at 100+ Hz), and add per-axis polling for auxiliary axes (R/V/A) in the `motionConsumerTask` or a separate per-axis update task. The v0.4 reference polls all axes synchronously in `execute()` — SlopDrive-32's dual-core architecture should keep the L0 fast path on Core 1 and auxiliary axes on Core 0 or a lower-priority Core 1 task.

5. **Ramp profiles (AxisRampData)**: The v0.4 ramp-in/ramp-out data is SET programmatically via `setAxisData()` — it controls how the position interpolates between the previous value and the new commanded value. Currently SlopDrive-32's `motionConsumerTask` does its own trapezoidal planning via `kinematics::planTrapezoid()`. If `AxisRampData` is to be honored, the planning function needs to accept ramp coefficients. **Recommendation**: For the initial v0.4 implementation, accept the `AxisRampData` in the API but defer full ramp-profile integration — the current trapezoidal planner is already a high-quality motion profile. This can be a Phase 2 enhancement.

6. **Intiface identifier**: Keep `"tcode-v03"` or update to `"tcode-v04"`? The Intiface device config JSON (`intiface/slopdrive32-device-config.json`) currently advertises `tcode-v03`. Since v0.4 is backward compatible, we can advertise `tcode-v04` and Intiface should still work (the wire format is identical). **Recommendation**: Update to `"tcode-v04"` and update the Intiface config.

---

## 8. Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| `<TCode.h>` library unavailable or unbuildable | Medium | High — forces Option B/C | Verify library source before starting |
| `<TCode.h>` library uses blocking calls (violates `.clinerules`) | Low | Medium | Audit library source; wrap in FreeRTOS task if needed |
| v0.4 parser changes magnitude decode behavior | Low | High — breaks existing streams | Test with MFP + Intiface before releasing |
| Multi-axis registration breaks existing single-axis apps | Low | Medium | v0.4 D2 response properly advertises available axes; hosts ignore unknown axes |
| Memory overhead of `TCodeManager` + axis objects | Medium | Low | ESP32-S3 has ample PSRAM; axis objects are small |
| `<TCode.h>` namespace conflicts | Low | Low | No `TCode` namespace currently in SlopDrive-32 |

---

## 9. Summary

SlopDrive-32 currently implements a hand-rolled TCode v0.3 parser with a single L0 axis callback. Upgrading to v0.4 requires:

1. **An axis state model** — persistent `TCodeAxis` objects with position, timestamp, and ramp data.
2. **Multi-axis support** — parse and dispatch R/V/A tokens to registered axis objects.
3. **Device command observer pattern** — structured dispatch for DSTOP, D0, D1, D2, and the new GetAssignedAxisValues.
4. **Output stream abstraction** — decouple response delivery from the parser.
5. **Dynamic D2 enumeration** — report all registered axes with their channel ranges.

The cleanest path is **Option A**: integrate the `<TCode.h>` external library that the reference v0.4 implementation already uses. This gives us a proven, backward-compatible v0.4 parser with ramp profiles, multi-axis dispatching, and observer events out of the box. The library's `TCodeManager` becomes the parser; SlopDrive-32's transports feed bytes into it, and the motion layer polls axis positions from it.

If the `<TCode.h>` library cannot be located or integrated, **Option B** (extend the existing parser) is a viable fallback with more implementation effort.

**Estimated implementation effort**:  
- Option A: 3–5 days (library integration + adapter + transport wiring + testing)  
- Option B: 7–10 days (full parser extension + axis model + observer pattern + testing)