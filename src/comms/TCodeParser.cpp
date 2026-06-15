// TCode v0.3 transport-agnostic parser — the dirty-talk translator :3
// Takes whatever filthy whispers Intiface sends and turns them into
// something the motor can actually understand.

#include "TCodeParser.h"
#include "AppLog.h"
#include "config_api.h"

// Intiface compat flag — default OFF so MultiFunPlayer (spec-correct decode)
// works out of the box. The WebUI flips this on when you're driving from
// Intiface and it's ramming mangled magnitudes at us. :3
volatile bool TCodeParser::intifaceCompat = false;

// ============================================================================
// feedLine — parse one or more whitespace-separated TCode commands
// ============================================================================
//
// Copied verbatim from ButtplugServer::parseTCode() and adapted to use
// injected callbacks instead of direct _onLinearCmd / _onStop / sendResponse
// members.  Logic is character-for-character identical to the original
// (Step 8 rule: behaviour must not change).

void TCodeParser::feedLine(const char* str, size_t len) {
    // Every frame that arrives is another pulse of Intiface's filthy stream
    // flooding into the parser's greedy throat. Count it BEFORE we swallow
    // so the rate diagnostic sees exactly how hard the host is pumping us. :3
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

            // --- Decode magnitude (TCode v0.3 fractional, digit-count based) ---
            // In TCode v0.3 the magnitude after the channel is a DECIMAL FRACTION
            // with an implied leading "0." — the number of digits IS the scale.
            // So L0500 = 0.500, L05000 = 0.5000, L09999 = 0.9999, etc. The spec
            // puts NO upper bound on digit count, so a horny app can ram us with
            // as much fractional precision as it likes (L0500000…). We must NOT
            // assume a fixed 3-digit (TCODE_MAGNITUDE_MAX=999) scale — that was
            // the bug: a 5-digit value like 50000 / 999 = 50.0, clamped to 1.0,
            // which made every fast high-precision stroke slam to the wall. :3
            //
            // Fix: divide by 10^digits so the scale always matches what we were
            // actually given. We cap accumulation at TCODE_MAGNITUDE_MAX_DIGITS
            // (truncating any excess digits — float can't resolve them anyway,
            // ~7 significant figures) so a pathologically long magnitude can
            // never overflow uint32 or stall the parse loop. Extra digits past
            // the cap are read and discarded, keeping the divisor in lockstep
            // with the digits we kept. We take exactly what we can handle and
            // spit the rest out — no choking, no overflow. :3
            const char* p = token + 2;
            uint32_t mag_value = 0;
            int mag_digits = 0;        // digits we actually KEPT (sets the scale)
            while (*p && isdigit((unsigned char)*p)) {
                if (mag_digits < TCODE_MAGNITUDE_MAX_DIGITS) {
                    mag_value = mag_value * 10 + (uint32_t)(*p - '0');
                    mag_digits++;
                }
                // else: excess precision — read past it but don't let it touch
                // mag_value or the scale. Truncated, drained, ignored. :3
                p++;
            }

            if (mag_digits == 0) {
                token = strtok(nullptr, " \t\r\n");
                continue;
            }

            // --- Pick the divisor: spec-correct vs Intiface compat ------------
            // DEFAULT (intifaceCompat == false): divide by 10^digits so the
            // scale always matches the digit count — the MFP / TCode v0.3 way.
            //
            // INTIFACE COMPAT (intifaceCompat == true): Intiface's buttplug
            // bridge does fuckshit and emits magnitudes meant to be read against
            // the legacy fixed /TCODE_MAGNITUDE_MAX (999) ceiling. Under the
            // digit-count decode those land shallow and the hole never fully
            // gapes. Flip the toggle in the WebUI and we scale against the fixed
            // ceiling instead so Intiface strokes go balls-deep again. :3
            float position;
            if (intifaceCompat) {
                position = (float)mag_value / TCODE_MAGNITUDE_MAX;
            } else {
                // Scale = 10^mag_digits. Integer power of ten so the fraction is
                // exact for the digits we kept (no fixed-magic divisor).
                float scale = 1.0f;
                for (int d = 0; d < mag_digits; d++) scale *= 10.0f;
                position = (float)mag_value / scale;
            }
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