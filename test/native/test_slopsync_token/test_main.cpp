// test_slopsync_token -- the HELLO credential precedence, pinned on the host.
//
// Why this suite exists: validateToken() decides whether a client owns the
// write plane, and every rung of it used to be reachable only from a live
// HELLO. The one property no live test can see is ORDER: a /uitoken is
// single-use, so it must be consumed before the ledger is consulted, or a page
// can hoard a mint and replay it after the posture is tightened. That is a
// call-order fact, and a recorder proves it where a device cannot.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "SlopSyncTokenPolicy.h"

using slopdrive::decideTokenGrant;
using slopsync::AccessLevel;

namespace {

struct Probes {
    std::vector<std::string> calls;
    bool uitoken_matches = false;
    AccessLevel ledger = AccessLevel::watch;

    auto consume() {
        return [this] {
            calls.push_back("uitoken");
            return uitoken_matches;
        };
    }
    auto lookup() {
        return [this] {
            calls.push_back("ledger");
            return ledger;
        };
    }
};

}  // namespace

TEST_CASE("rung 1: a matching /uitoken grants control and is burned") {
    Probes p;
    p.uitoken_matches = true;
    p.ledger = AccessLevel::configure;

    const auto g = decideTokenGrant(true, p.consume(), p.lookup());

    CHECK(g.granted == AccessLevel::control);
    CHECK(g.uitoken_burned);
    // Burned even though the ledger would have granted a HIGHER tier, and the
    // ledger is never consulted once it matched.
    REQUIRE(p.calls.size() == 1);
    CHECK(p.calls[0] == "uitoken");
}

TEST_CASE("the /uitoken probe runs first whenever a token is present") {
    Probes p;
    p.uitoken_matches = false;
    p.ledger = AccessLevel::control;

    const auto g = decideTokenGrant(true, p.consume(), p.lookup());

    CHECK(g.granted == AccessLevel::control);
    CHECK_FALSE(g.uitoken_burned);
    REQUIRE(p.calls.size() == 2);
    CHECK(p.calls[0] == "uitoken");
    CHECK(p.calls[1] == "ledger");
}

TEST_CASE("rung 2: the ledger tier passes through, watch does not") {
    SUBCASE("configure") {
        Probes p;
        p.ledger = AccessLevel::configure;
        const auto g = decideTokenGrant(true, p.consume(), p.lookup());
        CHECK(g.granted == AccessLevel::configure);
        CHECK_FALSE(g.uitoken_burned);
    }
    SUBCASE("watch is a non-answer, not a grant") {
        Probes p;
        p.ledger = AccessLevel::watch;
        const auto g = decideTokenGrant(true, p.consume(), p.lookup());
        CHECK(g.granted == AccessLevel::watch);
        CHECK_FALSE(g.uitoken_burned);
    }
}

TEST_CASE("rung 3: no token consults nothing and floors at watch") {
    Probes p;
    p.uitoken_matches = true;   // would match, but must never be asked
    p.ledger = AccessLevel::configure;

    const auto g = decideTokenGrant(false, p.consume(), p.lookup());

    CHECK(g.granted == AccessLevel::watch);
    CHECK_FALSE(g.uitoken_burned);
    CHECK(p.calls.empty());
}
