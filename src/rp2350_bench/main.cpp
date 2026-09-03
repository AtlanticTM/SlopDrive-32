// rp2350_bench -- commit() timing gate for the RP2350 (sd-4k1.1).
// Constraints:
// - CROSS-COMPILE ONLY. No RP2350 exists on this bench; nothing below is
//   measured until an operator flashes it and reads the VERDICT line over
//   USB serial (COM port, 115200). This bead stays open until that happens
//   (C-4/C-8) -- the flash+read recipe lives on the bd issue, not here.
// - Core 0: a 20 kHz repeating timer running a SYNTHETIC load whose
//   arithmetic mirrors the coprocessor's own tick: the same quintic Horner
//   eval the published render plan is read with (rpmotion::Core::
//   sampleCounts) and the same Bresenham emit-state walk
//   (src/rp2350_motion/main.cpp emitTowardPos, read-only, never edited here),
//   no PIO/GPIO, so it burns the same cycles without needing hardware. The
//   late-tick census uses the identical >1.5x-tick-period rule.
// - Core 1 (setup1/loop1): owns the slopmotion::Engine and replays, in REAL
//   wall time via time_us_64(), the "uneven-knot" case from
//   test/native/test_slopmotion/test_main.cpp ("Sample synthesis pace: 60 Hz
//   stamps make every knot interval uneven", ~line 652). WHY THIS CASE: of
//   the ceiling-adjacent sample-synthesis / uneven-knot / jittered-arrival
//   regressions in that file, this one has the highest sustained commit()
//   call RATE (~60/s nominal, bursting higher when the wandering 0-30 ms
//   transport-delay model lets several stamps come due in one poll) on a
//   GENEROUS-machine config (operatorConfig() -- every span is legal and
//   PLAYS), so every commit runs the FULL waveform-commit path (holdback
//   synthesis -> commitWaveform -> legality scan -> possible Blend
//   bisection) rather than a cheap reject-to-chase shortcut. The 5 ms
//   jittered-arrival segment chain (test_main.cpp ~line 2507) costs more
//   PER commit (Reshape bisection under an explicit policy) but arrives at
//   only ~6 calls/s; this case wins on total commit() WORK PER SECOND,
//   which is what the 20 kHz tick actually has to share the CPU with.
// - DOUBLE MATH runs on the RP2350 DCP, not software emulation: the link
//   pulls framework-arduinopico/lib/rp2350/{platform_wrap.txt,core_wrap.txt},
//   which -Wl,--wrap= every __aeabi_d*/libm double symbol to libpico.a's DCP
//   objects unconditionally (confirmed via `pio run -v` + `nm` on the linked
//   ELF -- no plain libgcc double symbol is linked in). This is the real
//   DCP-path cost, not a software-double floor.
// - Arduino-pico's core-1 stack defaults to 0x800 (2 KB,
//   PICO_CORE1_STACK_SIZE, pico-sdk multicore.h). Declaring
//   `core1_separate_stack = true` below raises it to a HARDCODED 0x2000
//   (8 KB -- cores/rp2040/main.cpp mallocs exactly that, not configurable
//   through this switch), still below the S3 sampler's 16 KB budget for the
//   identical reason (motion-control.md, T1 class: commit() nests KB-scale
//   Ruckig temporaries). If the measured high-water mark below is close to
//   8 KB, the next step is bypassing setup1()/loop1() and calling
//   multicore_launch_core1_with_stack() directly with a bigger buffer.
// - No delay() anywhere in stepperTick, the repeating-timer callback, or
//   loop1(). USB serial output is confined to the once-a-second report in
//   loop() (core 0's Arduino loop, not the timed paths).
// - Exempt from canon_lint's serial-print check (tools/canon_lint.py), same
//   standing as src/rp2350_motion/ and src/quad_probe/: a standalone bench
//   sketch, its own env, excluded from every other env's build_src_filter,
//   USB CDC is its only output surface, and SlopLog does not reach this
//   silicon.
#include <Arduino.h>
#include "pico/time.h"
#include "pico/multicore.h"

#include "slopmotion/slopmotion.hpp"

#include <array>
#include <cmath>
#include <cstring>

using namespace slopmotion;

// Raise core 1's stack past the framework's 2 KB default -- see the file
// header. This OVERRIDES the weak default in cores/rp2040/main.cpp.
bool core1_separate_stack = true;

// ---- Core 0: synthetic 20 kHz tick load -------------------------------------
// Arithmetic shape mirrors src/rp2350_motion/main.cpp (read-only, never
// edited): same constants, same Horner quintic eval, same Bresenham
// emit-state walk. No PIO, no GPIO -- the word is folded into a checksum
// sink instead of a hardware FIFO, since only the CPU cost is being timed.
namespace core0_load {

constexpr uint32_t kTickUs       = 50;       // 20 kHz, matches rp2350_motion
constexpr uint32_t kStateHz      = 400000;
constexpr uint32_t kStatesPerTick = (kTickUs * kStateHz) / 1000000u;

float    s_pos = 0.0f;
float    s_emitted = 0.0f;
uint32_t s_segElapsedUs = 0;
float    s_segTs = 0.5f;   // representative 500 ms rest-to-rest move, looped
std::array<float, 6> s_qc{};
uint8_t  s_qphase = 0;
uint32_t s_qword = 0;
uint8_t  s_qbits = 0;
uint32_t s_emitOverrun = 0;
volatile uint32_t s_checksum = 0;   // sink: keeps the loop from being folded away

// Tick-lateness census, identical rule to rp2350_motion's stepperTick():
// a tick that never ran on time is trajectory that silently never happened.
volatile uint32_t s_lateTicks = 0;
volatile uint32_t s_tickCount = 0;

// Quintic coefficients for a fixed 0->1 rest-to-rest span -- same algebra as
// the render plan's own slice build. Content doesn't matter here, only the
// per-tick Horner evaluation COST, which is identical for any span.
void primeQuintic() {
    constexpr float V0 = 0.0f, V1 = 0.0f, A0 = 0.0f, A1 = 0.0f;
    const float R1 = 1.0f - 0.0f - V0 - 0.5f * A0;
    const float R2 = V1 - V0 - A0;
    const float R3 = A1 - A0;
    s_qc = {0.0f, V0, 0.5f * A0,
            10.0f * R1 - 4.0f * R2 + 0.5f * R3,
            -15.0f * R1 + 7.0f * R2 - R3,
            6.0f * R1 - 3.0f * R2 + 0.5f * R3};
}

inline void renderTick() {
    s_segElapsedUs += kTickUs;
    float t = float(s_segElapsedUs);
    const float T = s_segTs * 1.0e6f;
    if (t >= T) { t = 0.0f; s_segElapsedUs = 0; }
    const float u = t / T;
    s_pos = ((((s_qc[5] * u + s_qc[4]) * u + s_qc[3]) * u + s_qc[2]) * u +
             s_qc[1]) * u + s_qc[0];
}

// Copied from emitTowardPos(): Bresenham spread of owed transitions across
// kStatesPerTick states, phase walk, word-pack every 16 states. The packed
// word is XORed into a checksum sink -- there is no PIO on this bench, so no
// FIFO-full path exists, but the accumulate/pack/reset cost is identical.
inline void emitTowardPos() {
    const float delta = s_pos - s_emitted;
    const int dirStep = (delta >= 0.0f) ? 1 : -1;
    unsigned n = (unsigned)((delta >= 0.0f) ? delta : -delta);
    if (n > kStatesPerTick) { n = kStatesPerTick; ++s_emitOverrun; }
    unsigned acc = 0;
    for (unsigned i = 0; i < kStatesPerTick; ++i) {
        acc += n;
        if (acc >= kStatesPerTick) {
            acc -= kStatesPerTick;
            s_qphase = uint8_t((s_qphase + ((dirStep > 0) ? 3u : 1u)) & 3u);
            s_emitted += (float)dirStep;
        }
        s_qword |= (uint32_t)(s_qphase & 3u) << s_qbits;
        s_qbits = uint8_t(s_qbits + 2);
        if (s_qbits == 32u) {
            s_checksum ^= s_qword;
            s_qword = 0;
            s_qbits = 0;
        }
    }
}

struct repeating_timer tick_timer;

bool tick(struct repeating_timer*) {
    static uint32_t s_lastTickUs = 0;
    const uint32_t tnow = time_us_32();
    if (s_lastTickUs != 0 && (tnow - s_lastTickUs) > (kTickUs + kTickUs / 2u))
        s_lateTicks = s_lateTicks + 1;
    s_lastTickUs = tnow;
    s_tickCount = s_tickCount + 1;

    renderTick();
    emitTowardPos();
    return true;
}

}  // namespace core0_load

// ---- Core 1: slopmotion::Engine commit() timing -----------------------------
namespace core1_commit {

constexpr uint64_t kMsUs = 1000ULL;
constexpr uint64_t kSUs  = 1000000ULL;
constexpr uint64_t kDtUs = 16667ULL;   // ~60 Hz stamp cadence

Engine* s_engine = nullptr;

// Cumulative histogram, 50 us buckets 0-10 ms + one overflow bucket. Never
// reset -- the gate's acceptance is "over the run" (sd-4k1.1), not a
// rolling window, so percentiles are computed against the whole run.
volatile uint32_t s_commits = 0;
volatile uint32_t s_maxCommitUs = 0;
std::array<uint32_t, 201> s_hist{};

double src(uint64_t t_us) {
    constexpr double f = 1.0, amp = 0.25, mid = 0.5;
    return mid + amp * std::sin(2.0 * 3.14159265358979 * f *
                                 (double(t_us) * 1e-6));
}

// Same wandering 0-30 ms transport-delay model as the native test's
// arrival() -- MFP's own tick (~16.7 ms) makes every knot interval uneven.
uint64_t arrival(int k) {
    return (uint64_t)k * kDtUs +
           (uint64_t)((15.0 + 15.0 * std::sin(0.7 * k)) * 1000.0);
}

void recordCommitUs(uint32_t us) {
    const uint32_t bucket = us / 50u;
    s_hist[bucket > 200u ? 200u : bucket]++;
    s_commits = s_commits + 1;
    if (us > s_maxCommitUs) s_maxCommitUs = us;
}

// Percentile from the cumulative histogram.
uint32_t percentileUs(double frac) {
    const uint32_t total = s_commits;
    if (total == 0) return 0;
    const uint32_t target = (uint32_t)(frac * (double)total);
    uint32_t acc = 0;
    for (size_t i = 0; i < s_hist.size(); ++i) {
        acc += s_hist[i];
        if (acc >= target) return (uint32_t)(i * 50u);
    }
    return s_maxCommitUs;
}

// ---- Core-1 stack high-water mark (paint-and-scan) --------------------------
// rp2040.getFreeStack() reports CURRENT headroom only -- it cannot see how
// deep a call that already returned went, which is exactly what commit()'s
// nested Ruckig temporaries need measured. This is the same technique
// FreeRTOS's uxTaskGetStackHighWaterMark uses: paint the whole stack with a
// sentinel once, before any deep call runs, then scan from the base for
// where the deepest call actually reached.
constexpr size_t kCore1StackBytes = 0x2000;   // hardcoded by cores/rp2040/main.cpp
constexpr uint8_t kCanary = 0xA5;
uint32_t s_stackHighWaterBytes = 0;

void paintStack() {
    // Leave a margin below the CURRENT sp so setup1()'s own live locals and
    // return address are not overwritten mid-function.
    const uint32_t sp = rp2040.getStackPointer();
    uint8_t* base = (uint8_t*)core1_separate_stack_address;
    uint8_t* ceil = (uint8_t*)(uintptr_t)(sp - 64u);
    if (ceil > base) memset(base, kCanary, (size_t)(ceil - base));
}

void scanHighWater() {
    const uint8_t* base = (const uint8_t*)core1_separate_stack_address;
    size_t untouched = 0;
    while (untouched < kCore1StackBytes && base[untouched] == kCanary)
        untouched++;
    const uint32_t used = (uint32_t)(kCore1StackBytes - untouched);
    if (used > s_stackHighWaterBytes) s_stackHighWaterBytes = used;
}

}  // namespace core1_commit

void setup1() {
    using namespace core1_commit;
    paintStack();
    // operatorConfig() mirrored by hand from test_main.cpp -- a small, stable
    // fixture, not worth a generator for one struct (T20 applies to
    // multi-consumer WIRE vocabularies; this is a test fixture, single
    // consumer here).
    static Config cfg = [] {
        Config c;
        c.limits.vmax = 5.0f;
        c.limits.amax = 250.0f;
        c.limits.jmax = 10000.0f;
        c.overshoot_guard = 0.0f;
        return c;
    }();
    static Engine engine(cfg, 0.5f);
    s_engine = &engine;
}

void loop1() {
    using namespace core1_commit;
    static uint64_t t0 = 0;
    static int i = 0;
    const uint64_t now = time_us_64();
    if (t0 == 0) t0 = now;
    const uint64_t elapsed = now - t0;

    while (arrival(i) <= elapsed) {
        Command c;
        c.target = (float)src((uint64_t)i * kDtUs);
        c.has_anchor = true;
        c.anchor_us = t0 + (uint64_t)i * kDtUs;
        const uint64_t before = time_us_64();
        s_engine->commit(c, now);
        const uint64_t after = time_us_64();
        recordCommitUs((uint32_t)(after - before));
        i++;
    }
    // 2 s of source content (2 full sine periods at f=1 Hz -> seamless wrap)
    // looped forever, so the bench runs indefinitely.
    if (elapsed > 2 * kSUs) { t0 = 0; i = 0; }

    scanHighWater();
}

void setup() {
    Serial.begin(115200);
    core0_load::primeQuintic();
    add_repeating_timer_us(-int32_t(core0_load::kTickUs), core0_load::tick,
                            nullptr, &core0_load::tick_timer);
}

void loop() {
    static uint32_t lastPrint = 0;
    if (millis() - lastPrint < 1000) return;
    lastPrint = millis();

    using namespace core1_commit;
    const uint32_t commits = s_commits;
    const uint32_t p50 = percentileUs(0.50);
    const uint32_t p99 = percentileUs(0.99);
    const char* verdict = (commits == 0) ? "WARMUP"
        : ((p99 < 5000u && core0_load::s_lateTicks == 0) ? "PASS" : "FAIL");

    Serial.printf(
        "[bench] commits=%lu p50=%luus p99=%luus max=%luus lateTicks=%lu "
        "ticks=%lu core1StackHW=%lu/%luB sizeofEngine=%luB VERDICT=%s\n",
        (unsigned long)commits, (unsigned long)p50, (unsigned long)p99,
        (unsigned long)s_maxCommitUs,
        (unsigned long)core0_load::s_lateTicks,
        (unsigned long)core0_load::s_tickCount,
        (unsigned long)s_stackHighWaterBytes,
        (unsigned long)kCore1StackBytes,
        (unsigned long)sizeof(Engine), verdict);
}
