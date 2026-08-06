// quad_probe -- open-loop drive tests: does it count runts, does it conserve
// across tiny reversals and at real step rates, does it follow A/B quadrature?
// Constraints:
// - WRITES drive registers on the fenced keys only ('r','w','W','v','z','x',
//   'q','m'). The unattended test runner ('g' and the default path) stays
//   FC 0x03 read-only.
// - MOVES THE MOTOR. Bench use, operator present, carriage clear of both ends.
// - Standalone: no FAS, no arbiter, no motion engine. Bit-bang only, so every
//   pulse width here is EXACT and nothing upstream can shape it.
// - Pin roles: step/dir mode STEP=AIM_PIN_STEP DIR=AIM_PIN_DIR; quadrature mode
//   A=AIM_PIN_STEP (PU-) B=AIM_PIN_DIR (DIR-), per the encoder-following wiring.
// See: dev-board sd-ccy (runt), sd-has (sample-path drift).

#include <Arduino.h>
#include "system/config_api.h"

namespace {

constexpr uint8_t  kSlave    = 1;
constexpr uint16_t kRegEncLo = 0x16;
constexpr uint32_t kBaud     = 19200;

// 32768 encoder counts/motor-rev over 2048 steps/rev = 16 counts per step.
constexpr float kCountsPerStep = 16.0f;
constexpr float kUmPerStep     = 19.2f;
// 2048 steps/motor-rev x 2:1 over a pi*25mm drum = 52.1519 steps/mm.
constexpr float kCountsPerMm   = kCountsPerStep * 52.1519f;

constexpr int32_t kPulses    = 400;   // per test leg; ~7.7 mm at 52.15 steps/mm
constexpr int32_t kRevCycles = 200;   // tiny-reversal cycles
constexpr int32_t kRevSteps  = 4;     // steps per leg of a tiny reversal

uint16_t crc16(const uint8_t* d, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        c ^= d[i];
        for (int b = 0; b < 8; ++b)
            c = (c & 1) ? uint16_t((c >> 1) ^ 0xA001) : uint16_t(c >> 1);
    }
    return c;
}

bool readRegs(uint16_t reg, uint16_t count, uint16_t* out) {
    uint8_t req[8] = {kSlave, 0x03, uint8_t(reg >> 8), uint8_t(reg),
                      uint8_t(count >> 8), uint8_t(count), 0, 0};
    const uint16_t c = crc16(req, 6);
    req[6] = uint8_t(c & 0xFF);
    req[7] = uint8_t(c >> 8);
    while (Serial1.available()) Serial1.read();
    delay(8);                          // RTU inter-frame gap: without it a
    Serial1.write(req, 8);             // back-to-back read returns garbage
    Serial1.flush();

    const size_t need = 5 + 2u * count;
    uint8_t rsp[64];
    if (need > sizeof(rsp)) return false;
    size_t got = 0;
    const uint32_t t0 = millis();
    while (got < need && millis() - t0 < 250)
        if (Serial1.available()) rsp[got++] = uint8_t(Serial1.read());
    if (got < need || rsp[0] != kSlave || rsp[1] != 0x03) return false;
    if (uint16_t(rsp[need - 2] | (rsp[need - 1] << 8)) != crc16(rsp, need - 2)) return false;
    for (uint16_t i = 0; i < count; ++i)
        out[i] = uint16_t((rsp[3 + 2 * i] << 8) | rsp[4 + 2 * i]);
    return true;
}

// ---- The ONE write path in this probe, deliberately fenced ----------------
// Everything else here is FC 0x03. Enabling encoder-following needs reg 0x19=2
// and reg 0x14=1 to persist (ServoModbus::saveToFlash writes 1 to 0x14).
// Reached only by the 'q' key, never by the test runner.
bool writeReg(uint16_t reg, uint16_t val) {
    uint8_t req[8] = {kSlave, 0x06, uint8_t(reg >> 8), uint8_t(reg),
                      uint8_t(val >> 8), uint8_t(val), 0, 0};
    const uint16_t c = crc16(req, 6);
    req[6] = uint8_t(c & 0xFF);
    req[7] = uint8_t(c >> 8);
    while (Serial1.available()) Serial1.read();
    Serial1.write(req, 8);
    Serial1.flush();
    // FC 0x06 echoes the request back; validate rather than fire-and-forget,
    // because a silent failure here looks exactly like "quadrature not supported".
    uint8_t rsp[8];
    size_t got = 0;
    const uint32_t t0 = millis();
    while (got < 8 && millis() - t0 < 300)
        if (Serial1.available()) rsp[got++] = uint8_t(Serial1.read());
    return got == 8 && rsp[0] == kSlave && rsp[1] == 0x06 &&
           uint16_t((rsp[4] << 8) | rsp[5]) == val;
}

bool encoder(int32_t& out) {
    uint16_t r[2];
    if (!readRegs(kRegEncLo, 2, r)) return false;
    out = int32_t((uint32_t(r[1]) << 16) | r[0]);
    return true;
}

// Settle before reading: a moving sample carries Modbus skew and means nothing.
bool settled(int32_t& enc) {
    delay(700);
    return encoder(enc);
}

void stepPulses(int32_t n, bool dirHigh, uint32_t highUs, uint32_t lowUs) {
    digitalWrite(AIM_PIN_DIR, dirHigh ? HIGH : LOW);
    delayMicroseconds(500);            // DIR setup, generous on purpose
    for (int32_t i = 0; i < n; ++i) {
        digitalWrite(AIM_PIN_STEP, HIGH);
        delayMicroseconds(highUs);
        digitalWrite(AIM_PIN_STEP, LOW);
        delayMicroseconds(lowUs);
    }
}

constexpr uint8_t kGray[4] = {0b00, 0b01, 0b11, 0b10};
int8_t g_phase = 0;

void quadTransitions(int32_t transitions, uint32_t dwellUs) {
    const int dir = transitions >= 0 ? 1 : -1;
    const int32_t n = transitions >= 0 ? transitions : -transitions;
    for (int32_t i = 0; i < n; ++i) {
        g_phase = int8_t((g_phase + dir) & 3);
        const uint8_t s = kGray[g_phase];
        digitalWrite(AIM_PIN_STEP, (s & 1) ? HIGH : LOW);
        digitalWrite(AIM_PIN_DIR, (s & 2) ? HIGH : LOW);
        delayMicroseconds(dwellUs);
    }
}

// ---- Test 1: which pulse widths does the drive actually count? --------------
// The runt measured at the pin is 99.5 us wide. If the drive counts a 99.5 us
// pulse the runt is a real step; if not, it is a lost step.
void testPulseWidths() {
    static const uint32_t kWidths[] = {2000, 500, 100, 20, 5, 1};
    Serial.println("");
    Serial.println("--- TEST 1: pulse-width sensitivity ---");
    Serial.println("width_us   expect   measured   counts/pulse   verdict");
    bool dirHigh = false;
    for (uint32_t w : kWidths) {
        int32_t a, b;
        if (!settled(a)) { Serial.println("  encoder read failed"); return; }
        const uint32_t low = (w < 2000) ? (2000 - w) : 200;
        stepPulses(kPulses, dirHigh, w, low);
        if (!settled(b)) { Serial.println("  encoder read failed"); return; }
        const int32_t d = b - a;
        const float per = float(d) / float(kPulses);
        const float frac = fabsf(per) / kCountsPerStep;
        const char* verdict = (frac > 0.9f) ? "COUNTED" : (frac < 0.1f) ? "IGNORED" : "PARTIAL";
        Serial.printf("%8lu %8ld %10ld %14.2f   %s",
                      (unsigned long)w, (long)(kPulses * (dirHigh ? -16 : 16)),
                      (long)d, per, verdict);
        Serial.println("");
        dirHigh = !dirHigh;
    }
}

// ---- Test 1b: conservation at REAL step rates ------------------------------
// Everything else here runs near 1 kHz; the machine runs 5-50 kHz. Nothing in
// this probe clears the drive until this passes. Equal pulses up then down at
// each rate, so any net movement is a rate-dependent miscount.
void testRateSweep() {
    static const uint32_t kPeriodUs[] = {2000, 500, 200, 100, 50, 20};
    Serial.println("");
    Serial.println("--- TEST 1b: conservation vs step RATE ---");
    Serial.println("period_us  step_kHz      fwd      rev      net   um_err  verdict");
    for (uint32_t p : kPeriodUs) {
        const uint32_t half = p / 2;
        int32_t a, b, c;
        if (!settled(a)) { Serial.println("  encoder read failed"); return; }
        stepPulses(kPulses, false, half, p - half);
        if (!settled(b)) { Serial.println("  encoder read failed"); return; }
        stepPulses(kPulses, true, half, p - half);
        if (!settled(c)) { Serial.println("  encoder read failed"); return; }
        const int32_t fwd = b - a, rev = c - b, net = c - a;
        const float um = float(net) / kCountsPerStep * kUmPerStep;
        Serial.printf("%9lu %9.2f %8ld %8ld %8ld %8.1f  %s",
                      (unsigned long)p, 1000.0f / float(p),
                      (long)fwd, (long)rev, (long)net, um,
                      (fabsf(um) < 20.0f) ? "ok" : "LOSING");
        Serial.println("");
    }
}

// ---- Test 2: does step/dir conserve across tiny reversals? -----------------
// The scope dither is 3-4 pulses between direction changes. This reproduces it
// deliberately, with clean exactly-timed pulses.
void testTinyReversals() {
    Serial.println("");
    Serial.println("--- TEST 2: tiny-reversal conservation ---");
    int32_t a, b;
    if (!settled(a)) { Serial.println("  encoder read failed"); return; }
    for (int32_t k = 0; k < kRevCycles; ++k) {
        stepPulses(kRevSteps, false, 500, 500);
        stepPulses(kRevSteps, true,  500, 500);
    }
    if (!settled(b)) { Serial.println("  encoder read failed"); return; }
    const int32_t net = b - a;
    const float umPerRev = float(net) / kCountsPerStep * kUmPerStep / float(kRevCycles * 2);
    Serial.printf("%ld cycles of +%ld/-%ld steps (%ld reversals)",
                  (long)kRevCycles, (long)kRevSteps, (long)kRevSteps,
                  (long)(kRevCycles * 2));
    Serial.println("");
    Serial.printf("net %+ld counts = %+.2f steps = %+.4f um/reversal",
                  (long)net, float(net) / kCountsPerStep, umPerRev);
    Serial.println("");
    // A real encoder never returns exact zero; judge against the drift being
    // hunted (-11.56 um/reversal on the sample path), not against perfection.
    Serial.println(fabsf(umPerRev) < 1.0f
                   ? "VERDICT: step/dir CONSERVES at this rate -- loss is upstream"
                   : "VERDICT: step/dir LOSES at this rate -- loss is at the drive");
}

// ---- Test 3: does the drive follow A/B quadrature on these pins? -----------
// Read counts/transition carefully: 4.00 means the drive is still decoding
// STEP/DIR (A rises once per 4 transitions, 16 counts/step), NOT quadrature.
void testQuadrature() {
    Serial.println("");
    Serial.println("--- TEST 3: A/B quadrature following ---");
    int32_t a, b, c;
    if (!settled(a)) { Serial.println("  encoder read failed"); return; }
    quadTransitions(+kPulses, 2000);
    if (!settled(b)) { Serial.println("  encoder read failed"); return; }
    quadTransitions(-kPulses, 2000);
    if (!settled(c)) { Serial.println("  encoder read failed"); return; }
    const int32_t fwd = b - a, rev = c - b, net = c - a;
    const float perT = float(fwd) / float(kPulses);
    Serial.printf("forward %+ld (%.3f/transition)  reverse %+ld  net %+ld",
                  (long)fwd, perT, (long)rev, (long)net);
    Serial.println("");
    if (fwd == 0 && rev == 0)
        Serial.println("VERDICT: no motion -- not in encoder-following mode");
    else if (fabsf(fabsf(perT) - 4.0f) < 0.5f)
        Serial.println("VERDICT: STEP/DIR decoding (4.00/transition) -- quadrature NOT active");
    else if (fabsf(fabsf(perT) - 16.0f) < 2.0f)
        Serial.println("VERDICT: QUADRATURE ACTIVE (16/transition)");
    else
        Serial.println("VERDICT: unexpected ratio -- read the numbers, not this line");
}

// ---- Test 4: step/dir or quadrature? The counts cannot tell you -----------
// 400 Gray transitions yield 1600 counts under EITHER reading: step/dir sees
// 100 A-rising-edges, x1 quadrature sees 100 full cycles. Same number.
//
// Pulsing A with B held STATIC separates them. Step/dir counts every A edge and
// marches, with B choosing the direction. Quadrature just rattles between two
// adjacent states and nets zero, whatever B is doing.
void testDecodeMode() {
    Serial.println("");
    Serial.println("--- TEST 4: step/dir vs quadrature discriminator ---");
    Serial.println("A pulsed with B static: step/dir MARCHES, quadrature nets ~0");
    int32_t travel[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        const bool bHigh = (i == 1);
        int32_t a, b;
        if (!settled(a)) { Serial.println("  encoder read failed"); return; }
        digitalWrite(AIM_PIN_DIR, bHigh ? HIGH : LOW);
        delayMicroseconds(500);
        for (int32_t k = 0; k < kPulses; ++k) {
            digitalWrite(AIM_PIN_STEP, HIGH);
            delayMicroseconds(1000);
            digitalWrite(AIM_PIN_STEP, LOW);
            delayMicroseconds(1000);
        }
        if (!settled(b)) { Serial.println("  encoder read failed"); return; }
        travel[i] = b - a;
        Serial.printf("B=%s : %+ld counts (%.2f/pulse)",
                      bHigh ? "HIGH" : "LOW ", (long)travel[i],
                      float(travel[i]) / float(kPulses));
        Serial.println("");
    }
    const bool marched = fabsf(float(travel[0])) > float(kPulses) * 8.0f ||
                         fabsf(float(travel[1])) > float(kPulses) * 8.0f;
    const bool flipped = (travel[0] < 0) != (travel[1] < 0);
    // A dead drive produces no travel too. Only call it quadrature if the
    // QUADRATURE test moved the machine; otherwise this is unresponsiveness.
    int32_t qa = 0, qb = 0;
    if (settled(qa)) { quadTransitions(+kPulses, 2000); settled(qb); }
    const bool quadMoved = labs(qb - qa) > kPulses;
    if (marched && flipped)
        Serial.println("VERDICT: STEP/DIR -- A counted as steps, B chose direction");
    else if (!marched && quadMoved)
        Serial.println("VERDICT: QUADRATURE -- static B idle, quadrature moved");
    else if (!marched && !quadMoved)
        Serial.println("VERDICT: NO RESPONSE -- drive ignores BOTH. Not a quadrature result.");
    else
        Serial.println("VERDICT: ambiguous -- read the two numbers above");
}

constexpr uint16_t kEgearNum = 32768;   // 0x0A, from a clean dump
constexpr uint16_t kEgearDen = 2048;    // 0x0B, from a clean dump

// ---- Restore the electronic gear ------------------------------------------
// Zeroing 0x0A to reach the special functions destroys the machine's step
// scaling. This puts it back from values captured with correct frame spacing,
// never from a re-read taken while the gear is already zeroed.
// ---- Test 5: reversal loss vs the drive's ramp register 0x03 ---------------
// The regime no other test here reaches. TEST 2 reverses continuously but at
// +-4 steps, so the drive never banks real velocity; TEST 1b uses real rates
// but SETTLES 700 ms between legs, which lets any ramp residual retire. This
// runs TEST 2's continuity at TEST 1b's rate, swept across 0x03's regimes.
// Manual v2.55 p11: <60000 internal ramp, 60000 none, 60001..60098 none plus
// position feedforward in the low two digits. See docs/drive-accel-register.md.
//
// MEASURE AND COMMIT ARE SEPARATE KEYS, and that is forced by the drive, not a
// style choice. Writing 0x03 needs 0x00=1, which makes the drive ignore step
// pulses, and 0x00 CANNOT BE CLEARED OVER MODBUS (dev-board sd-opb, measured:
// the write acks and the register stays 1). Only a drive power cycle clears
// it. So one arm of an A/B is: commit the value, power-cycle, then measure
// with a run that writes NO registers at all.
constexpr int32_t  kLadLeg    = 400;   // steps/leg, 7.7 mm -- the excursion the other tests already use
constexpr int32_t  kLadCycles = 100;   // 200 reversals per rung
constexpr uint32_t kLadHalfUs = 25;    // nominal 50 us period; achieved rate is MEASURED below

// COMMIT one 0x03 value to the drive's EEPROM. Leaves 0x00 = 1 on purpose:
// it cannot be cleared over the wire (sd-opb), and the power cycle this
// requires is what clears it. The drive is DEAF TO STEP/DIR until that cycle.
bool commitAccel(uint16_t v) {
    Serial.println("");
    Serial.printf("--- COMMIT 0x03 = %u TO DRIVE EEPROM ---", v);
    Serial.println("");
    uint16_t before = 0;
    readRegs(0x03, 1, &before);
    const bool ackEn  = writeReg(0x00, 1); delay(200);
    const bool ackAcc = writeReg(0x03, v); delay(200);
    uint16_t back = 0xFFFF;
    const bool rdAcc  = readRegs(0x03, 1, &back);
    const bool ackSav = writeReg(0x14, 1);
    delay(3000);                       // the save is not instant; 0x14 self-clears
    uint16_t sav = 0xFFFF;
    readRegs(0x14, 1, &sav);
    uint16_t after = 0xFFFF;
    readRegs(0x03, 1, &after);
    Serial.printf("was=%u  ack00=%d ack03=%d rd03=%d val=%u  ack14=%d 0x14now=%u  0x03now=%u",
                  before, ackEn, ackAcc, rdAcc, back, ackSav, sav, after);
    Serial.println("");
    const bool ok = (back == v) && (after == v);
    Serial.println(ok ? "WRITTEN. POWER-CYCLE THE DRIVE NOW -- that clears 0x00 and is the "
                        "only way step/dir comes back. Then re-dump to confirm it stuck."
                      : "WRITE DID NOT TAKE -- do not power-cycle yet, read the line above.");
    return ok;
}

// Staircase pre-flight. An unhomed machine's encoder counts have no origin
// (zero is wherever the shaft sat at drive power-on), so the carriage position
// is genuinely unknown and the ladder's +-7.7 mm could be into a hard stop.
// Walk out in escalating pairs and abort the moment commanded travel and
// actual travel disagree, which is what a wall looks like from here.
// WRITES NOTHING. It runs at 2.5 kHz, where even the slowest ramp setting
// reaches speed in ~1.5 ms out of a 40 ms leg, so commanded and actual travel
// agree at ANY 0x03 value and a shortfall means a wall rather than a ramp.
bool ladderPreflight() {
    Serial.println("  preflight: walking out to +-7.7 mm at 2.5 kHz, no register writes");
    for (int32_t leg : {100, 200, 400}) {
        for (int dir = 0; dir < 2; ++dir) {
            int32_t a = 0, b = 0;
            if (!settled(a)) { Serial.println("  preflight: encoder read failed"); return false; }
            stepPulses(leg, dir != 0, 200, 200);
            if (!settled(b)) { Serial.println("  preflight: encoder read failed"); return false; }
            const float got  = fabsf(float(b - a)) / kCountsPerStep;
            const float want = float(leg);
            Serial.printf("    %s %4ld steps -> %6.1f actual (%3.0f%%, %.2f mm)",
                          dir ? "rev" : "fwd", (long)leg, got, 100.0f * got / want,
                          got / 52.1519f);
            Serial.println("");
            if (got < want * 0.9f) {
                Serial.println("  PREFLIGHT FAIL: carriage did not travel as commanded. It is "
                               "against a stop or the path is blocked. LADDER ABORTED, nothing "
                               "further will move.");
                return false;
            }
        }
    }
    Serial.println("  preflight OK: at least 7.7 mm clear each way");
    return true;
}

// MEASURE ONLY. Writes no register, so 0x00 stays 0 and step/dir stays live.
// Whatever 0x03 currently holds IS the condition under test; change arms with
// commitAccel plus a power cycle, never inside a run.
void testAccelBurst() {
    Serial.println("");
    Serial.println("--- TEST 5: reversal loss at the drive's CURRENT ramp setting ---");
    uint16_t acc = 0, en = 0xFFFF;
    if (!readRegs(0x03, 1, &acc) || !readRegs(0x00, 1, &en)) {
        Serial.println("  register read failed -- aborting, nothing moved");
        return;
    }
    Serial.printf("0x03 = %u  -> %s", acc,
                  acc < 60000 ? "INTERNAL RAMP ON (drive re-ramps every pulse train)"
                              : acc == 60000 ? "no internal ramp" : "no ramp + position feedforward");
    Serial.println("");
    if (en != 0) {
        // Every pulse below would be ignored and the encoder would not move, so
        // the run would report a perfect zero and mean nothing at all.
        Serial.printf("0x00 = %u -- drive is in Modbus mode and IGNORES step/dir. "
                      "Power-cycle it first. ABORTING.", en);
        Serial.println("");
        return;
    }
    Serial.printf("%ld cycles of +-%ld steps (%.2f mm), %ld reversals, no settle between",
                  (long)kLadCycles, (long)kLadLeg, float(kLadLeg) / 52.1519f,
                  (long)kLadCycles * 2);
    Serial.println("");
    if (!ladderPreflight()) return;

    Serial.println("");
    Serial.println("  0x03    kHz    net_counts    steps  um/reversal  verdict");
    int32_t a = 0, b = 0;
    if (!settled(a)) { Serial.println("  encoder read failed"); return; }
    const uint32_t t0 = micros();
    for (int32_t k = 0; k < kLadCycles; ++k) {
        stepPulses(kLadLeg, false, kLadHalfUs, kLadHalfUs);
        stepPulses(kLadLeg, true,  kLadHalfUs, kLadHalfUs);
    }
    const uint32_t el = micros() - t0;
    if (!settled(b)) { Serial.println("  encoder read failed"); return; }
    const int32_t net = b - a;
    const float um  = float(net) / kCountsPerStep * kUmPerStep / float(kLadCycles * 2);
    const float khz = float(kLadCycles * 2 * kLadLeg) / float(el) * 1000.0f;
    Serial.printf("%6u %6.1f  %12ld  %7.1f  %11.3f  %s",
                  acc, khz, (long)net, float(net) / kCountsPerStep, um,
                  fabsf(um) < 0.5f ? "conserves" : "LOSES");
    Serial.println("");
}

void restoreEgear() {
    Serial.println("");
    Serial.println("--- RESTORE E-GEAR 0x0A=32768 0x0B=2048 ---");
    const bool a = writeReg(0x0A, kEgearNum);
    delay(50);
    const bool b = writeReg(0x0B, kEgearDen);
    delay(50);
    uint16_t ra = 0, rb = 0;
    readRegs(0x0A, 1, &ra);
    readRegs(0x0B, 1, &rb);
    Serial.printf("acked %d/%d | readback 0x0A=%u 0x0B=%u", a, b, ra, rb);
    Serial.println("");
    Serial.println(ra == kEgearNum && rb == kEgearDen ? "RESTORED" : "NOT restored -- power-cycle the drive");
}

// ---- Write 0x19=2 and commit to EEPROM, e-gear untouched -----------------
// Poll 0x14 fast: 0 idle / 1 saving / 2 complete, and the complete state may be
// transient, so catch any non-zero rather than sampling slowly and seeing 0.
// keepEnabled: leave 0x00 = 1 across the commit. The manual's procedures open
// with Modbus-enable and never show it turned back off before the reboot, and
// 0x00 is itself a power-on parameter -- so restoring it may undo the state the
// commit needs to latch in. Leaves the drive deaf to PU/DIR until reboot.
void saveSpecialFn(bool keepEnabled = false) {
    Serial.println("");
    Serial.println("--- WRITE 0x19=2 AND SAVE TO EEPROM (e-gear untouched) ---");
    uint16_t gn = 0, gd = 0, mb = 0;
    readRegs(0x0A, 1, &gn);
    readRegs(0x0B, 1, &gd);
    readRegs(0x00, 1, &mb);
    Serial.printf("e-gear 0x0A=%u 0x0B=%u | modbus enable 0x00=%u", gn, gd, mb);
    Serial.println("");

    writeReg(0x00, 1);
    delay(80);
    if (!writeReg(0x19, 2)) { Serial.println("0x19=2 NOT ACKED -- stopping"); return; }
    delay(80);
    if (!writeReg(0x14, 1)) Serial.println("WARN: 0x14=1 not acked");

    // SILENT wait: an EEPROM commit is a flash write, and polling the bus
    // through it is a good way to disturb it. Say nothing for 3 s, then look.
    Serial.println("committing (silent 3 s, no bus traffic)...");
    delay(3000);
    uint16_t seen = 0;
    readRegs(0x14, 1, &seen);
    if (!keepEnabled) { writeReg(0x00, mb); delay(80); }
    else Serial.println("leaving 0x00=1 -- drive is DEAF to PU/DIR until you reboot");

    uint16_t f = 0, a = 0;
    readRegs(0x19, 1, &f);
    readRegs(0x0A, 1, &a);
    Serial.printf("save flag after wait=%u | 0x19=%u | 0x0A=%u", seen, f, a);
    Serial.println("");
    Serial.println(seen == 0 ? "save flag back to 0 (idle) -- commit finished or never ran; reboot decides"
                             : "save flag still busy -- give it longer");
}

// ---- Step-by-step verified commit ----------------------------------------
// Every earlier attempt wrote blind and only checked the end state. Nothing
// persists across a reboot -- not even 0x00 -- so the failure is somewhere IN
// the sequence. Read back after every single write and print it.
void verifiedCommit() {
    Serial.println("");
    Serial.println("--- VERIFIED COMMIT (readback after every write) ---");
    uint16_t v = 0;

    Serial.print("1) write 0x00=1 (modbus enable) ... ");
    const bool ack0 = writeReg(0x00, 1);
    delay(120);
    readRegs(0x00, 1, &v);
    Serial.printf("ack=%d readback=%u %s", ack0, v, v == 1 ? "OK" : "<-- DID NOT TAKE");
    Serial.println("");

    Serial.print("2) write 0x19=2 (encoder follow) ... ");
    const bool ack1 = writeReg(0x19, 2);
    delay(120);
    readRegs(0x19, 1, &v);
    Serial.printf("ack=%d readback=%u %s", ack1, v, v == 2 ? "OK" : "<-- DID NOT TAKE");
    Serial.println("");

    Serial.print("3) write 0x14=1 (parameter save) ... ");
    const bool ack2 = writeReg(0x14, 1);
    delay(120);
    readRegs(0x14, 1, &v);
    Serial.printf("ack=%d readback=%u", ack2, v);
    Serial.println("");

    delay(3000);
    readRegs(0x14, 1, &v);
    Serial.printf("4) after 3 s: 0x14=%u", v);
    Serial.println("");
    readRegs(0x00, 1, &v);
    Serial.printf("   0x00=%u", v);
    Serial.println("");
    readRegs(0x19, 1, &v);
    Serial.printf("   0x19=%u", v);
    Serial.println("");
    Serial.println("If every step read back OK and a reboot still shows 0x19=0,");
    Serial.println("then 0x14 does not cover 0x19 on this firmware.");
}

// ---- Back to pulse+direction, no reboot needed ----------------------------
// 0x19=2 is live in RAM and DISABLES step/dir, so leaving it set leaves the
// machine dead. Writing 0x19=0 needs 0x00=1 first (parameter writes are gated).
void restorePulseDir() {
    Serial.println("");
    Serial.println("--- RESTORE PULSE+DIRECTION (0x19=0) ---");
    // Force 0x00 to 0 at the end, never "back to what it was": 0x00=1 is what
    // disables the external pulse input, so restoring a stale 1 leaves the
    // drive deaf for a different reason than the one being fixed.
    uint16_t v = 0;
    writeReg(0x00, 1);
    delay(120);
    const bool ok = writeReg(0x19, 0);
    delay(120);
    readRegs(0x19, 1, &v);
    writeReg(0x00, 0);
    delay(120);
    uint16_t m2 = 0;
    readRegs(0x00, 1, &m2);
    Serial.printf("ack=%d | 0x19=%u | 0x00=%u", ok, v, m2);
    Serial.println("");
    Serial.println(v == 0 && m2 == 0 ? "PULSE+DIR RESTORED -- step/dir works again"
                          : "0x19 not cleared -- power-cycle the drive");
}

// ---- Save variant: write 0x14=2 instead of 1 ------------------------------
// The manual's save-flag column reads "1: Save the parameters in / 2: Save it",
// while the other sheet calls 2 a COMPLETE status. If 2 is a command rather
// than a status, every attempt so far has been issuing the wrong one.
void saveVariant2() {
    Serial.println("");
    Serial.println("--- SAVE VARIANT: 0x19=2 then 0x14=2 ---");
    uint16_t v = 0;
    writeReg(0x00, 1); delay(120);
    readRegs(0x00, 1, &v); Serial.printf("0x00=%u", v); Serial.println("");
    writeReg(0x19, 2); delay(120);
    readRegs(0x19, 1, &v); Serial.printf("0x19=%u", v); Serial.println("");
    const bool ok = writeReg(0x14, 2);
    Serial.printf("0x14=2 ack=%d", ok); Serial.println("");
    delay(3000);
    readRegs(0x14, 1, &v); Serial.printf("0x14 after 3s=%u", v); Serial.println("");
    readRegs(0x19, 1, &v); Serial.printf("0x19 now=%u", v); Serial.println("");
    Serial.println("POWER-CYCLE THE DRIVE and dump -- 0x19=2 means it stuck.");
}

// ---- Full config dump, read-only. Take this BEFORE changing anything -----
// 0x0A/0x0B are the electronic gear and the machine's whole calibration hangs
// off them (52.15 steps/mm). Enabling special functions requires 0x0A = 0, so
// the pre-change values are the only way back.
void dumpRegs() {
    Serial.println("");
    Serial.println("--- DRIVE REGISTER DUMP 0x00..0x19 (read-only) ---");
    for (uint16_t r = 0x00; r <= 0x19; ++r) {
        uint16_t v = 0;
        const bool ok = readRegs(r, 1, &v);
        const char* note = (r == 0x0A) ? "  <-- e-gear numerator (0 = special fns)"
                         : (r == 0x0B) ? "  <-- e-gear denominator"
                         : (r == 0x19) ? "  <-- special function selector"
                         : (r == 0x14) ? "  <-- save flag (0 idle 1 saving 2 done)"
                         : "";
        if (ok) Serial.printf("  0x%02X = %5u%s", r, v, note);
        else    Serial.printf("  0x%02X =  ????  (read failed)", r);
        Serial.println("");
        delay(15);
    }
    Serial.println("RECORD THE 0x0A / 0x0B VALUES BEFORE ENABLING QUADRATURE.");
}

// ---- Enable encoder-following: reg 0x19 = 2, persisted via reg 0x14 -------
// Reads 0x19 back rather than trusting the write echo, because "wrote it" and
// "the drive accepted it" are different claims.
// Datasheet: 0x0A "if 0, enables special functions (see 0x19)". So 0x19 is
// INERT while the electronic gear is programmed -- which is why an earlier
// attempt read 0x19=2 back and changed nothing. 0x0A must be zeroed first.
// 0x14 is 0 idle / 1 saving / 2 complete, so poll it rather than assume.
void enableQuadrature() {
    Serial.println("");
    Serial.println("--- ENABLE QUADRATURE: 0x0A=0 (unlock special fns), 0x19=2 ---");
    uint16_t gearN = 0, gearD = 0, fn = 0;
    readRegs(0x0A, 1, &gearN);
    readRegs(0x0B, 1, &gearD);
    readRegs(0x19, 1, &fn);
    Serial.printf("before: 0x0A=%u 0x0B=%u 0x19=%u", gearN, gearD, fn);
    Serial.println("");
    Serial.printf("*** RESTORE VALUES: 0x0A=%u 0x0B=%u ***", gearN, gearD);
    Serial.println("");

    // 0x00 reads 0 = "Modbus disabled". Reads/writes still answer, but the save
    // flag never reached 2 (complete) with it off. Enable for the commit, then
    // put it back so Modbus control is not left on alongside step/dir.
    uint16_t mbEnable = 0;
    readRegs(0x00, 1, &mbEnable);
    Serial.printf("modbus enable 0x00 was %u -> setting 1 for the commit", mbEnable);
    Serial.println("");
    writeReg(0x00, 1);
    delay(80);

    if (!writeReg(0x0A, 0)) { Serial.println("write 0x0A=0 NOT ACKED -- stopping"); return; }
    delay(50);
    if (!writeReg(0x19, 2)) { Serial.println("write 0x19=2 NOT ACKED -- stopping"); return; }
    delay(50);
    if (!writeReg(0x14, 1)) Serial.println("WARN: 0x14=1 not acked");

    uint16_t save = 0;
    for (int i = 0; i < 40; ++i) {           // poll for 2 = complete
        delay(100);
        if (readRegs(0x14, 1, &save) && save == 2) break;
    }
    Serial.printf("save flag 0x14 = %u (%s)", save,
                  save == 2 ? "COMPLETE" : save == 1 ? "still saving" : "NOT SAVED");
    Serial.println("");

    writeReg(0x00, mbEnable);          // restore the Modbus enable flag
    delay(80);

    uint16_t a = 0, f = 0;
    readRegs(0x0A, 1, &a);
    readRegs(0x19, 1, &f);
    Serial.printf("after : 0x0A=%u 0x19=%u", a, f);
    Serial.println("");
    Serial.println(a == 0 && f == 2
        ? "SET. Now POWER-CYCLE THE DRIVE, then run 'g' -- mode latches at boot."
        : "did NOT take -- do not power-cycle, read the values above");
}

}  // namespace

// ---- Modbus absolute-position motion (ossm-rs sequence) -------------------
// FC 0x7B, 4-byte BIG-ENDIAN absolute position, 8-byte frame, 8-byte echo.
// Frame shape taken from ossm-rs Motor57AIMxx::set_absolute_position.
// This repo recorded 0x7B as bench-dead at fw 2.1.21; the suspicion is that the
// drive was never listening, not that it rejects the FC. See modbusMotionTest.
bool g_quiet7B = false;
bool g_noEcho7B = false;      // fire-and-forget: halves the wire time per frame
uint32_t g_baud = kBaud;

// FC 0x06 with no echo wait. The magic sequence's final write (0x00=506) never
// answers -- the drive has already changed baud by the time it would.
void writeRegNoEcho(uint16_t reg, uint16_t val) {
    uint8_t req[8] = {kSlave, 0x06, uint8_t(reg >> 8), uint8_t(reg),
                      uint8_t(val >> 8), uint8_t(val), 0, 0};
    const uint16_t c = crc16(req, 6);
    req[6] = uint8_t(c & 0xFF);
    req[7] = uint8_t(c >> 8);
    while (Serial1.available()) Serial1.read();
    Serial1.write(req, 8);
    Serial1.flush();
}

// ---- Baud reprogram, OSSM-RS magic sequence -------------------------------
// Identical in ossm-rs Motor57AIMxx::set_baud_rate and this repo's
// ServoModbus::reprogramBaud: 0x00=1, 0x03=<code>, 0x04=129, 0x00=506.
// VOLATILE ON PURPOSE. Never writes 0x14, so a drive power cycle always
// recovers factory 19200 and a botched rebaud cannot strand the link.
// 0x03 is the ACCEL register doing double duty as the code carrier, so accel
// must be rewritten afterward by whoever cares about it.
bool rebaud(uint32_t target) {
    const uint16_t code = (target == 115200) ? 803 : (target == 38400) ? 802
                        : (target == 19200)  ? 801 : 800;
    Serial.printf("  magic sequence -> %lu (code %u)", (unsigned long)target, code);
    Serial.println("");
    writeRegNoEcho(0x00, 1);      delay(30);
    writeRegNoEcho(0x03, code);   delay(30);
    writeRegNoEcho(0x04, 129);    delay(30);
    writeRegNoEcho(0x00, 506);    delay(100);

    Serial1.updateBaudRate(target);
    delay(50);
    uint16_t v = 0;
    for (int i = 0; i < 3; ++i) {
        if (readRegs(0x0E, 1, &v)) {
            g_baud = target;
            Serial.printf("  CONFIRMED @%lu (alarm=0x%04X)", (unsigned long)target, v);
            Serial.println("");
            writeReg(0x03, 50000);   // 0x03 carried the baud code; restore accel
            return true;
        }
        delay(50);
    }
    Serial.printf("  NO ANSWER @%lu -- restoring %lu", (unsigned long)target,
                  (unsigned long)g_baud);
    Serial.println("");
    Serial1.updateBaudRate(g_baud);
    delay(50);
    return false;
}

bool sendAbsolute7B(int32_t pos) {
    uint8_t req[8] = {kSlave, 0x7B,
                      uint8_t(pos >> 24), uint8_t(pos >> 16),
                      uint8_t(pos >> 8),  uint8_t(pos), 0, 0};
    const uint16_t c = crc16(req, 6);
    req[6] = uint8_t(c & 0xFF);
    req[7] = uint8_t(c >> 8);
    while (Serial1.available()) Serial1.read();
    if (!g_noEcho7B) delay(8);      // RTU gap matters only when we read back
    Serial1.write(req, 8);
    Serial1.flush();
    if (g_noEcho7B) return true;
    uint8_t rsp[16];
    size_t got = 0;
    const uint32_t t0 = millis();
    while (got < 8 && millis() - t0 < 300)
        if (Serial1.available()) rsp[got++] = uint8_t(Serial1.read());
    if (got == 0) {
        if (!g_quiet7B) { Serial.print("      0x7B: no echo"); Serial.println(""); }
        return false;
    }
    if (!g_quiet7B) {
        Serial.print("      0x7B echo:");
        for (size_t i = 0; i < got; ++i) Serial.printf(" %02X", rsp[i]);
        Serial.println("");
    }
    return got >= 8 && rsp[0] == kSlave;
}

// One arm of the motion test. `reseat` replays the ossm-rs ordering trap:
// writing 0x00=1 resets 0x02 target speed and 0x18 max output to defaults, so a
// drive that is enabled but never re-armed ACCEPTS 0x7B and does not move --
// indistinguishable from "ignores 0x7B entirely".
void modbusMotionArm(const char* label, bool reseat, int32_t delta) {
    Serial.printf("  -- arm: %s", label); Serial.println("");
    writeReg(0x02, 300);        // target speed, RPM
    writeReg(0x03, 50000);      // acceleration
    writeReg(0x05, 3000);       // speed loop P
    writeReg(0x07, 3000);       // position loop P
    writeReg(0x18, 600);        // standstill max output
    writeReg(0x01, 1);          // driver output enable

    writeReg(0x00, 1);          // ModbusEnable -- clobbers 0x02 and 0x18
    delay(800);                 // ossm-rs settles here before commanding

    if (reseat) {
        writeReg(0x02, 300);
        writeReg(0x18, 600);
    }
    uint16_t chk[1];
    if (readRegs(0x02, 1, chk)) Serial.printf("      0x02 speed reads %u", chk[0]);
    Serial.println("");

    int32_t before = 0, after = 0;
    if (!settled(before)) { Serial.print("      encoder read failed"); Serial.println(""); return; }
    sendAbsolute7B(before + delta);
    delay(1200);
    settled(after);
    Serial.printf("      enc %ld -> %ld   moved %ld counts (%.3f mm), asked %ld",
                  (long)before, (long)after, (long)(after - before),
                  double(after - before) / kCountsPerMm, (long)delta);
    Serial.println("");
    Serial.print(labs(after - before) > 200 ? "      *** MOTION ***" : "      no motion");
    Serial.println("");
}

// 'm' -- does this drive follow FC 0x7B absolute setpoints at all?
// Restores 0x00=0 on the way out so the step/dir input works again without a
// power cycle being strictly required.
void modbusMotionTest() {
    Serial.println("");
    Serial.println("=== Modbus absolute-position test (ossm-rs FC 0x7B) ===");
    uint16_t st[2];
    if (readRegs(0x00, 2, st)) Serial.printf("  entry: 0x00=%u 0x01=%u", st[0], st[1]);
    Serial.println("");

    modbusMotionArm("0x00=1 WITHOUT re-seating speed (the suspected old failure)",
                    false, 3000);
    modbusMotionArm("0x00=1 THEN re-seat 0x02/0x18 (ossm-rs order)", true, 3000);
    modbusMotionArm("re-seated, opposite direction", true, -3000);

    writeReg(0x00, 0);          // hand the drive back to its pulse input
    Serial.print("  0x00 forced to 0 -- step/dir input live again");
    Serial.println("");
    Serial.println("=== done ===");
}

// 'n' -- does STREAMING absolute setpoints accumulate error the way step/dir
// does? This is the whole reason the Modbus path is interesting: a pulse is a
// RELATIVE move, so a step the shaft never made is lost forever, while an
// absolute setpoint restates the truth every frame and cannot accumulate.
// Same shape as the FAS drift cell: N strokes out and back, start and end at
// the same commanded point, then compare where the shaft actually is.
void modbusStreamTest(int32_t strokes, int32_t amp, uint32_t frameMs, bool noEcho) {
    Serial.println("");
    Serial.printf("=== Modbus STREAM: %ld strokes, +/-%ld counts, %lu ms/frame ===",
                  (long)strokes, (long)amp, (unsigned long)frameMs);
    Serial.println("");

    writeReg(0x02, 300);
    writeReg(0x03, 50000);
    writeReg(0x05, 3000);
    writeReg(0x07, 3000);
    writeReg(0x18, 600);
    writeReg(0x01, 1);
    writeReg(0x00, 1);
    delay(800);
    writeReg(0x02, 300);
    writeReg(0x18, 600);

    g_quiet7B = true;
    g_noEcho7B = noEcho;
    Serial.printf("  baud %lu | echo %s", (unsigned long)g_baud, noEcho ? "OFF" : "on");
    Serial.println("");
    int32_t home0 = 0;
    if (!settled(home0)) { Serial.print("  encoder read failed"); Serial.println(""); return; }
    Serial.printf("  center %ld", (long)home0); Serial.println("");

    // Cosine so the stream starts and ends at the center with zero velocity,
    // which is what makes the end-to-end comparison meaningful.
    const int32_t framesPerStroke = int32_t(1000.0f / float(frameMs));
    int32_t sent = 0;
    const uint32_t tStart = millis();
    for (int32_t s = 0; s < strokes; ++s) {
        for (int32_t f = 0; f < framesPerStroke; ++f) {
            const float ph = 2.0f * 3.14159265f * float(f) / float(framesPerStroke);
            const int32_t tgt = home0 + int32_t(float(amp) * (1.0f - cosf(ph)) * 0.5f);
            sendAbsolute7B(tgt);
            ++sent;
            delay(frameMs);
        }
    }
    const uint32_t elapsed = millis() - tStart;
    sendAbsolute7B(home0);
    delay(1500);

    int32_t end = 0;
    settled(end);
    Serial.printf("  achieved %.1f Hz (%lu ms for %ld frames)",
                  1000.0f * float(sent) / float(elapsed ? elapsed : 1),
                  (unsigned long)elapsed, (long)sent);
    Serial.println("");
    Serial.printf("  frames %ld | commanded back to %ld | actual %ld | error %ld counts (%.3f mm, %.2f steps)",
                  (long)sent, (long)home0, (long)end, (long)(end - home0),
                  double(end - home0) / kCountsPerMm,
                  double(end - home0) / kCountsPerStep);
    Serial.println("");
    g_quiet7B = false;
    g_noEcho7B = false;
    writeReg(0x00, 0);
    Serial.println("=== done ===");
}

// 'S' -- smoothness sweep. Streams a continuous cosine and steps the drive's
// own accel limit (0x03) between phases so the operator can FEEL each one.
//
// Why accel is the lever: the drive runs its own point-to-point profile to
// every setpoint. With accel very high it sprints to each target and idles
// until the next frame, so a 333 Hz stream is felt as 333 dashes per second.
// Accel is the only low-pass between the setpoint staircase and the shaft.
// Too low and it lags the trajectory instead, which costs accuracy -- hence
// the per-phase tracking error, so smooth and accurate are judged together.
// 'F' -- frame rate vs TRACKING accuracy, drive limits left wide open.
//
// Lowering the drive's accel to smooth the motion is barred: funscripts vary in
// speed and acceleration and the Modbus path must reproduce what FAS did, so
// clamping the drive is a downgrade, not a fix. That leaves frame rate as the
// lever -- and faster is NOT automatically better, because the drive has to
// consume every frame. This finds the knee.
//
// Reports lag in TIME as well as distance: distance lag is meaningless without
// the speed it happened at, and time lag is what actually distorts a script.
void modbusFrameSweep(int32_t amp, uint32_t phaseMs) {
    static const uint32_t kFrameMs[] = {20, 10, 5, 3, 2, 1};
    Serial.println("");
    Serial.printf("=== FRAME-RATE SWEEP: +/-%ld counts, %lu ms/phase, drive limits WIDE OPEN ===",
                  (long)amp, (unsigned long)phaseMs);
    Serial.println("");
    writeReg(0x02, 1000);
    writeReg(0x03, 50000);
    writeReg(0x05, 3000);
    writeReg(0x07, 3000);
    writeReg(0x18, 600);
    writeReg(0x01, 1);
    writeReg(0x00, 1);
    delay(800);
    writeReg(0x02, 1000);
    writeReg(0x03, 50000);
    writeReg(0x18, 600);

    int32_t home0 = 0;
    if (!settled(home0)) { Serial.print("  encoder read failed"); Serial.println(""); return; }
    const uint32_t cycleMs = 600;
    const float peakMmS = 3.14159265f * (float(amp) / kCountsPerMm) / (float(cycleMs) / 1000.0f);
    Serial.printf("  cosine %lu ms/cycle, peak ~%.0f mm/s", (unsigned long)cycleMs, peakMmS);
    Serial.println("");
    Serial.println("  frame_ms   achieved_Hz   worst_lag_counts   lag_mm   lag_ms_equiv");

    g_quiet7B = true;
    for (uint32_t fm : kFrameMs) {
        const uint32_t t0 = millis();
        int32_t worst = 0, sent = 0;
        while (millis() - t0 < phaseMs) {
            const uint32_t t = millis() - t0;
            const float ph = 2.0f * 3.14159265f * float(t % cycleMs) / float(cycleMs);
            const int32_t tgt = home0 + int32_t(float(amp) * (1.0f - cosf(ph)) * 0.5f);
            g_noEcho7B = true;
            sendAbsolute7B(tgt);
            ++sent;
            delay(fm);
            // Skip the first two cycles: a phase starts with the target at the
            // bottom of the stroke while the carriage is wherever the previous
            // phase left it, up to a full amplitude away. Latching that
            // transient reports it as steady-state tracking error, which is how
            // the first run of this sweep produced ~20mm on a 24mm stroke with
            // no trend and a flipping sign.
            if (t > 2 * cycleMs && (t % 101) < fm) {
                g_noEcho7B = false;
                int32_t enc = 0;
                if (encoder(enc) && labs(enc - tgt) > labs(worst)) worst = enc - tgt;
            }
        }
        const uint32_t el = millis() - t0;
        const float lagMm = float(worst) / kCountsPerMm;
        Serial.printf("  %8lu %13.1f %18ld %8.2f %14.1f",
                      (unsigned long)fm, 1000.0f * float(sent) / float(el ? el : 1),
                      (long)worst, lagMm, 1000.0f * fabsf(lagMm) / peakMmS);
        Serial.println("");
    }
    g_noEcho7B = false;
    sendAbsolute7B(home0);
    delay(1500);
    int32_t end = 0;
    settled(end);
    Serial.printf("  return error %ld counts (%.2f steps)",
                  (long)(end - home0), double(end - home0) / kCountsPerStep);
    Serial.println("");
    g_quiet7B = false;
    writeReg(0x00, 0);
    Serial.println("=== done ===");
}

void modbusSmoothSweep(uint32_t frameMs, int32_t amp, uint32_t phaseMs) {
    static const uint16_t kAccels[] = {50000, 20000, 8000, 3000, 1000};
    Serial.println("");
    Serial.printf("=== SMOOTHNESS SWEEP: %lu ms frames, +/-%ld counts, %lu ms/phase ===",
                  (unsigned long)frameMs, (long)amp, (unsigned long)phaseMs);
    Serial.println("");

    writeReg(0x02, 1000);      // speed limit well above the trajectory, so
    writeReg(0x05, 3000);      // ACCEL is the only thing being swept
    writeReg(0x07, 3000);
    writeReg(0x18, 600);
    writeReg(0x01, 1);
    writeReg(0x00, 1);
    delay(800);
    writeReg(0x02, 1000);
    writeReg(0x18, 600);

    int32_t home0 = 0;
    if (!settled(home0)) { Serial.print("  encoder read failed"); Serial.println(""); return; }

    g_quiet7B = true;
    const uint32_t cycleMs = 600;     // 0.6 s per stroke -- deliberately brisk
    for (uint16_t acc : kAccels) {
        g_noEcho7B = false;
        writeReg(0x03, acc);
        g_noEcho7B = true;
        Serial.printf("  --- accel 0x03 = %u ---", acc);
        Serial.println("");
        const uint32_t t0 = millis();
        int32_t worst = 0;
        while (millis() - t0 < phaseMs) {
            const uint32_t t = millis() - t0;
            const float ph = 2.0f * 3.14159265f * float(t % cycleMs) / float(cycleMs);
            const int32_t tgt = home0 + int32_t(float(amp) * (1.0f - cosf(ph)) * 0.5f);
            sendAbsolute7B(tgt);
            delay(frameMs);
            // Sample tracking error occasionally: smooth-but-lagging is a
            // failure too, so accuracy is reported next to the feel.
            if ((t % 97) == 0) {
                g_noEcho7B = false;
                int32_t enc = 0;
                if (encoder(enc)) {
                    const int32_t err = enc - tgt;
                    if (labs(err) > labs(worst)) worst = err;
                }
                g_noEcho7B = true;
            }
        }
        Serial.printf("      worst tracking error %ld counts (%.2f mm)",
                      (long)worst, double(worst) / kCountsPerMm);
        Serial.println("");
    }
    g_noEcho7B = false;
    sendAbsolute7B(home0);
    delay(1500);
    int32_t end = 0;
    settled(end);
    Serial.printf("  return error %ld counts (%.2f steps)",
                  (long)(end - home0), double(end - home0) / kCountsPerStep);
    Serial.println("");
    g_quiet7B = false;
    writeReg(0x03, 50000);
    writeReg(0x00, 0);
    Serial.println("=== done ===");
}

void setup() {
    Serial.begin(115200);
    delay(2500);
    Serial1.begin(kBaud, SERIAL_8N1, AIM_PIN_485_RX, AIM_PIN_485_TX);
    pinMode(AIM_PIN_STEP, OUTPUT);
    pinMode(AIM_PIN_DIR, OUTPUT);
    digitalWrite(AIM_PIN_STEP, LOW);
    digitalWrite(AIM_PIN_DIR, LOW);

    Serial.println("");
    Serial.println("=== quad_probe ===");
    Serial.printf("STEP/A=GPIO%d  DIR/B=GPIO%d  %ld pulses/leg  %.0f counts/step",
                  AIM_PIN_STEP, AIM_PIN_DIR, (long)kPulses, kCountsPerStep);
    Serial.println("");

    // Baud AUTO-DETECT. The drive keeps a reprogrammed 115200 across power
    // cycles (measured -- the 'power cycle always recovers 19200' assumption in
    // ServoModbus.cpp is false), while this probe restarts at 19200 on every
    // boot AND the host opening the USB CDC port resets the S3. Without this,
    // every session silently talks at the wrong baud and every read fails.
    uint16_t st[2];
    bool linked = false;
    for (uint32_t b : {19200u, 115200u}) {
        Serial1.updateBaudRate(b);
        delay(30);
        if (readRegs(0x00, 2, st)) {
            g_baud = b;
            linked = true;
            Serial.printf("drive @%lu: enable=%u output=%u",
                          (unsigned long)b, st[0], st[1]);
            break;
        }
    }
    if (!linked) Serial.print("drive: NO MODBUS ANSWER at 19200 or 115200 -- check the RS485 link");
    Serial.println("");
    Serial.println("READY -- 'd' dump, 'r' restore e-gear, 'w'/'W' save (W keeps 0x00=1), 'v' verified commit, 'z' back to pulse+dir, 'x' save-with-2, 'g' tests, 'q' full enable, 'm' MODBUS MOTION, 'n'/'N' MODBUS STREAM (N=fast+noecho), 'b' rebaud 115200, 'a' RAMP BURST (measure only), 'k'/'j' commit 0x03=60000/50000 to EEPROM (power-cycle after), 'S' accel sweep, 'F' FRAME-RATE sweep.");
}

// Trigger-on-demand rather than run-at-boot: the banner is gone by the time a
// monitor attaches, and unattended motion at power-on is not acceptable.
void loop() {
    if (!Serial.available()) {
        delay(50);
        return;
    }
    const int key = Serial.read();
    while (Serial.available()) Serial.read();
    if (key == 'd' || key == 'D') { dumpRegs(); return; }
    if (key == 'r' || key == 'R') { restoreEgear(); return; }
    if (key == 'w') { saveSpecialFn(false); return; }
    if (key == 'W') { saveSpecialFn(true);  return; }
    if (key == 'v' || key == 'V') { verifiedCommit(); return; }
    if (key == 'z' || key == 'Z') { restorePulseDir(); return; }
    if (key == 'x' || key == 'X') { saveVariant2(); return; }
    if (key == 'q' || key == 'Q') { enableQuadrature(); return; }
    if (key == 'm' || key == 'M') { modbusMotionTest(); return; }
    if (key == 'n') { modbusStreamTest(25, 20000, 20, false); return; }
    if (key == 'N') { modbusStreamTest(25, 20000, 2,  true);  return; }
    if (key == 'b' || key == 'B') { rebaud(115200); return; }
    if (key == 'a' || key == 'A') { testAccelBurst(); return; }
    // Two explicit values, never a toggle: an A/B arm you have to infer is an
    // A/B arm you will mislabel. Both need a drive power cycle after.
    if (key == 'k') { commitAccel(60000); return; }
    if (key == 'j') { commitAccel(50000); return; }
    if (key == 'S') { modbusSmoothSweep(1, 20000, 9000); return; }
    if (key == 'F') { modbusFrameSweep(20000, 7000); return; }
    testPulseWidths();
    testRateSweep();
    testTinyReversals();
    testQuadrature();
    testDecodeMode();
    Serial.println("");
    Serial.println("=== done === (send another character to repeat)");
}
