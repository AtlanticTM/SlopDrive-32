// In-memory log buffer - SlopDrive-32
//
// The USB Serial port is dedicated to Intiface TCode (serial control mode), so
// debug output must NOT go to Serial or it would corrupt the command stream.
// Instead, log lines are stored in a small RAM ring buffer and viewed through
// the web UI (GET /api/log). Use applogf() exactly like Serial.printf().
#ifndef APPLOG_H
#define APPLOG_H

#include <Arduino.h>

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
