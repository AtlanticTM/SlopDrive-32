// Bluetooth LE TCode transport — implementation
//
// Extracted verbatim from ButtplugServer in buttplug.cpp (Step 8).
// The entire implementation can be compiled out by omitting -DBLE_ENABLED
// from build_flags (saves ~120K flash when not needed).

#include "BleTransport.h"
#include "AppLog.h"
#include "config_api.h"

#if defined(BLE_ENABLED)

#include <NimBLEDevice.h>

// ---- Static pointer for response-hook thunk ---------------------------------
static BleTransport* s_active_ble = nullptr;

static void _bleTxResponse(const char* msg) {
    if (s_active_ble) s_active_ble->_sendResponse(msg);
}

// ---- Callback shims --------------------------------------------------------
// NimBLE delivers events to subclasses of its callback interfaces.  These thin
// shims just forward into the owning BleTransport instance.

// NimBLE 2.x callbacks: NimBLEConnInfo& in every signature, `override` kept so
// upstream signature drift is a compile error, never a silently-dead shim.
// These thin shims just forward the central's filthy little writes into the
// owning BleTransport. Good pup, takes it and passes it along. :3
class BleServerCallbacks : public NimBLEServerCallbacks {
public:
    explicit BleServerCallbacks(BleTransport* owner) : _owner(owner) {}
    void onConnect(NimBLEServer* /*srv*/, NimBLEConnInfo& /*connInfo*/) override {
        _owner->_onConnect();
    }
    void onDisconnect(NimBLEServer* /*srv*/, NimBLEConnInfo& /*connInfo*/, int /*reason*/) override {
        _owner->_onDisconnect();
        // Resume advertising so the next host can mount us again. :3
        NimBLEDevice::startAdvertising();
    }
private:
    BleTransport* _owner;
};

class BleRxCallbacks : public NimBLECharacteristicCallbacks {
public:
    explicit BleRxCallbacks(BleTransport* owner) : _owner(owner) {}
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& /*connInfo*/) override {
        const NimBLEAttValue v = chr->getValue();
        if (v.size() > 0) {
            _owner->_feedBytes((const uint8_t*)v.data(), v.size());
        }
    }
private:
    BleTransport* _owner;
};

// ---- Constructor -----------------------------------------------------------

BleTransport::BleTransport(TCodeParser& parser)
    : _parser(parser) {}

// ---- Lifecycle --------------------------------------------------------------

void BleTransport::begin() {
    if (_running) return;

    NimBLEDevice::init(BLE_DEVICE_NAME);
    // NimBLE 2.x: setPower() takes dBm directly as int8_t. +9 dBm = the max
    // legal output. (Do NOT pass ESP_PWR_LVL_P9 — the enum silently converts
    // to int8_t and its ORDINAL, not +9, becomes the dBm value. uhoh. :C)
    NimBLEDevice::setPower(9);

    _server = NimBLEDevice::createServer();
    _server->setCallbacks(new BleServerCallbacks(this));

    NimBLEService* svc = _server->createService(BLE_NUS_SERVICE_UUID);

    // RX: host -> device (TCode in). WRITE + WRITE_NR (no response) for speed.
    NimBLECharacteristic* rx = svc->createCharacteristic(
        BLE_NUS_RX_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx->setCallbacks(new BleRxCallbacks(this));

    // TX: device -> host (TCode replies). NOTIFY.
    _tx_char = svc->createCharacteristic(
        BLE_NUS_TX_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);

    // NimBLE 2.x: services auto-start when the server starts — the old explicit
    // svc->start() is a deprecated no-op and is gone. The device name rides
    // along automatically from NimBLEDevice::init(). :3
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(BLE_NUS_SERVICE_UUID);
    adv->enableScanResponse(true);
    NimBLEDevice::startAdvertising();

    _running = true;
    applogf("[BLE] Advertising '%s' (Nordic UART service)", BLE_DEVICE_NAME);
    applog("[BLE] Connect a BLE host and write TCode to the RX characteristic.");
}

void BleTransport::stop() {
    if (!_running) return;
    NimBLEDevice::stopAdvertising();
    NimBLEDevice::deinit(true);
    _server  = nullptr;
    _tx_char = nullptr;
    _running   = false;
    _connected = false;
    applog("[BLE] Stopped");
}

// ---- Connection state (called by callback shims) ----------------------------

void BleTransport::_onConnect()    { _connected = true; }
void BleTransport::_onDisconnect() { _connected = false; }

// ---- Response hooks ---------------------------------------------------------

void BleTransport::installResponseHooks() {
    s_active_ble = this;
    _parser.onResponse(_bleTxResponse);
}

void BleTransport::removeResponseHooks() {
    if (s_active_ble == this) s_active_ble = nullptr;
    _parser.onResponse(nullptr);
}

void BleTransport::_sendResponse(const char* msg) {
    if (_tx_char && _connected) {
        _tx_char->notify((const uint8_t*)msg, strlen(msg));
    }
}

// ---- TCode line assembly (mirrors SerialTransport::poll()) ------------------

void BleTransport::_feedBytes(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            if (_len > 0) {
                _buf[_len] = '\0';
                _linked = true;
                _parser.feedLine(_buf, _len);
                _len = 0;
            }
        } else {
            if (_len < sizeof(_buf) - 1) {
                _buf[_len++] = c;
            } else {
                _len = 0;  // overrun: resync
            }
        }
    }

    // Newline-less host: a single write that didn't end in a terminator is
    // treated as one complete command (the common case for TCode-over-BLE).
    if (_len > 0 && len > 0) {
        char last = (char)data[len - 1];
        if (last != '\n' && last != '\r') {
            _buf[_len] = '\0';
            _linked = true;
            _parser.feedLine(_buf, _len);
            _len = 0;
        }
    }
}

#else  // !BLE_ENABLED — empty stubs so this file still compiles

BleTransport::BleTransport(TCodeParser& parser) : _parser(parser) {}
void BleTransport::begin()                 {}
void BleTransport::stop()                  {}
void BleTransport::_onConnect()            {}
void BleTransport::_onDisconnect()         {}
void BleTransport::installResponseHooks()  {}
void BleTransport::removeResponseHooks()   {}
void BleTransport::_feedBytes(const uint8_t*, size_t) {}
void BleTransport::_sendResponse(const char*) {}

#endif // BLE_ENABLED