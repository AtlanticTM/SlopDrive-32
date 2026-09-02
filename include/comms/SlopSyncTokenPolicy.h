// SlopSyncTokenPolicy -- HELLO credential precedence (/uitoken, trust ledger,
// watch) as one hardware-free decision function.
// Constraints:
//   BEHAVIOR-PRESERVING EXTRACTION ONLY: an edit that alters a granted tier or
//   the probe ORDER is a security change and needs an operator ruling first.
//   Probes are INJECTED and evaluated lazily in rung order: the /uitoken probe
//   BURNS (single-use) so it runs first and unconditionally, and the ledger
//   probe must not run once the token matched.
//   `uitoken_burned` REPORTS the probe consumed a token; it is not an
//   instruction to burn one.
//   Hardware-free: no Arduino, no logging, no SystemState.
// See:
//   .claude/rules/transport.md (Auth); SlopDriveHubDelegate::validateToken;
//   test/native/test_slopsync_token

#pragma once

#include "slopsync/generated/registry_constants.hpp"

namespace slopdrive {

struct TokenGrant {
    slopsync::AccessLevel granted;
    bool uitoken_burned;
};

// consume_uitoken() -> bool: a live /uitoken matched and has been CONSUMED.
// ledger_role() -> AccessLevel: the trust ledger's answer; `watch` from it is a
// non-answer, not a grant.
template <class ConsumeUiToken, class LedgerRole>
TokenGrant decideTokenGrant(bool has_token, ConsumeUiToken&& consume_uitoken, LedgerRole&& ledger_role) {
    if (has_token && consume_uitoken()) {
        return {slopsync::AccessLevel::control, true};
    }
    if (has_token) {
        const slopsync::AccessLevel role = ledger_role();
        if (role > slopsync::AccessLevel::watch) return {role, false};
    }
    return {slopsync::AccessLevel::watch, false};
}

}  // namespace slopdrive
