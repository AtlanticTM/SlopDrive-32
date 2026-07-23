// ============================================================================
// test_main.cpp — doctest unit tests for slopsync-core's M4 channel-layer
// building blocks:
//   channel/intent_registry.hpp  (IntentRing, IngressRateLimiter — §9.3)
//   channel/subscription.hpp     (SubscriptionTable, SubscriptionEntry — §10.2, §9.1)
//   channel/event_channel.hpp    (EventQueue — §9.4)
//   channel/retained_store.hpp   (RetainedStore — §9.1)
//
// Native (host-side, hardware-free) test, same pattern as
// test/native/test_slopsync_catalog/test_main.cpp: doctest's bundled main(),
// no Arduino, no bus/FreeRTOS dependency — header-only, entirely math/logic.
// All clock inputs are explicit nowMs values passed straight to the APIs
// under test (they take nowMs directly, not an IClock) — zero real-time
// dependence. SPEC section numbers cite docs/slopsync/SPEC.md.
//
// These four headers are PURE, session-engine-independent building blocks
// (M4 scope): no transport, no Hub/Client. Where a test wants to show how
// two of them cooperate (RetainedStore + SubscriptionTable's pacing =
// conflation with zero queues, per both files' design notes), the test
// wires them together itself — that wiring is exactly what a later
// milestone's session engine will do for real.
// ============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "slopsync/channel/event_channel.hpp"
#include "slopsync/channel/intent_registry.hpp"
#include "slopsync/channel/retained_store.hpp"
#include "slopsync/channel/subscription.hpp"
#include "slopsync/generated/registry_constants.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

using namespace slopsync;

namespace {
constexpr std::byte B(int x) { return std::byte(uint8_t(x)); }

std::vector<std::byte> bytesOf(std::initializer_list<int> vals) {
    std::vector<std::byte> v;
    v.reserve(vals.size());
    for (int x : vals) v.push_back(B(x));
    return v;
}

// N bytes, all with value `fill` — used for oversize-rejection tests where
// content doesn't matter, only size does.
std::vector<std::byte> fillBytes(size_t n, int fill) {
    return std::vector<std::byte>(n, B(fill));
}
}  // namespace

// ============================================================================
// IntentRing — §9.3 idempotency ring.
// ============================================================================
TEST_CASE("IntentRing: store/lookup roundtrip") {
    IntentRing<> ring;
    auto echo = bytesOf({0xDE, 0xAD, 0xBE, 0xEF});

    CHECK(ring.store(1, echo));
    auto found = ring.lookup(1);
    REQUIRE(found.has_value());
    REQUIRE(found->size() == echo.size());
    CHECK(std::equal(found->begin(), found->end(), echo.begin()));

    // Never-stored id.
    CHECK_FALSE(ring.lookup(999).has_value());
}

TEST_CASE("IntentRing: 33rd store evicts oldest (id 1 gone, 2..33 present)") {
    IntentRing<> ring;  // default Depth == limits::idempotency_ring_depth == 32
    REQUIRE(IntentRing<>::kDepth == 32);

    for (uint16_t id = 1; id <= 32; ++id) {
        auto echo = bytesOf({int(id)});
        REQUIRE(ring.store(id, echo));
    }
    REQUIRE(ring.size() == 32);
    CHECK(ring.lookup(1).has_value());  // still resident before the 33rd store

    auto echo33 = bytesOf({33});
    REQUIRE(ring.store(33, echo33));
    REQUIRE(ring.size() == 32);  // saturates, never grows past Depth

    CHECK_FALSE(ring.lookup(1).has_value());  // evicted: oldest slot overwritten
    for (uint16_t id = 2; id <= 33; ++id) {
        auto found = ring.lookup(id);
        REQUIRE(found.has_value());
        REQUIRE(found->size() == 1);
        CHECK(uint8_t((*found)[0]) == uint8_t(id));
    }
}

TEST_CASE("IntentRing: duplicate id lookup returns stored bytes unchanged (byte-exact)") {
    IntentRing<> ring;
    auto echo = bytesOf({1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
    REQUIRE(ring.store(42, echo));

    // I-02's core mechanism: repeated lookups of the same (duplicate) id
    // return byte-identical stored ECHO bytes every time, with no re-apply
    // (there is nothing here TO re-apply — lookup() never mutates the ring).
    for (int i = 0; i < 5; ++i) {
        auto found = ring.lookup(42);
        REQUIRE(found.has_value());
        REQUIRE(found->size() == echo.size());
        CHECK(std::equal(found->begin(), found->end(), echo.begin()));
    }
    CHECK(ring.size() == 1);
}

TEST_CASE("IntentRing: clear() empties") {
    IntentRing<> ring;
    for (uint16_t id = 1; id <= 5; ++id) REQUIRE(ring.store(id, bytesOf({int(id)})));
    REQUIRE(ring.size() == 5);

    ring.clear();
    CHECK(ring.size() == 0);
    for (uint16_t id = 1; id <= 5; ++id) CHECK_FALSE(ring.lookup(id).has_value());

    // Ring is fully usable after clear(), not just emptied.
    REQUIRE(ring.store(1, bytesOf({0x99})));
    CHECK(ring.lookup(1).has_value());
}

TEST_CASE("IntentRing: an echo larger than kSlotCapacity is rejected, ring unmodified") {
    IntentRing<> ring;
    REQUIRE(IntentRing<>::kSlotCapacity == 96);

    auto oversize = fillBytes(97, 0xAA);
    CHECK_FALSE(ring.store(1, oversize));
    CHECK(ring.size() == 0);
    CHECK_FALSE(ring.lookup(1).has_value());

    // Exactly at capacity still fits.
    auto exact = fillBytes(96, 0xBB);
    CHECK(ring.store(2, exact));
    CHECK(ring.lookup(2).has_value());
}

// ============================================================================
// IngressRateLimiter — §9.3/§10.5 ingress rate limiting.
// ============================================================================
TEST_CASE("IngressRateLimiter: 50 allowed in second one, 51st denied") {
    IngressRateLimiter limiter(50, /*nowMs=*/0);
    for (int i = 0; i < 50; ++i) {
        CHECK(limiter.allow(0));
    }
    CHECK_FALSE(limiter.allow(0));
}

TEST_CASE("IngressRateLimiter: refills next second") {
    IngressRateLimiter limiter(50, /*nowMs=*/0);
    for (int i = 0; i < 50; ++i) REQUIRE(limiter.allow(0));
    REQUIRE_FALSE(limiter.allow(0));

    // A full second later, the bucket has refilled back to capacity: another
    // full 50 are allowed, and the 51st of THIS second is denied too.
    for (int i = 0; i < 50; ++i) CHECK(limiter.allow(1000));
    CHECK_FALSE(limiter.allow(1000));
}

TEST_CASE("IngressRateLimiter: burst then sustained pattern") {
    IngressRateLimiter limiter(50, /*nowMs=*/0);

    // Burst: a full second's worth allowed immediately (burst tolerance of
    // one second's worth, per the registry default's own framing — "generous
    // for UIs").
    for (int i = 0; i < 50; ++i) REQUIRE(limiter.allow(0));
    REQUIRE_FALSE(limiter.allow(0));

    // Sustained: one call every 20ms (== 50/s) forever after must never be
    // denied — the continuous refill exactly matches steady-state
    // consumption at the configured rate.
    uint32_t t = 0;
    for (int i = 0; i < 500; ++i) {
        t += 20;
        CHECK(limiter.allow(t));
    }
}

// ============================================================================
// SubscriptionTable — §10.2 grants, §9.1 push pacing.
// ============================================================================
TEST_CASE("SubscriptionTable: upsert-replaces semantics") {
    SubscriptionTable<> table;
    CHECK(table.upsert(0x0100, 10.0f, Priority::normal));
    CHECK(table.size() == 1);

    // Re-subscribe the same channel with a different rate/priority: table
    // size stays 1, entry is updated in place (§6.6).
    CHECK(table.upsert(0x0100, 30.0f, Priority::elevated));
    CHECK(table.size() == 1);

    const SubscriptionEntry* e = table.find(0x0100);
    REQUIRE(e != nullptr);
    CHECK(e->granted_rate_hz == doctest::Approx(30.0f));
    CHECK(e->priority == Priority::elevated);
}

TEST_CASE("SubscriptionTable: capacity 64 enforced") {
    SubscriptionTable<> table;
    REQUIRE(SubscriptionTable<>::kCapacity == 64);

    for (uint16_t id = 0; id < 64; ++id) {
        REQUIRE(table.upsert(id, 1.0f, Priority::normal));
    }
    CHECK(table.size() == 64);

    // 65th distinct channel is rejected; existing 64 are untouched.
    CHECK_FALSE(table.upsert(64, 1.0f, Priority::normal));
    CHECK(table.size() == 64);
    CHECK(table.find(64) == nullptr);

    // Re-subscribing an EXISTING channel is still fine at full capacity
    // (upsert-replace never grows the table).
    CHECK(table.upsert(0, 5.0f, Priority::critical));
    CHECK(table.size() == 64);
}

TEST_CASE("SubscriptionEntry::dueForPush: rate 0 (on-change) semantics") {
    SubscriptionTable<> table;
    REQUIRE(table.upsert(0x0200, 0.0f, Priority::normal));
    SubscriptionEntry* e = table.find(0x0200);
    REQUIRE(e != nullptr);

    // Fresh grant: due immediately regardless of changePending (§9.1
    // push-on-grant of the retained value).
    CHECK(e->dueForPush(0, /*changePending=*/false));
    e->markPushed(0);

    // No change since the last push: never due.
    CHECK_FALSE(e->dueForPush(1000, /*changePending=*/false));

    // rate 0 = on-change only: any pending change is due immediately, no
    // rate-ceiling wait.
    CHECK(e->dueForPush(1, /*changePending=*/true));
}

TEST_CASE("SubscriptionEntry::dueForPush: rate 10Hz due at most every 100ms") {
    SubscriptionTable<> table;
    REQUIRE(table.upsert(0x0201, 10.0f, Priority::normal));
    SubscriptionEntry* e = table.find(0x0201);
    REQUIRE(e != nullptr);
    e->markPushed(0);  // simulate the push-on-grant already having happened

    // Before one period (100ms) has elapsed, even with a change pending,
    // it's not due yet — that's the rate CEILING.
    CHECK_FALSE(e->dueForPush(50, /*changePending=*/true));
    CHECK_FALSE(e->dueForPush(99, /*changePending=*/true));

    // No change pending: never due, no matter how much time passed.
    CHECK_FALSE(e->dueForPush(500, /*changePending=*/false));

    // Exactly at the period boundary, due (timeReached is inclusive).
    CHECK(e->dueForPush(100, /*changePending=*/true));

    e->markPushed(100);
    CHECK_FALSE(e->dueForPush(150, /*changePending=*/true));  // only 50ms since last push
    CHECK(e->dueForPush(200, /*changePending=*/true));
}

TEST_CASE("Conflation semantics: two publishes between dues -> one push of latest") {
    // Demonstrates the design note in subscription.hpp/retained_store.hpp:
    // RetainedStore holds only the latest value; SubscriptionEntry's pacing
    // decides only WHEN to read it. A session engine tracks "last seq this
    // subscriber was sent" itself (deliberately outside both classes, see
    // subscription.hpp's header comment) — modeled here with a plain local.
    SubscriptionTable<> table;
    REQUIRE(table.upsert(0x0003, 10.0f, Priority::critical));  // safety channel, arbitrary here
    SubscriptionEntry* e = table.find(0x0003);
    REQUIRE(e != nullptr);
    e->markPushed(0);

    RetainedStore<> store;
    uint16_t lastSentSeq = 0;
    // Baseline publish so there is a value at all.
    auto seq0 = store.publish(0x0003, bytesOf({0}));
    REQUIRE(seq0.has_value());
    lastSentSeq = *seq0;

    // Two publishes land inside one pacing window (before the 100ms due
    // point) — a real hub would call publish() as new safety-word values
    // arrive; only the LATEST is ever retained.
    auto seqA = store.publish(0x0003, bytesOf({0xAA}));
    REQUIRE(seqA.has_value());
    auto seqB = store.publish(0x0003, bytesOf({0xBB}));
    REQUIRE(seqB.has_value());
    CHECK(*seqB == *seqA + 1);  // seq bumps monotonically per publish, §9.1

    bool changePending = (*seqB != lastSentSeq);
    CHECK(changePending);

    // Not yet due (period is 100ms, only 50ms elapsed) — neither A nor B has
    // been sent; there is no queue holding A "waiting its turn".
    CHECK_FALSE(e->dueForPush(50, changePending));

    // At the due point, exactly one push happens, and what it carries is
    // whatever RetainedStore currently holds — B, the latest, never A.
    REQUIRE(e->dueForPush(100, changePending));
    auto view = store.get(0x0003);
    REQUIRE(view.has_value());
    CHECK(view->seq == *seqB);
    REQUIRE(view->payload.size() == 1);
    CHECK(uint8_t(view->payload[0]) == 0xBB);

    e->markPushed(100);
    lastSentSeq = view->seq;
    CHECK_FALSE(e->dueForPush(150, /*changePending=*/(view->seq != lastSentSeq)));
}

TEST_CASE("SubscriptionTable: remove() and iteration") {
    SubscriptionTable<> table;
    REQUIRE(table.upsert(1, 1.0f, Priority::background));
    REQUIRE(table.upsert(2, 2.0f, Priority::normal));
    REQUIRE(table.upsert(3, 3.0f, Priority::elevated));
    REQUIRE(table.size() == 3);

    CHECK(table.remove(2));
    CHECK(table.size() == 2);
    CHECK(table.find(2) == nullptr);
    CHECK_FALSE(table.remove(2));  // already gone

    size_t seen = 0;
    for (const auto& e : table) {
        CHECK((e.channel_id == 1 || e.channel_id == 3));
        ++seen;
    }
    CHECK(seen == 2);
}

// ============================================================================
// EventQueue — §9.4 bounded per-subscriber event queue.
// ============================================================================
TEST_CASE("EventQueue: fill 16, push 17th -> oldest dropped, counter 1, FIFO order intact") {
    EventQueue<> q;
    REQUIRE(EventQueue<>::kDepth == 16);

    for (int i = 1; i <= 16; ++i) {
        REQUIRE(q.push(bytesOf({i})));
    }
    CHECK(q.size() == 16);
    CHECK(q.full());
    CHECK(q.eventsDropped() == 0);

    // 17th push drops the oldest (event "1") to make room.
    REQUIRE(q.push(bytesOf({17})));
    CHECK(q.size() == 16);  // saturates, never exceeds Depth
    CHECK(q.eventsDropped() == 1);

    // Remaining 16 are events 2..17, in FIFO order (event "1" is gone).
    for (int expected = 2; expected <= 17; ++expected) {
        auto popped = q.pop();
        REQUIRE(popped.has_value());
        REQUIRE(popped->size() == 1);
        CHECK(uint8_t((*popped)[0]) == uint8_t(expected));
    }
    CHECK(q.empty());
    CHECK_FALSE(q.pop().has_value());
}

TEST_CASE("EventQueue: an event larger than kSlotCapacity is rejected, queue unmodified") {
    EventQueue<> q;
    REQUIRE(EventQueue<>::kSlotCapacity == 128);

    auto oversize = fillBytes(129, 0xCC);
    CHECK_FALSE(q.push(oversize));
    CHECK(q.size() == 0);
    CHECK(q.eventsDropped() == 0);  // rejection is not the same thing as overflow-drop

    auto exact = fillBytes(128, 0xDD);
    CHECK(q.push(exact));
    CHECK(q.size() == 1);
}

// ============================================================================
// RetainedStore — §9.1 hub-side retained STATE values.
// ============================================================================
TEST_CASE("RetainedStore: publish bumps seq monotonically; get returns latest bytes") {
    RetainedStore<> store;

    auto s1 = store.publish(0x0010, bytesOf({1, 1, 1}));
    REQUIRE(s1.has_value());
    CHECK(*s1 == 1);

    auto s2 = store.publish(0x0010, bytesOf({2, 2, 2, 2}));
    REQUIRE(s2.has_value());
    CHECK(*s2 == 2);

    auto s3 = store.publish(0x0010, bytesOf({3}));
    REQUIRE(s3.has_value());
    CHECK(*s3 == 3);

    auto view = store.get(0x0010);
    REQUIRE(view.has_value());
    CHECK(view->seq == 3);
    REQUIRE(view->payload.size() == 1);
    CHECK(uint8_t(view->payload[0]) == 3);
}

TEST_CASE("RetainedStore: get() on a never-published channel is nullopt") {
    RetainedStore<> store;
    CHECK_FALSE(store.get(0xBEEF).has_value());

    store.publish(0x0001, bytesOf({9}));
    CHECK_FALSE(store.get(0x0002).has_value());  // a different, still-unpublished channel
}

TEST_CASE("RetainedStore: capacity behavior") {
    RetainedStore<32> store;
    for (uint16_t ch = 0; ch < 32; ++ch) {
        auto s = store.publish(ch, bytesOf({int(ch)}));
        REQUIRE(s.has_value());
    }

    // 33rd distinct channel is rejected; the 32 existing entries are
    // untouched and still readable.
    CHECK_FALSE(store.publish(32, bytesOf({1})).has_value());
    CHECK_FALSE(store.get(32).has_value());
    CHECK(store.get(0).has_value());
    CHECK(store.get(31).has_value());

    // Re-publishing an EXISTING channel at full capacity is still fine
    // (publish-replace never grows the store).
    auto s = store.publish(0, bytesOf({0xFF}));
    REQUIRE(s.has_value());
    CHECK(*s == 2);  // channel 0's second publish
}

TEST_CASE("RetainedStore: payload > 242 bytes (min_transport_payload) is rejected") {
    RetainedStore<> store;
    REQUIRE(limits::min_transport_payload == 242);

    auto oversize = fillBytes(243, 0xEE);
    CHECK_FALSE(store.publish(0x0020, oversize).has_value());
    CHECK_FALSE(store.get(0x0020).has_value());

    // Exactly at the MTU floor still fits.
    auto exact = fillBytes(242, 0xEF);
    auto s = store.publish(0x0020, exact);
    REQUIRE(s.has_value());
    CHECK(*s == 1);
    auto view = store.get(0x0020);
    REQUIRE(view.has_value());
    CHECK(view->payload.size() == 242);
}
