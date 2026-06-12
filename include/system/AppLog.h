// In-memory log buffer - SlopDrive-32
//
// The USB Serial port is dedicated to Intiface TCode (serial control mode), so
// debug output must NOT go to Serial or it would corrupt the command stream.
// Instead, log lines are stored in a small RAM ring buffer and viewed through
// the web UI (GET /api/log). Use applogf() exactly like Serial.printf().
#ifndef APPLOG_H
#define APPLOG_H

#include <Arduino.h>
#include "config_api.h"

// ---- Shared logging macros (used by every .cpp that wants to log) ---------
// In serial-control mode the USB Serial port is dedicated to Intiface TCode,
// so status/debug must go to the in-memory web log (applog), NOT Serial.
// Otherwise log normally to Serial.  Include this header and use APPLOG/APPLOGF.
// If your file needs different names (e.g. the TMC driver uses MLOGF/MLOGLN),
// define your own narrow macros — the underlying applog/applogf functions are
// still available.
#if SERIAL_CONTROL_MODE
  #define APPLOG(s)      applog(s)
  #define APPLOGF(...)   applogf(__VA_ARGS__)
#else
  #define APPLOG(s)      Serial.println(s)
  #define APPLOGF(...)   Serial.printf(__VA_ARGS__)
#endif

// Capture a printf-style log line into the ring buffer (thread-safe enough for
// our use: short critical section guarded by a mutex). Never writes to Serial.
void applogf(const char* fmt, ...);

// Append a plain string line.
void applog(const char* line);

// Concatenate all buffered lines (oldest first) into the provided String.
void applogDump(String& out);

// Initialize the mutex. Call once in setup() before any logging.
void applogBegin();

#endif  // APPLOG_H