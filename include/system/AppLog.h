// AppLog — compatibility shim over SlopLog (lib/sloplog).
//
// Historically this was its own mutex'd line ring for the web UI. It is now
// a thin facade: APPLOG/APPLOGF and applog/applogf feed the SlopLog core
// (level Info, tag "app"), and the web ring (/api/log) plus Serial are just
// SlopLog SINKS registered in applogBegin(). SERIAL_CONTROL_MODE no longer
// branches the macros — it decides whether the Serial sink gets registered
// (the USB port stays clean for Intiface TCode either way).
//
// New code should prefer SLOGx("tag", ...) from <sloplog/sloplog.h> directly
// — real levels, tags, and per-call-site throttling (SLOGW_EVERY_MS). These
// macros exist so 200+ legacy call sites keep working during the migration.
#ifndef APPLOG_H
#define APPLOG_H

#include <Arduino.h>

#include "config_api.h"
#include "sloplog/sloplog.h"

#define APPLOG(s)      applog(s)
#define APPLOGF(...)   applogf(__VA_ARGS__)

// printf-style, level Info, tag "app". Never blocks; bounded truncation.
void applogf(const char* fmt, ...);

// Plain string line, level Info, tag "app".
void applog(const char* line);

// Concatenate the web ring's buffered lines (oldest first) for /api/log.
void applogDump(String& out);

// Register the SlopLog sinks (web ring always; Serial unless
// SERIAL_CONTROL_MODE). Call once in setup(). Records logged before this is
// called are buffered in the SlopLog core and flow out on the first drain.
void applogBegin();

// Pump: fan buffered records out to the sinks. Call from httpTask (Core 0).
void applogDrain();

#endif  // APPLOG_H
