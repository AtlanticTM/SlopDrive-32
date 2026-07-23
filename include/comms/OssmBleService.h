#pragma once

// OSSM BLE GATT Service — SlopDrive-32 masquerade
//
// Implements the complete OSSM BLE protocol contract (see GLOBAL CONTEXT in
// PLAN_OSSM_Parity.md) so third-party apps (OSSM Possum, XToys) can control
// SlopDrive-32 as if it were a stock KinkyMakers OSSM.
//
// NimBLE callbacks run on the NimBLE host task (Core 0). Motion-affecting
// values are handed to PatternEngine setters (volatile fields) — never acted
// on inline with blocking work. No delay(), no cross-core directly.

#include <Arduino.h>

class PatternEngine;
class SystemState;
class RangeMapper;
class NimBLEServer;
class NimBLEService;
class NimBLECharacteristic;
class NimBLEAdvertising;

// OSSM state as emitted in the state JSON char 2000
enum class OssmState : uint8_t {
    MENU_IDLE = 0,
    STROKEENGINE_IDLE,
    STROKEENGINE_RUNNING,
    SIMPLEPENETRATION_IDLE,
    STREAMING,
    HOMING,
    ERROR_IDLE
};

class OssmBleService {
public:
    OssmBleService(SystemState& state, PatternEngine& patternEngine,
                   RangeMapper& mapper);
    ~OssmBleService();

    // ---- Lifecycle -----------------------------------------------------------
    void init();        // create NimBLE objects (call once in setup)
    void start();       // start advertising, register service
    void stop();        // teardown advertising + service, free NimBLE
    void update();      // heartbeat notify, disconnect ramp (call from Core 0)
    void emergencyStop(); // stop motion, keep BLE up

    bool isRunning() const { return _running; }

    // ---- Accessors for NimBLE callback classes (they need these at compile time) --
    bool getKnobLimit()   const { return _knobLimit; }
    void setKnobLimit(bool v) { _knobLimit = v; }
    bool getLatencyComp() const { return _latencyComp; }
    void setLatencyComp(bool v) { _latencyComp = v; }
    int  getDescCacheIdx() const { return _descCacheIdx; }
    void setDescCacheIdx(int idx) { _descCacheIdx = idx; }

    String buildStateJson();
    static const char* descriptionForIndex(int idx);

    // ---- Internals exposed for NimBLE callback shims (see .cpp) ------------
    // Called from the shim classes; do NOT call from application code.
    void _onConnect(uint16_t connHandle);
    void _onDisconnect(uint16_t connHandle, int reason);
    void _onCommandWrite(NimBLECharacteristic* chr);

private:
    SystemState&    _state;
    PatternEngine&  _patternEngine;
    RangeMapper&    _mapper;

    // NimBLE objects — allocated in init(), freed in stop()
    NimBLEServer*       _server      = nullptr;
    NimBLEService*      _svc         = nullptr;
    NimBLECharacteristic* _charCmd   = nullptr;  // 1000
    NimBLECharacteristic* _charKnob  = nullptr;  // 1010
    NimBLECharacteristic* _charLat   = nullptr;  // 1030
    NimBLECharacteristic* _charState = nullptr;  // 2000
    NimBLECharacteristic* _charList  = nullptr;  // 3000
    NimBLECharacteristic* _charDesc  = nullptr;  // 3010
    NimBLEAdvertising*   _adv         = nullptr;

    // ---- State ---------------------------------------------------------------
    volatile OssmState  _ossmState     = OssmState::MENU_IDLE;
    volatile bool       _running       = false;
    volatile bool       _clientConnected = false;
    uint32_t            _lastNotifyMs   = 0;

    // ---- Knob / latency stubs ------------------------------------------------
    volatile bool       _knobLimit    = true;   // default true (like stock OSSM)
    volatile bool       _latencyComp  = false;  // default false (like stock OSSM)

    // ---- Disconnect safety ramp ----------------------------------------------
    // When client disconnects mid-motion: 1s grace, then 2s ease-in-out-sine ramp.
    uint32_t            _discoStartMs   = 0;
    volatile bool       _ramping        = false;
    float               _rampStartSpeed = 0.0f;
    volatile int        _descCacheIdx   = -1;     // pattern idx written to 3010

    // Command parser helpers (called from _onCommandWrite)
    void _handleSet(const char* cmd);
    void _handleGo(const char* cmd);
    void _handleStream(const char* cmd);
    void _respond(const char* response);

    // Build state JSON for char 2000
    String _buildStateJson();

    // Compute the disconnect ramp speed factor (0..1)
    float _rampFactor(uint32_t nowMs);

    // OSSM pattern descriptions (matching BLE_PROTOCOL.md table)
    static const char* _descriptionForIndex(int idx);

    uint16_t _lastConnHandle = 0;
    friend class OssmServerCallbacks;
    friend class OssmCmdCallbacks;
    friend class OssmKnobCallbacks;
    friend class OssmLatCallbacks;
    friend class OssmStateCallbacks;
    friend class OssmListCallbacks;
    friend class OssmDescCallbacks;
};