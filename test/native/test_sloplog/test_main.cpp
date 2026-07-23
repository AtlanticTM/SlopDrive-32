// SlopLog — hardware-free core suite. Exercises the ring/sink/floor/drop
// contract with an injected port (manual clock, counting lock) — the same
// determinism posture as the slopsync suites.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "sloplog/sloplog_core.hpp"

using namespace sloplog;

namespace {

struct FakePort final : IPort {
    uint32_t ms = 0;
    uint8_t core = 0;
    int lockDepth = 0;
    int maxDepth = 0;

    uint32_t nowMs() override { return ms; }
    uint8_t coreId() override { return core; }
    void lock() override {
        ++lockDepth;
        if (lockDepth > maxDepth) maxDepth = lockDepth;
    }
    void unlock() override { --lockDepth; }
};

struct CaptureSink final : ISink {
    std::vector<Record> got;
    void write(const Record& r) override { got.push_back(r); }
};

}  // namespace

TEST_CASE("logf formats, stamps, and drains in order") {
    FakePort port;
    LogCore<16> log(port);
    CaptureSink sink;
    REQUIRE(log.addSink(&sink));

    port.ms = 1234;
    port.core = 1;
    log.logf(Level::Info, "wifi", "connected ch%d rssi %d", 6, -52);
    port.ms = 1300;
    port.core = 0;
    log.logf(Level::Warn, "arbiter", "rejected: %s", "not homed");

    CHECK(log.pending() == 2);
    CHECK(log.drain() == 2);
    CHECK(log.pending() == 0);
    REQUIRE(sink.got.size() == 2);

    CHECK(sink.got[0].ms == 1234);
    CHECK(sink.got[0].core == 1);
    CHECK(sink.got[0].level == Level::Info);
    CHECK(std::string(sink.got[0].tag) == "wifi");
    CHECK(std::string(sink.got[0].msg) == "connected ch6 rssi -52");

    CHECK(sink.got[1].level == Level::Warn);
    CHECK(std::string(sink.got[1].msg) == "rejected: not homed");

    CHECK(port.lockDepth == 0);   // every lock released
    CHECK(port.maxDepth == 1);    // never nested
}

TEST_CASE("floor rejects below, runtime-adjustable") {
    FakePort port;
    LogCore<16> log(port, Level::Warn);
    CaptureSink sink;
    log.addSink(&sink);

    log.logf(Level::Info, "t", "dropped");
    log.logf(Level::Warn, "t", "kept");
    log.setFloor(Level::Trace);
    log.logf(Level::Debug, "t", "kept now");

    log.drain();
    REQUIRE(sink.got.size() == 2);
    CHECK(std::string(sink.got[0].msg) == "kept");
    CHECK(std::string(sink.got[1].msg) == "kept now");
}

TEST_CASE("tag and message truncate at their bounds, always NUL-terminated") {
    FakePort port;
    LogCore<16> log(port);
    CaptureSink sink;
    log.addSink(&sink);

    std::string longTag(40, 'T');
    std::string longMsg(300, 'M');
    log.logf(Level::Info, longTag.c_str(), "%s", longMsg.c_str());
    log.drain();

    REQUIRE(sink.got.size() == 1);
    CHECK(std::string(sink.got[0].tag).size() == Record::kTagBytes - 1);
    CHECK(std::string(sink.got[0].msg).size() == Record::kMsgBytes - 1);
}

TEST_CASE("overflow drops oldest and accounts the loss") {
    FakePort port;
    LogCore<8> log(port);
    CaptureSink sink;
    log.addSink(&sink);

    for (int i = 0; i < 11; ++i) log.logf(Level::Info, "t", "m%d", i);

    // 8 slots, 11 pushes: m0..m2 dropped, m3..m10 survive.
    CHECK(log.pending() == 8);
    CHECK(log.totalLost() == 3);
    log.drain();
    REQUIRE(sink.got.size() == 8);
    CHECK(std::string(sink.got[0].msg) == "m3");
    CHECK(std::string(sink.got[7].msg) == "m10");
    // The loss count rides on a surviving record so a reader can see the gap.
    CHECK(sink.got.back().lost == 3);

    // After drain the ring is clean and losses don't leak into new records.
    log.logf(Level::Info, "t", "fresh");
    log.drain();
    CHECK(sink.got.back().lost == 0);
    CHECK(log.totalLost() == 3);  // lifetime counter unaffected by drain
}

TEST_CASE("drain honors maxRecords and multiple sinks see every record") {
    FakePort port;
    LogCore<16> log(port);
    CaptureSink a, b;
    log.addSink(&a);
    log.addSink(&b);

    for (int i = 0; i < 6; ++i) log.logf(Level::Info, "t", "m%d", i);
    CHECK(log.drain(4) == 4);
    CHECK(a.got.size() == 4);
    CHECK(log.drain() == 2);
    CHECK(a.got.size() == 6);
    CHECK(b.got.size() == 6);
}

TEST_CASE("null and overflow sink registration refused") {
    FakePort port;
    LogCore<16, 2> log(port);
    CaptureSink s1, s2, s3;
    CHECK_FALSE(log.addSink(nullptr));
    CHECK(log.addSink(&s1));
    CHECK(log.addSink(&s2));
    CHECK_FALSE(log.addSink(&s3));  // MaxSinks = 2
}
