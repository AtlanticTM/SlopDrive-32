// SlopLog — hardware-free logging core. No Arduino, no FreeRTOS, no heap in
// steady state: everything platform-specific (time source, critical section,
// core id, sinks) is injected, so this exact code runs on the S3, the C5
// nodes, and inside the native doctest suite.
//
// Shape: producers on ANY task/core format a bounded Record on their own
// stack, then commit it into a fixed-slot ring under a short externally-
// supplied lock (the ~µs copy is the entire critical section). A single
// drain() caller (Core 0 on firmware) pops records and fans them out to
// registered sinks. Overflow drops the OLDEST record and counts the loss —
// a logger that can block a motion core is worse than no logger.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace sloplog {

enum class Level : uint8_t { Trace = 0, Debug, Info, Warn, Error, Fatal, Off };

inline const char* levelName(Level l) {
    switch (l) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO";
        case Level::Warn:  return "WARN";
        case Level::Error: return "ERROR";
        case Level::Fatal: return "FATAL";
        default:           return "?";
    }
}

inline char levelChar(Level l) { return "TDIWEF?"[uint8_t(l) < 6 ? uint8_t(l) : 6]; }

// One log record. Fixed-size by design: slots are copied whole under the
// ring lock and never reference caller memory after commit.
struct Record {
    static constexpr size_t kTagBytes = 12;   // includes NUL; longer tags truncate
    static constexpr size_t kMsgBytes = 104;  // includes NUL; longer messages truncate

    uint32_t ms = 0;        // producer-supplied timestamp (platform millis)
    Level level = Level::Info;
    uint8_t core = 0;       // producing core id (0/1; 0xFF = unknown/host)
    uint16_t lost = 0;      // records dropped immediately before this one
    char tag[kTagBytes] = {};
    char msg[kMsgBytes] = {};
};

// Where drained records go. write() is only ever called from drain()'s
// caller (single-threaded fan-out) — sinks need no locking of their own
// against each other. A sink that can block (network, flash) must do its
// own buffering; drain() trusts sinks to return promptly.
class ISink {
public:
    virtual ~ISink() = default;
    virtual void write(const Record& r) = 0;
};

// Injected platform surface: time + core id + the ring's critical section.
// Firmware backs this with millis()/xPortGetCoreID()/portMUX; native tests
// back it with a ManualClock-style fake and a no-op (or std::mutex) lock.
class IPort {
public:
    virtual ~IPort() = default;
    virtual uint32_t nowMs() = 0;
    virtual uint8_t coreId() = 0;
    virtual void lock() = 0;    // must be safe from any task on any core
    virtual void unlock() = 0;
};

// The core: fixed ring of Records + sink registry + drop accounting.
// Producers: push()/logf() from anywhere (lock held only for the slot copy).
// Consumer: exactly one caller pumps drain() (not enforced — documented).
template <size_t Slots = 64, size_t MaxSinks = 4>
class LogCore {
    static_assert(Slots >= 8, "ring too small to absorb a burst");

public:
    explicit LogCore(IPort& port, Level floor = Level::Trace)
        : _port(port), _floor(floor) {}

    // Runtime floor — records below it are rejected at push (cheap), on top
    // of whatever compile-time floor the macros already applied.
    void setFloor(Level l) { _floor = l; }
    Level floor() const { return _floor; }

    // Sinks may carry their own floor: a sink at Warn stays registered but
    // only sees Warn+. setSinkFloor() retunes a live sink (e.g. demote the
    // serial sink once the web UI has proven it is receiving logs).
    bool addSink(ISink* s, Level sinkFloor = Level::Trace) {
        if (s == nullptr || _sinkCount >= MaxSinks) return false;
        _sinks[_sinkCount] = s;
        _sinkFloors[_sinkCount] = sinkFloor;
        ++_sinkCount;
        return true;
    }

    bool setSinkFloor(ISink* s, Level sinkFloor) {
        for (size_t i = 0; i < _sinkCount; ++i) {
            if (_sinks[i] == s) {
                _sinkFloors[i] = sinkFloor;
                return true;
            }
        }
        return false;
    }

    // Boot mode: while set, every commit drains synchronously to the sinks.
    // ONLY safe while a single task is running (setup(), before the
    // scheduler spawns other producers) — flip it off before task creation.
    void setImmediateDrain(bool on) { _immediateDrain = on; }

    // Format-and-commit. Bounded: one vsnprintf into a stack Record, one
    // locked copy. Truncation is silent and fine (kMsgBytes is the contract).
    void logf(Level level, const char* tag, const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        vlogf(level, tag, fmt, ap);
        va_end(ap);
    }

    void vlogf(Level level, const char* tag, const char* fmt, va_list ap) {
        if (level < _floor || level >= Level::Off) return;
        Record r;
        r.ms = _port.nowMs();
        r.level = level;
        r.core = _port.coreId();
        copyBounded(r.tag, tag, Record::kTagBytes);
        vsnprintf(r.msg, Record::kMsgBytes, fmt, ap);
        push(r);
    }

    // Commit a pre-built record (producers that format their own).
    void push(const Record& r) {
        if (r.level < _floor || r.level >= Level::Off) return;
        _port.lock();
        Record& slot = _ring[_write % Slots];
        bool overwriting = (_write - _read) >= Slots;
        if (overwriting) {
            _read++;          // drop-oldest
            _lostSinceDrain++;
            _totalLostShadow++;
        }
        slot = r;
        slot.lost = _lostSinceDrain;  // rides on the next record a reader sees
        _write++;
        _port.unlock();
        if (_immediateDrain) drain();
    }

    // Fan out up to `maxRecords` pending records to every sink. Returns how
    // many were written. Single-consumer by contract.
    size_t drain(size_t maxRecords = Slots) {
        size_t n = 0;
        while (n < maxRecords) {
            Record r;
            _port.lock();
            if (_read == _write) {
                _port.unlock();
                break;
            }
            r = _ring[_read % Slots];
            _read++;
            _lostSinceDrain = 0;
            _port.unlock();
            for (size_t i = 0; i < _sinkCount; ++i) {
                if (r.level >= _sinkFloors[i]) _sinks[i]->write(r);
            }
            ++n;
        }
        return n;
    }

    // Lifetime drop count (records lost to overflow, ever).
    uint32_t totalLost() const { return _totalLostShadow; }
    size_t pending() const {
        // Racy read is fine: diagnostic only.
        return size_t(_write - _read);
    }

private:
    static void copyBounded(char* dst, const char* src, size_t cap) {
        if (src == nullptr) { dst[0] = '\0'; return; }
        size_t i = 0;
        for (; i + 1 < cap && src[i] != '\0'; ++i) dst[i] = src[i];
        dst[i] = '\0';
    }

    IPort& _port;
    Level _floor;
    Record _ring[Slots] = {};
    // Free-running 32-bit indices (house seq-ring idiom): pending = write-read.
    uint32_t _write = 0;
    uint32_t _read = 0;
    uint16_t _lostSinceDrain = 0;
    uint32_t _totalLostShadow = 0;
    ISink* _sinks[MaxSinks] = {};
    Level _sinkFloors[MaxSinks] = {};
    size_t _sinkCount = 0;
    bool _immediateDrain = false;
};

}  // namespace sloplog
