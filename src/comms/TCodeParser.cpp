// TCode v0.3 transport-agnostic parser — implementation

#include "TCodeParser.h"
#include "AppLog.h"
#include "config_api.h"

// ============================================================================
// feedLine — parse one or more whitespace-separated TCode commands
// ============================================================================
//
// Copied verbatim from ButtplugServer::parseTCode() and adapted to use
// injected callbacks instead of direct _onLinearCmd / _onStop / sendResponse
// members.  Logic is character-for-character identical to the original
// (Step 8 rule: behaviour must not change).

void TCodeParser::feedLine(const char* str, size_t len) {
    // Count the raw frame BEFORE parsing for rate diagnostics.
    rxFrameCount++;

    // Null-terminated working copy (TCode lines are short).
    char buf[256];
    size_t copy_len = min(len, sizeof(buf) - 1);
    memcpy(buf, str, copy_len);
    buf[copy_len] = '\0';

    char* token = strtok(buf, " \t\r\n");
    while (token != nullptr) {
        size_t tlen = strlen(token);
        if (tlen == 0) {
            token = strtok(nullptr, " \t\r\n");
            continue;
        }

        char axis = toupper(token[0]);

        // ---- Device commands (D...) ----
        if (axis == 'D') {
            if (strncasecmp(token, "DSTOP", 5) == 0) {
                applog("[TCode] DSTOP - stop motion");
                if (_onStop) _onStop();
            } else if (strncasecmp(token, "D0", 2) == 0) {
                if (_onResponse) _onResponse("D0 SlopDrive-32 1.0\n");
            } else if (strncasecmp(token, "D1", 2) == 0) {
                if (_onResponse) _onResponse("D1 TCode v0.3\n");
            } else if (strncasecmp(token, "D2", 2) == 0) {
                if (_onResponse) _onResponse("D2 L0 0 9999 Up\n");
            }
            token = strtok(nullptr, " \t\r\n");
            continue;
        }

        // ---- Linear / Rotation / Vibration axes (we only use L0) ----
        if (axis == 'L' || axis == 'R' || axis == 'V') {
            if (tlen < 2) {
                token = strtok(nullptr, " \t\r\n");
                continue;
            }

            uint8_t channel = token[1] - '0';

            // Only handle linear axis 0
            if (axis != 'L' || channel != 0) {
                token = strtok(nullptr, " \t\r\n");
                continue;
            }

            // --- Decode magnitude (FIXED scale, NOT digit-count based) ---
            const char* p = token + 2;
            uint32_t mag_value = 0;
            int mag_digits = 0;
            while (*p && isdigit((unsigned char)*p)) {
                mag_value = mag_value * 10 + (uint32_t)(*p - '0');
                mag_digits++;
                p++;
            }

            if (mag_digits == 0) {
                token = strtok(nullptr, " \t\r\n");
                continue;
            }

            float position = (float)mag_value / TCODE_MAGNITUDE_MAX;
            position = constrain(position, 0.0f, 1.0f);

            // --- Optional modifier: I (interval ms) or S (speed units/100ms) ---
            uint32_t duration_ms = 0;

            if (*p == 'I' || *p == 'i') {
                p++;
                duration_ms = (uint32_t)atoi(p);
            } else if (*p == 'S' || *p == 's') {
                p++;
                long rate = atol(p);
                if (rate > 0) {
                    duration_ms = (uint32_t)(100000L / rate);
                }
            }

            // Rate-limit this log to ~2/sec (same as original).
            static uint32_t s_last_log_ms = 0;
            uint32_t now_log = millis();
            if (now_log - s_last_log_ms >= 500) {
                s_last_log_ms = now_log;
                applogf("[TCode] L0: pos=%.4f dur=%ums (digits=%d)",
                        position, duration_ms, mag_digits);
            }

            if (_onLinearCmd) {
                _onLinearCmd(position, duration_ms);
            }
        }

        token = strtok(nullptr, " \t\r\n");
    }
}