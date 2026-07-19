// CurrentSensor — INA228 driver implementation for the 36V motor bus.
// Build-guarded behind DRIVER_AIM_SERVO.
//
// The register map differs from the INA226 — this uses RobTillaart/INA228.
// We drink the current straight off the 5mΩ shunt: full travel is a thirsty
// gulp, a stall is the drive choking as the carriage stuffs itself against the
// hard stop until it can't take another step. yippie! :3
#if defined(DRIVER_AIM_SERVO)

#include "CurrentSensor.h"
#include <Wire.h>
#include <INA228.h>
#include "config_api.h"
#include "AppLog.h"

// Route debug the same way the servo driver does — keep the USB TCode stream
// clean when SERIAL_CONTROL_MODE is on. :3
#if SERIAL_CONTROL_MODE
  #define CLOGF(...)  applogf(__VA_ARGS__)
  #define CLOGLN(s)   applog(String(s).c_str())
#else
  #define CLOGF(...)  Serial.printf(__VA_ARGS__)
  #define CLOGLN(s)   Serial.println(s)
#endif

// Single INA228 instance at 0x40 on the default Wire bus. The isolator is
// transparent, so this is just a normal I2C device to us. :3
static INA228 _ina(INA228_I2C_ADDR);

bool CurrentSensor::init() {
    // Wire.begin(SDA, SCL) is the CALLER's job (main setup) — we assume the bus
    // is already up. begin() just probes whether the chip acknowledges. :3
    if (!_ina.begin()) {
        CLOGF("INA228: NOT FOUND at 0x%02X — current sensing DISABLED, homing will refuse. uhoh :3\n",
              INA228_I2C_ADDR);
        _ready = false;
        return false;
    }

    // Calibrate: max current 32.768A across the 5mΩ shunt. The library computes
    // SHUNT_CAL from these; per NewPCB §5 this must land on 4096 exactly. We
    // feed it the full-scale numbers and let it do the math. :3
    // INA228 v0.3.x setMaxCurrentShunt(maxCurrent, shunt) — no normalize arg on
    // this release. With maxCurrent = 32.768A across the 5mΩ shunt the library
    // derives CURRENT_LSB = 32.768/2^19 = 62.5µA and SHUNT_CAL = 4096 exactly,
    // no rounding needed — the numbers are already the natural full-scale fit. :3
    int rc = _ina.setMaxCurrentShunt(INA228_MAX_CURRENT_A, INA228_SHUNT_OHMS);

    if (rc != 0) {
        CLOGF("INA228: setMaxCurrentShunt() returned %d (non-zero = warning) :3\n", rc);
    }

    // ADCRANGE = 0 -> ±163.84mV full scale. Do NOT use range 1 (±40.96mV) — it
    // clips at ~8A, well below the motor's 16-20A ceiling and we'd never see a
    // real stall. Range 0 lets us feel the full 32.768A gape. :3
    _ina.setADCRange(0);

    // Averaging + conversion time: a touch of averaging cleans the reading for
    // the force loop without killing our poll rate. 16-sample average keeps us
    // comfortably above the ~150Hz homing poll. Tune vs noise on real hardware.
    _ina.setAverage(2);              // 2 => 16 samples averaged (library enum)
    _ina.setBusVoltageConversionTime(4);   // ~150µs class
    _ina.setShuntVoltageConversionTime(4); // ~150µs class

    _ready = true;
    CLOGF("INA228: ready @ 0x%02X — bus=%.1fV current=%.2fA (should be ~36V, ~0A idle) :3\n",
          INA228_I2C_ADDR, readBusV(), readCurrentA());
    return true;
}

float CurrentSensor::readCurrentA() {
    if (!_ready) return 0.0f;
    // getCurrent() returns amps directly (library applies CURRENT_LSB). :3
    float a = (float)_ina.getCurrent();
    _last_current_a = a;   // stash for the I2C-free cached read the WebUI sips

    // Track the peak |current| seen since boot / since the last resetPeaks().
    // Homing and normal strokes both feed this — the operator can glance at
    // the Health tab after a session and see exactly how hard it strained. :3
    float mag = fabsf(a);
    if (mag > _peak_current_a) _peak_current_a = mag;

    return a;
}

float CurrentSensor::readBusV() {
    if (!_ready) return 0.0f;
    // getBusVoltage() returns volts directly. VBUS is tied to the shunt high
    // side so this reads the real 36V rail — free telemetry. :3
    float v = (float)_ina.getBusVoltage();
    _last_bus_v = v;       // stash for the cached cross-core read :3
    return v;
}

float CurrentSensor::readPowerW() {
    if (!_ready) return 0.0f;
    // getPower() returns watts directly — the chip multiplies V×I on-die from
    // the same registers we already calibrated for current. Free telemetry. :3
    float w = (float)_ina.getPower();
    _last_power_w = w;

    if (w > _peak_power_w) _peak_power_w = w;

    return w;
}

float CurrentSensor::readDieTempC() {
    if (!_ready) return 0.0f;
    // getTemperature() returns the INA228's own die temp in °C — a free health
    // signal on the isolated dirty side of the board. Not the motor, but if
    // this chip is cooking, something nearby probably is too. :3
    float t = (float)_ina.getTemperature();
    _last_die_temp_c = t;
    return t;
}

float CurrentSensor::readShuntV() {
    if (!_ready) return 0.0f;
    // getShuntVoltage() returns volts directly — the raw signal the current
    // reading is derived from. Mostly a diagnostic sanity-check value. :3
    float v = (float)_ina.getShuntVoltage();
    _last_shunt_v = v;
    return v;
}

float CurrentSensor::readEnergyWh() {
    if (!_ready) return 0.0f;
    // getWattHour() reads the chip's own 40-bit hardware energy accumulator
    // (integrated continuously in hardware since power-up or the last
    // RSTACC) and converts Joules -> Wh. Library returns double for the
    // extra headroom on the 40-bit register; we truncate to float since that's
    // plenty of precision for a session Wh readout. :3
    float wh = (float)_ina.getWattHour();
    _last_energy_wh = wh;
    return wh;
}

void CurrentSensor::resetPeaks() {
    // Clear the software peak trackers...
    _peak_current_a = 0.0f;
    _peak_power_w   = 0.0f;

    // ...AND the chip's own hardware energy/charge accumulators, so a fresh
    // home/session starts the Wh counter back at zero too. setAccumulation(1)
    // is the INA228's RSTACC bit (register 0) — momentary reset, self-clearing.
    // Only touch the hardware if the chip actually answered init(). :3
    if (_ready) {
        _ina.setAccumulation(1);
        _last_energy_wh = 0.0f;
    }
}


#endif // defined(DRIVER_AIM_SERVO)
