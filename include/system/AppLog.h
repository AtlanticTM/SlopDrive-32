// AppLog — the SlopLog sink bridge for the S3 main controller.
//
// The migration to SlopLog is complete: all firmware logging goes through
// SLOGx("tag", ...) from <sloplog/sloplog.h> (real levels, tags, per-call-site
// throttling). The old APPLOG/APPLOGF macros and applog()/applogf() producers
// are GONE — this header now exposes ONLY the plumbing that wires SlopLog's
// output to the two device-specific sinks:
//   - the /api/log web ring (the WebUI's primary log surface), and
//   - the USB Serial handoff (full at boot, Warn+ once the WebUI is receiving).
// SERIAL_CONTROL_MODE decides whether the Serial sink is registered at all
// (the USB port stays clean for Intiface TCode either way).
#ifndef APPLOG_H
#define APPLOG_H

#include <Arduino.h>

#include "config_api.h"
#include "sloplog/sloplog.h"

// Concatenate the web ring's buffered lines (oldest first) for /api/log.
void applogDump(String& out);

// Register the SlopLog sinks (web ring + serial, ALWAYS — serial gating is
// runtime, see below). Call once in setup(). Records logged before this is
// called are buffered in the SlopLog core and flow out on the first drain.
void applogBegin();

// Runtime serial gating: mute the serial sink completely while serial TCode
// traffic is actively flowing (Intiface owns the port), restore when idle.
// Poll from httpTask with serialTransport.isActive(). Composes with
// applogSerialQuiet() (post-handshake Warn+ floor).
void applogSerialDedicated(bool dedicated);

// Pump: fan buffered records out to the sinks. Call from httpTask (Core 0).
void applogDrain();

// The serial handoff: boot logs stream to serial in full; once the WebUI
// proves it is receiving logs (first /api/log serve), serial demotes to
// Warn+ — the web ring is the primary log surface from then on.
void applogSerialQuiet();

#endif  // APPLOG_H
