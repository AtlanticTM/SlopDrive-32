# Intiface device config for SlopDrive-32

`slopdrive32-device-config.json` teaches Intiface Central to recognize the
machine as a **TCode v0.3** linear device. It's still plain TCode — the JSON just
maps the device's name + Bluetooth UUIDs onto Intiface's built-in `tcode-v03`
protocol so the position commands flow through unchanged.

## Loading it

1. Intiface Central → **Settings** → **Device Config File** (older builds: the
   Advanced/gear page has a *User Device Config* file picker).
2. Select `slopdrive32-device-config.json`. Restart the engine if prompted.
3. **Start Scanning.**

## Per-transport notes

| Web UI mode | What to do in Intiface |
|-------------|------------------------|
| **Bluetooth** | Pick *Bluetooth* in the web UI (Settings → Connection), then Start Scanning. `SlopDrive-32` appears as a BLE device. **This is the transport that actually needs this file** — Intiface can't auto-detect a custom BLE service otherwise. |
| **WebSocket** | Add a *Websocket* device, protocol `TCode v0.3`, address `ws://<esp32-ip>:55555`. |
| **Serial** | Add a *Serial* device on the ESP32's COM port. |

## Keep it in sync with the firmware

The UUIDs, name, and range in the JSON **must** match `include/system/config_api.h`:

| JSON | `config_api.h` |
|------|-----------|
| name `SlopDrive-32` | `BLE_DEVICE_NAME` |
| service `8a846175-…71` | `BLE_NUS_SERVICE_UUID` |
| `tx` (Intiface→device write) `8a846175-…72` | `BLE_NUS_RX_CHAR_UUID` |
| `rx` (device→Intiface notify) `8a846175-…73` | `BLE_NUS_TX_CHAR_UUID` |
| identifier `slopdrive32-0001` | `INTIFACE_ADDRESS` |
| step range `[0, 999]` | `TCODE_MAGNITUDE_MAX` (999) |

> Note the deliberate tx/rx swap: Intiface's `tx` endpoint is the characteristic
> it *writes* to, which is the device's **RX** characteristic, and vice-versa.

Intiface always sends the full `0..999` range. Your Stroke Window, end trims and
max-speed limits are applied **on the device** afterward, so changing them in the
web UI never requires touching this file.
