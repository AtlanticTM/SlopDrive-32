// AppLog — the SlopLog sink bridge for the S3 main controller. The web
// line-ring (/api/log) is a SlopLog sink; the four bridge functions register
// the sinks and pump/gate them. All logging now flows through SLOGx directly
// — see the header for the migration story.

#include "AppLog.h"

#include <new>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"

#include "SystemState.h"

namespace {

// The /api/log web ring: formatted lines, oldest-first dump. Written only
// from the drain caller (httpTask), but dumped from HTTP handlers which can
// interleave on other priorities — a short spinlock keeps the copy honest.
//
// SEVERITY SPLIT, and why this ring uses a different scheme from the other two.
// The core ring and the SlopSync bridge ring are TRANSIENT — something drains
// them at 100/200 Hz, so a "low records are capped, extras dropped" rule bites
// for at most one drain interval. This ring is a DISPLAY: nothing drains it,
// its whole job is "the last N lines". Cap the low class here and a Debug flood
// would freeze the operator's log view on stale lines, which is its own kind of
// lying about machine state.
//
// So the storage is physically PARTITIONED into two independent sub-rings —
// kLowLines for Trace/Debug/Info, kHighLines for Warn+ — each overwriting its
// OWN oldest entry. Debug spam scrolls freely through its 44 lines and can
// never reach the 16 lines holding the errors. Emission order is restored at
// dump() time by merging on a monotonic sequence number, which costs an O(n)
// walk on an HTTP GET that was already O(n) and zero extra work on the write
// path (O(1), no scan, no allocation, spinlock held only for the copy).
class WebRingSink final : public sloplog::ISink {
public:
    void write(const sloplog::Record& r) override {
        char line[kLineBytes];
        int n = snprintf(line, sizeof(line), "[%5lu.%03lu %c %s] %s",
                         (unsigned long)(r.ms / 1000u), (unsigned long)(r.ms % 1000u),
                         sloplog::levelChar(r.level), r.tag, r.msg);
        if (n < 0) return;
        if (n >= int(sizeof(line))) n = int(sizeof(line)) - 1;
        // Legacy call sites often carry their own trailing newline — the
        // ring is line-oriented, so strip it.
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (r.lost && size_t(n) < sizeof(line)) {
            snprintf(line + n, sizeof(line) - size_t(n), " (+%u lost)", r.lost);
        }

        portENTER_CRITICAL(&_mux);
        if (sloplog::isReserved(r.level)) commitLocked(_high, r.level, line);
        else                              commitLocked(_low, r.level, line);
        portEXIT_CRITICAL(&_mux);
    }

    void dump(String& out) {
        // Copy under the lock, then build the String outside it (String
        // append can reallocate — never allocate in a critical section).
        // _loSnap/_hiSnap used to be function-local `static` here (comment:
        // "far too big for an HTTP stack") — moved to instance members
        // (TRAPS T2 pass, 2026-07-28) so they ride into PSRAM with the rest
        // of this object instead of adding a SECOND ~8.9 KB internal-BSS
        // reservation on top of _low/_high. Same single-shared-buffer
        // semantics either way; dump() is never reentrant (httpTask only).
        LowSub& loSnap = _loSnap;
        HighSub& hiSnap = _hiSnap;
        portENTER_CRITICAL(&_mux);
        memcpy(&loSnap, &_low, sizeof(LowSub));
        memcpy(&hiSnap, &_high, sizeof(HighSub));
        const uint32_t evicted[sloplog::kLevelCount] = {
            _evicted[0], _evicted[1], _evicted[2], _evicted[3], _evicted[4], _evicted[5]};
        portEXIT_CRITICAL(&_mux);

        // Merge the two sub-rings back into emission order on `seq`. Both are
        // individually ordered oldest-first from their own start index, so this
        // is a two-finger merge — O(kLines), no sorting.
        size_t li = 0, hi = 0;
        const size_t lstart = (loSnap.head + kLowLines - loSnap.count) % kLowLines;
        const size_t hstart = (hiSnap.head + kHighLines - hiSnap.count) % kHighLines;
        while (li < loSnap.count || hi < hiSnap.count) {
            const bool takeLow =
                (hi >= hiSnap.count) ||
                (li < loSnap.count &&
                 // Wrap-safe compare: seq is monotonic and the window is 60.
                 int32_t(loSnap.seq[(lstart + li) % kLowLines] -
                         hiSnap.seq[(hstart + hi) % kHighLines]) < 0);
            if (takeLow) {
                out += loSnap.lines[(lstart + li) % kLowLines];
                ++li;
            } else {
                out += hiSnap.lines[(hstart + hi) % kHighLines];
                ++hi;
            }
            out += '\n';
        }

        // The never-silent footer: what this ring shed, what the sloplog core
        // ring shed (per level), and what the SlopSync bridge shed. A drop the
        // operator cannot see is the failure mode this whole change exists to
        // remove, so it rides on every single /api/log serve.
        char foot[192];
        char core[80];
        sloplog::logger().formatDropSummary(core, sizeof(core));
        snprintf(foot, sizeof(foot),
                 "[sloplog] webring evicted T:%lu D:%lu I:%lu W:%lu E:%lu F:%lu | "
                 "core dropped %s | bridge dropped %lu (%lu warn+)",
                 (unsigned long)evicted[0], (unsigned long)evicted[1],
                 (unsigned long)evicted[2], (unsigned long)evicted[3],
                 (unsigned long)evicted[4], (unsigned long)evicted[5], core,
                 (unsigned long)_bridgeDropped, (unsigned long)_bridgeDroppedHigh);
        out += foot;
        out += '\n';
    }

    // Filled by applogDump() before the footer is built — the sink itself has
    // no business reaching into SystemState.
    void setBridgeDrops(uint32_t all, uint32_t high) {
        _bridgeDropped = all;
        _bridgeDroppedHigh = high;
    }

private:
    static constexpr size_t kLineBytes = 140;
    static constexpr size_t kLowLines = 44;    // Trace/Debug/Info
    static constexpr size_t kHighLines = 16;   // Warn/Error/Fatal — untouchable

    // Sized per class so the reserve costs no RAM: 44 + 16 = the same 60 lines
    // the flat ring held, plus 5 B/line of seq+level bookkeeping.
    template <size_t N>
    struct Sub {
        char lines[N][kLineBytes] = {};
        uint32_t seq[N] = {};
        uint8_t level[N] = {};
        size_t head = 0;
        size_t count = 0;
    };
    using LowSub = Sub<kLowLines>;
    using HighSub = Sub<kHighLines>;

    // Commit into one sub-ring. Caller holds _mux. O(1): overwrite the oldest
    // slot of THIS class only, accounting the line it displaces.
    template <size_t N>
    void commitLocked(Sub<N>& sub, sloplog::Level level, const char* line) {
        if (sub.count == N) {
            // The slot about to be reused is this sub-ring's oldest entry. Per
            // §9.4 the operator gets told — and by construction a low-class
            // eviction can only ever have displaced another low-class line.
            const uint8_t victim = sub.level[sub.head];
            if (victim < sloplog::kLevelCount) ++_evicted[victim];
        }
        strncpy(sub.lines[sub.head], line, kLineBytes - 1);
        sub.lines[sub.head][kLineBytes - 1] = '\0';
        sub.level[sub.head] = uint8_t(level);
        sub.seq[sub.head] = _seq++;
        sub.head = (sub.head + 1) % N;
        if (sub.count < N) ++sub.count;
    }

    LowSub _low;
    HighSub _high;
    // dump()'s copy-under-lock snapshot targets — see dump()'s comment. Same
    // PSRAM placement as _low/_high (this whole object is placement-new'd
    // there, see webRingOrNull() below).
    LowSub _loSnap;
    HighSub _hiSnap;
    uint32_t _seq = 0;
    uint32_t _evicted[sloplog::kLevelCount] = {};
    uint32_t _bridgeDropped = 0;
    uint32_t _bridgeDroppedHigh = 0;
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
};

// ~17.8 KB total (_low + _high + _loSnap + _hiSnap) — TRAPS T2: this used to
// be a magic-static object (`static WebRingSink s;`), i.e. ~17.8 KB of
// internal BSS for a ring that ONLY httpTask ever touches (write() from the
// drain caller, dump() from HTTP handlers) — no ISR, no DMA, nothing that
// requires internal RAM. Found during the 2026-07-28 heap-relief pass as the
// single largest non-mandatory internal-RAM reservation in the build
// (xtensa-esp32s3-elf-nm --size-sort), on a device already down to ~15 KB
// free heap post-NimBLE-return. Placement-new'd into PSRAM from
// applogBegin() instead, same idiom as SlopSyncHubService in main.cpp
// (heap_caps_malloc + placement new, refuse rather than eat internal RAM if
// PSRAM is somehow absent). applogBegin() runs single-task, before any
// FreeRTOS task exists, so there is no construction-order race to worry
// about (unlike the magic-static version, whose first call could in
// principle race — it never did in practice, since applogBegin() always ran
// first, but this is now explicit rather than incidental).
static WebRingSink* s_webRing = nullptr;

// nullptr iff the one-time PSRAM allocation above failed. Every call site
// checks: this ring is a diagnostics convenience, not a safety plane, so the
// honest degradation is "no web log ring this boot", never a crash.
WebRingSink* webRingOrNull() { return s_webRing; }

// ---- RFC-017: the SlopLog -> SlopSync bridge sink --------------------------
// Every drained record is copied into the SPSC ring in SystemState; the Core-0
// "SlopSyncHub" task drains it and turns each line into a 0x0008 log EVENT.
//
// THE CONTRACT, identical to SerialSink's and for the same field-bug reason
// (USB-CDC with no host blocked ~100 ms/line ON HTTPTASK): this sink MUST NOT
// BLOCK and MUST NOT ALLOCATE. It does exactly one bounded struct copy behind
// two atomics. It never calls into slopsync::Hub — the Hub is mutex-free and
// single-task by contract, and calling publishLog() from here would be a silent
// data race on the safety plane.
//
// Overflow is drop-and-count (SystemState::slopLogPush increments
// sloplog_bridge_dropped) and the count is surfaced two ways: in the 0x0006
// hub-status snapshot's log_dropped field and via SLOPLOG itself, throttled.
//
// ARMING, and why it is not simply "on from applogBegin": the sink is
// REGISTERED at the top of setup() but stays INERT until the SlopSync hub
// service is about to spawn its task. Boot narrates ~100 lines through a 16-slot
// hand-off ring that nothing is draining yet, so an armed-from-boot bridge would
// keep the first 16 banner lines, drop the rest, and open the machine's life
// with a three-digit drop count that means nothing. The hub's own 32-deep replay
// ring is what a late-joining client reads anyway, and it cannot hold a boot log
// either. Arming late makes the counter mean what it says.
class SlopSyncSink final : public sloplog::ISink {
public:
    void bind(SystemState* s) { _state = s; }
    void arm() { _armed = true; }

    void write(const sloplog::Record& r) override {
        SystemState* st = _state;
        if (st == nullptr || !_armed) return;
        SystemState::SlopLogRec out;
        out.ms = r.ms;
        out.level = uint8_t(r.level);
        out.lost = r.lost;
        // Bounded copies; the two layouts are static_asserted identical below,
        // so these are exact-size memcpys with a guaranteed NUL already in
        // place (sloplog always NUL-terminates both fields).
        std::memcpy(out.tag, r.tag, sizeof(out.tag));
        std::memcpy(out.msg, r.msg, sizeof(out.msg));
        out.tag[sizeof(out.tag) - 1] = '\0';
        out.msg[sizeof(out.msg) - 1] = '\0';
        st->slopLogPush(out);  // false = ring full; the ring counted it
    }

private:
    SystemState* _state = nullptr;
    volatile bool _armed = false;
};

static_assert(sizeof(SystemState::SlopLogRec::tag) == sloplog::Record::kTagBytes,
              "SlopLogRec.tag drifted from sloplog::Record::kTagBytes");
static_assert(sizeof(SystemState::SlopLogRec::msg) == sloplog::Record::kMsgBytes,
              "SlopLogRec.msg drifted from sloplog::Record::kMsgBytes");
// RFC-017 leans on this: registry `log_levels` 0..5 mirror sloplog::Level
// number-for-number, so the bridge is a cast and not a translation table.
static_assert(uint8_t(sloplog::Level::Trace) == 0 && uint8_t(sloplog::Level::Fatal) == 5,
              "sloplog::Level no longer mirrors the registry's log_levels");
// The bridge ring's severity reserve keys off a raw level number so
// SystemState.h need not include sloplog. Keep the two definitions welded.
static_assert(SystemState::SLOPLOG_RESERVE_LEVEL == uint8_t(sloplog::kReserveFloor),
              "bridge-ring reserve floor drifted from sloplog::kReserveFloor");

SlopSyncSink& syncSink() {
    static SlopSyncSink s;
    return s;
}

}  // namespace

// Serial-sink gating is RUNTIME, not compile-time (the SERIAL_CONTROL_MODE
// #if that used to exclude the sink silenced ALL serial logging forever —
// a field incident). One input picks the sink's floor: handshook — the
// WebUI served /api/log at least once -> Warn+ only; until then, full.
static bool s_serialHandshook = false;

static void applySerialFloor() {
    sloplog::Level floor = s_serialHandshook ? sloplog::Level::Warn : sloplog::Level::Trace;
    sloplog::logger().setSinkFloor(&sloplog::serialSink(), floor);
}

// The state pointer the bridge sink was bound to, kept so applogDump() can
// quote the bridge ring's drop counters in the /api/log footer.
static SystemState* s_state = nullptr;

void applogBegin(SystemState* state) {
    s_state = state;
    // TRAPS T2: placement-new the web ring into PSRAM (see webRingOrNull()'s
    // comment) instead of the ~17.8 KB magic-static this used to be. Runs
    // single-task, before any other setup() work — no construction-order
    // race, unlike a lazily-first-called magic static would have.
    void* mem = heap_caps_malloc(sizeof(WebRingSink), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (mem != nullptr) {
        s_webRing = new (mem) WebRingSink();
        sloplog::logger().addSink(s_webRing);
    } else {
        SLOGE("sys", "no PSRAM block for the /api/log web ring — ring DISABLED this boot");
    }
    // RFC-017: the in-band log plane. Registered here (and only here) per §7.5 —
    // AppLog.cpp is the sink/bridge file. Binding a null state leaves the sink
    // registered but inert, which is what a build without SlopSync wants.
    syncSink().bind(state);
    sloplog::logger().addSink(&syncSink());
    // USB serial mirrors everything until told otherwise (see gating above).
    // Zero TX timeout = the CDC driver DROPS when full/unlistened instead of
    // blocking (~100 ms/line) — what makes unconditional writes safe on the
    // drain task.
    Serial.setTxTimeoutMs(0);
    sloplog::logger().addSink(&sloplog::serialSink());
    // Boot mode: drain synchronously after every line so the whole boot
    // narrates to serial in real time (single-task phase only — main.cpp
    // flips this off right before the FreeRTOS tasks spawn).
    sloplog::logger().setImmediateDrain(true);
}

void applogSyncBridgeArm() { syncSink().arm(); }

void applogDrain() { sloplog::drainToSinks(); }

// Called from TWO tasks now: httpTask (first /api/log serve) and the SlopSync
// hub task (first in-band log GRANT, RFC-017). Both only ever set the flag
// TRUE, so the worst possible interleaving is a harmless redundant write —
// not worth a lock on a one-shot handoff.
void applogSerialQuiet() {
    s_serialHandshook = true;
    applySerialFloor();
}

void applogDump(String& out) {
    WebRingSink* ring = webRingOrNull();
    if (ring == nullptr) {
        out += "[sloplog] web log ring unavailable this boot (PSRAM alloc failed at applogBegin)\n";
        return;
    }
    if (s_state != nullptr) {
        ring->setBridgeDrops(
            s_state->sloplog_bridge_dropped.load(std::memory_order_relaxed),
            s_state->sloplog_bridge_dropped_high.load(std::memory_order_relaxed));
    }
    ring->dump(out);
}

