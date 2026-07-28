// slopsync-core — PAIRING and TRUST: the §12.2 ceremony, RFC-027's three
// association modes, and RFC-029's trust ledger.
//
// ONE CEREMONY, THREE ASSOCIATION MODES, ALL ENDING IN PAIR_GRANT{token, role}
// (RFC-027.2). The ROLE is an attribute of the GRANT, never of the ceremony —
// there is zero or one PIN on a hub, never one secret per tier.
//
//   (a) KNOCK-AND-APPROVE — the PRIMARY mode, and the only capability-agnostic
//       one. A bare PAIR_REQ carrying NO proof lands in a bounded pending list
//       (limits::pairing_pending_max = 4, so a knock flood exhausts nothing),
//       which is published as protocol STATE on 0x000A with an EVENT twin on
//       0x000B. Any `configure` session approves {instance_id, role} through
//       the 0x0009 admin surface. THE JOINER NEEDS ONE BUTTON AND NO DISPLAY —
//       that is the whole point, because a coin-cell remote has neither a
//       keypad nor a screen, and because the trusted surface being "any
//       configure client" is what kills the circular "the WebUI is trusted
//       because it is the WebUI" dependency.
//
//   (b) NUMERIC PROOF — today's HMAC-PIN flow, kept for keyboard-bearing
//       joiners when no configure session exists. HONESTY CLAUSE, normative
//       (RFC-027.5): 4 digits is 10^4 offline HMACs, so a passive observer of
//       the pairing exchange can brute-force the PIN. That is acceptable for
//       the v1 threat model (casual/drive-by prevention) and MUST be stated
//       plainly rather than implied away. SPAKE2 is the reserved v2 upgrade.
//
//   (c) PUSH-TO-PAIR — a PHYSICAL-PRESENCE proof opens a short SINGLE-GRANT
//       window and the first knock in it is granted with no approval. The spec
//       requires the PROOF, not a GPIO: the bare-minimum hardware is NONE,
//       because the power cord is the button. This file provides the WINDOW;
//       the GESTURE that opens it is the application's (firmware milestone 5:
//       the NVS boot-counter — N consecutive boots with uptime < ~10 s).
//       FACTORY-FRESH RULE: if ZERO configure tokens exist, the window grants
//       `configure`, because physical possession is root (Matter/Chromecast
//       commissioning semantics). Thereafter it grants the configured default.
//
// THE TRUST LEDGER (RFC-029 item 2, RFC-027.4) is this file's other half. Each
// paired device carries {instance_id, kind, name, version, first_seen,
// last_seen, role, state} plus its presentation and pairing modes. The
// feasibility pass ruled it a BLOB STORE and never a packed STATE roster — an
// entry is ~96 encoded bytes, so a 242 B snapshot holds two of them, which is
// unusable. Persistence is the APPLICATION's (milestone 5, NVS): encodeLedger/
// decodeLedger are the seam, one blob, kept under trust_ledger_max_bytes.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "slopsync/core/crypto.hpp"
#include "slopsync/core/rng.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/util/bounded_string.hpp"
#include "slopsync/util/serial_arithmetic.hpp"
#include "slopsync/wire/cbor/cbor_reader.hpp"
#include "slopsync/wire/cbor/cbor_writer.hpp"
#include "slopsync/wire/hmac_sha256.hpp"
#include "slopsync/wire/messages/trust_submap.hpp"  // kTrustTokenProofBytes — the ONE owner of the proof width

namespace slopsync {

// Ledger string caps (registry `trust_ledger_keys` notes). Fixed-width and
// truncating, never rejecting: a roster entry that vanishes because a client
// picked a long name is a worse failure than a shortened label, and the
// authoritative full name still rides HELLO/0x0007 while the session lives.
inline constexpr size_t kLedgerKindMaxBytes = size_t(limits::trust_ledger_kind_max_bytes);
inline constexpr size_t kLedgerNameMaxBytes = size_t(limits::trust_ledger_name_max_bytes);
inline constexpr size_t kLedgerVersionMaxBytes = size_t(limits::client_ver_max_bytes);

// ---- The PENDING list (RFC-027(a)) ------------------------------------------
// knocks awaiting a configure session's answer.

// One pending knock. `mode` is the registry's `kind u8` field of the 0x000A
// slot layout: the `pairing_modes` BIT that produced this knock, so an
// approving UI can say HOW the device asked.
struct PendingKnock {
    std::array<std::byte, limits::instance_id_bytes> instance_id{};
    uint32_t session_id = 0;   // the live session that knocked (see below)
    uint32_t expiresAtMs = 0;  // wrap-safe deadline, §7.2
    uint8_t mode = pairing_modes::knock_approve;
    BoundedString<kLedgerKindMaxBytes> kind{};
    BoundedString<kLedgerNameMaxBytes> name{};
    BoundedString<kLedgerVersionMaxBytes> version{};
    // RFC-029 item 2: this entry is a paired device asking to be RE-trusted
    // after its version changed, not a stranger asking to be let in. Same
    // approval verb, different sentence in the UI ("plugin 0.2.3 -> 0.3.0 —
    // keep trusting?" rather than "new device wants control").
    bool reapproval = false;
    bool used = false;
};

// WHY A KNOCK IS BOUND TO A LIVE SESSION. A grant has to be DELIVERED, and the
// only channel to a joiner that owns no token is its current session. The
// alternative — mint the token and hold it for a knocker that reconnects
// later — means the hub would have to hand a token to an unauthenticated
// re-knock claiming that instance_id, i.e. a free credential for any spoofer
// for the life of the entry. So: pending knocks die with their session, and
// approving a departed knocker fails honestly instead of widening the window.
// The cost is that a joiner must stay awake for the ~120 s it takes an
// operator to press approve, which is trivially true of something that just
// had its button pressed. This also puts pending-knock cleanup on the SAME
// teardown path as source ownership, which is exactly where this project's
// third field bug taught us session-scoped state belongs.
template <size_t Cap = size_t(limits::pairing_pending_max)>
class PendingPairingList {
public:
    static constexpr size_t kCapacity = Cap;

    // Adds or REFRESHES a knock for this instance. Returns nullptr only when
    // the list is full of OTHER instances — that bound is what makes a knock
    // flood harmless: it costs an attacker nothing to send and costs the hub a
    // fixed 4 slots, and a re-knock from an already-pending device refreshes
    // rather than consuming a second one.
    PendingKnock* add(std::span<const std::byte> instance_id, uint32_t session_id, uint32_t expiresAtMs,
                      uint8_t mode) {
        PendingKnock* k = find(instance_id);
        if (k == nullptr) {
            for (auto& e : _slots) {
                if (!e.used) { k = &e; break; }
            }
            if (k == nullptr) return nullptr;
            *k = PendingKnock{};
            std::memcpy(k->instance_id.data(), instance_id.data(), k->instance_id.size());
        }
        k->used = true;
        k->session_id = session_id;
        k->expiresAtMs = expiresAtMs;
        k->mode = mode;
        return k;
    }

    PendingKnock* find(std::span<const std::byte> instance_id) {
        if (instance_id.size() != limits::instance_id_bytes) return nullptr;
        for (auto& e : _slots) {
            if (e.used && std::equal(e.instance_id.begin(), e.instance_id.end(), instance_id.begin())) return &e;
        }
        return nullptr;
    }
    const PendingKnock* find(std::span<const std::byte> instance_id) const {
        return const_cast<PendingPairingList*>(this)->find(instance_id);
    }

    bool remove(std::span<const std::byte> instance_id) {
        PendingKnock* k = find(instance_id);
        if (k == nullptr) return false;
        *k = PendingKnock{};
        return true;
    }

    // Fires `fn(const PendingKnock&)` for each knock whose window elapsed, then
    // frees it. Wrap-safe (§7.2). Returns how many expired.
    template <typename Fn>
    size_t expire(uint32_t nowMs, Fn&& fn) {
        size_t n = 0;
        for (auto& e : _slots) {
            if (!e.used || !timeReached(nowMs, e.expiresAtMs)) continue;
            fn(const_cast<const PendingKnock&>(e));
            e = PendingKnock{};
            ++n;
        }
        return n;
    }

    // Session teardown: drop every knock belonging to a departed session.
    template <typename Fn>
    size_t dropBySession(uint32_t session_id, Fn&& fn) {
        size_t n = 0;
        for (auto& e : _slots) {
            if (!e.used || e.session_id != session_id) continue;
            fn(const_cast<const PendingKnock&>(e));
            e = PendingKnock{};
            ++n;
        }
        return n;
    }

    size_t count() const {
        size_t n = 0;
        for (const auto& e : _slots) {
            if (e.used) ++n;
        }
        return n;
    }
    const PendingKnock* at(size_t i) const { return i < Cap ? &_slots[i] : nullptr; }

private:
    std::array<PendingKnock, Cap> _slots{};
};

// ---- PairingManager ---------------------------------------------------------
// the ledger, the ceremonies, and the windows.

class PairingManager {
public:
    static constexpr size_t kMaxPaired = size_t(limits::paired_devices_max);

    // RFC-029's trust-ledger entry. `state` is a registry `trust_states` value.
    struct PairedEntry {
        std::array<std::byte, limits::instance_id_bytes> instance_id{};
        std::array<std::byte, limits::token_bytes> token{};
        AccessLevel role = AccessLevel::control;
        uint8_t state = trust_states::trusted;
        uint8_t presentationMode = 0;                      // trust_keys.presentation_mode
        uint8_t pairingMode = pairing_modes::knock_approve;  // which ceremony paired it
        BoundedString<kLedgerKindMaxBytes> kind{};
        BoundedString<kLedgerNameMaxBytes> name{};
        BoundedString<kLedgerVersionMaxBytes> version{};
        // UNIX epoch seconds, or 0 for unknown — and 0 WILL be common. The
        // protocol's only clock is boot-relative and wraps in ~71 minutes
        // (§7.2), so these are filled in only when the application pushes a
        // wall clock (setWallClockSeconds). The library never invents one.
        uint32_t firstSeen = 0;
        uint32_t lastSeen = 0;
        bool used = false;
    };

    // ---- PIN window (§12.2, mode (b)) ---------------------------------------
    // `pinAscii` must stay valid while the window is open (points at app
    // memory, e.g. the digits also shown on the OLED/WebUI).
    void openWindow(std::span<const char> pinAscii, uint32_t nowMs);
    void closeWindow();
    bool windowOpen(uint32_t nowMs) const;  // auto-expires after limits::pairing_window_default_s

    // ---- Presence window (RFC-027(c), mode (c)) -----------------------------
    // SINGLE-GRANT: the first knock consumes it. Opened by the APPLICATION
    // after it observed a physical-presence proof (firmware M5: the boot-count
    // gesture); this library never touches a GPIO and never decides what
    // "presence" means.
    void openPresenceWindow(uint32_t nowMs);
    void closePresenceWindow();
    bool presenceWindowOpen(uint32_t nowMs) const;
    // THE FACTORY-FRESH RULE (RFC-027.2(c)): zero configure tokens in the
    // ledger means nobody has claimed this machine yet, so whoever unboxed and
    // powered it possesses it and the window grants `configure`. Once a
    // configure token exists, possession is no longer proof of ownership and
    // the window drops to the configured default (`control`), with
    // knock-and-approve doing the rest.
    AccessLevel presenceGrantRole() const;
    void setPresenceDefaultRole(AccessLevel r) { _presenceDefaultRole = r; }
    bool hasConfigureToken() const;

    // Which `pairing_modes` bits this hub is offering RIGHT NOW — the value
    // WELCOME advertises in `trust`.pairing_modes, re-evaluated per session so
    // a transient push-to-pair window is visible exactly while it is open.
    // knock_approve is offered iff `enableKnockApprove`; pin_proof iff a PIN
    // window is open; push_to_pair iff a presence window is open.
    uint8_t offeredModes(uint32_t nowMs) const;
    void setKnockApproveEnabled(bool on) { _knockApproveEnabled = on; }
    bool knockApproveEnabled() const { return _knockApproveEnabled; }

    // ---- Wall clock (the first_seen/last_seen seam) -------------------------
    // The application pushes UNIX epoch seconds when it HAS them (SNTP). 0
    // means unknown and is the honest default; see PairedEntry.
    void setWallClockSeconds(uint32_t s) { _wallSeconds = s; }
    uint32_t wallClockSeconds() const { return _wallSeconds; }

    // ---- PIN-proof ceremony (mode (b)) --------------------------------------
    enum class PairOutcome : uint8_t { Granted, Denied, WindowClosed };
    // Verifies proof against (PIN, nonce) in CONSTANT TIME through the injected
    // ICrypto (RFC-028.3 makes that protocol-wide). On success: creates or
    // replaces the instance's entry with a fresh rng token at `grantRole`,
    // returns Granted and fills tokenOut. Three consecutive failures close the
    // window (§12.2).
    PairOutcome handlePairReq(std::span<const std::byte> instance_id,
                              std::span<const std::byte> pin_proof,
                              std::span<const std::byte> nonce,
                              IRandom& rng, uint32_t nowMs,
                              AccessLevel grantRole,
                              std::span<std::byte> tokenOut,
                              ICrypto& crypto = defaultCrypto());

    // ---- Granting (shared by every mode) ------------------------------------
    // Mints a fresh token for `instance_id` at `role`, recording which ceremony
    // did it. Create-or-replace: re-pairing an already-known instance reissues
    // rather than consuming a second slot, and always resets `state` to trusted
    // (an approval IS the answer to a tripwire). Returns nullptr only when the
    // ledger is full of other devices. `tokenOut` may be empty.
    PairedEntry* grant(std::span<const std::byte> instance_id, AccessLevel role, uint8_t pairingMode,
                       IRandom& rng, std::span<std::byte> tokenOut);

    // ---- Token validation (HELLO path, §12.2) -------------------------------
    // CONSTANT-TIME token comparison (RFC-028.3). Returns `watch` for no match,
    // and `watch` for a RECOGNIZED-PENDING device whose role is suspended —
    // both are "you may look, you may not act", which is the whole design.
    AccessLevel validate(std::span<const std::byte> instance_id, std::span<const std::byte> token,
                         ICrypto& crypto = defaultCrypto()) const;

    // ---- RFC-029 item 6: PROOF presentation (the AUTH path) -----------------
    // Same answer as validate(), from a proof instead of the credential:
    // HMAC-SHA256(key = this device's token, message = the session's WELCOME
    // nonce) truncated to 16 bytes. The token NEVER crosses the wire, so a
    // passive observer of a cleartext v1 binding captures a one-time proof
    // rather than a credential that works forever.
    //
    // WHY THIS LIVES HERE AND NOT IN THE HUB: the stored token is the thing
    // being proved, and handing a raw token out of the ledger so a caller can
    // HMAC it itself would put live credentials in call-site locals for the
    // sake of code layout. The ledger computes and compares; nothing else ever
    // sees the bytes. Both the HMAC and the compare go through the injected
    // ICrypto (RFC-028.3 makes constant-time comparison protocol-wide).
    //
    // Returns `watch` for every failure mode — unknown device, wrong proof,
    // suspended device — exactly like validate(), so a caller cannot
    // accidentally distinguish "no such device" from "wrong proof" and turn the
    // ledger into an instance-id oracle.
    //
    // `proofMatched` (optional) reports the ONE distinction a hub legitimately
    // needs and a wire peer must never learn: whether the HMAC actually
    // verified. A RECOGNIZED-PENDING device that proves correctly returns
    // `watch` (its role is suspended, which is a person's decision to reverse)
    // yet must NOT burn an authentication strike, because nothing about it
    // failed to authenticate. Both cases still look identical on the wire — the
    // hub NACKs one and GRANTs `watch` for the other, which is exactly what the
    // client would have observed anyway.
    AccessLevel validateProof(std::span<const std::byte> instance_id, std::span<const std::byte> proof,
                              std::span<const std::byte> nonce, ICrypto& crypto = defaultCrypto(),
                              bool* proofMatched = nullptr) const;

    // ---- RFC-029 item 2: THE CLIENT-CHANGE TRIPWIRE -------------------------
    // Called once per HELLO from a device whose token validated. Records what
    // was observed and reports whether trust just dropped.
    //
    // HONESTY CLAUSE, NORMATIVE — and stated HERE because this function is
    // where a reader meets the mechanism: `client_ver` is SELF-REPORTED. This
    // catches an honest update and NOTHING ELSE. A deliberately malicious
    // update reports whatever version it likes and keeps its token; this is a
    // TRIPWIRE, not attestation, and no code or UI above it may imply
    // otherwise. What actually bounds a hostile client is role scoping,
    // instant revocation, its visibility in the roster, and the fact that the
    // safety ops are role-exempt for everyone including it.
    struct HelloObservation {
        bool known = false;            // the instance is in the ledger
        bool versionChanged = false;   // the tripwire fired on THIS hello
        bool suspended = false;        // ...and the granted role was suspended
        AccessLevel effectiveRole = AccessLevel::watch;
        AccessLevel suspendedRole = AccessLevel::watch;  // what re-approval restores
    };
    HelloObservation observeHello(std::span<const std::byte> instance_id, std::string_view client_kind,
                                  std::string_view client_name, std::string_view client_ver,
                                  bool hasClientVer, uint8_t presentationMode);

    // Trust-change policy (RFC-029 item 2, "hub-configurable"). A device whose
    // granted role is AT OR BELOW this level auto-rekeeps on a version change;
    // anything above it is suspended pending re-approval. Default `watch`,
    // i.e. exactly the RFC's stated default (watch auto-rekeeps, control and
    // configure require re-approval).
    void setTrustChangeAutoKeepMax(AccessLevel r) { _autoKeepMax = r; }
    AccessLevel trustChangeAutoKeepMax() const { return _autoKeepMax; }

    // ---- Ledger management (revocation, roster, NVS adapters) ---------------
    bool revoke(std::span<const std::byte> instance_id);
    size_t entryCount() const;
    const PairedEntry* entry(size_t i) const;        // i-th USED entry, compacted
    const PairedEntry* slot(size_t i) const;         // i-th raw slot (BLOB addressing)
    PairedEntry* findByInstance(std::span<const std::byte> instance_id);
    const PairedEntry* findByInstance(std::span<const std::byte> instance_id) const;
    bool importEntry(const PairedEntry& e);          // app restores from NVS at boot
    uint16_t generation() const { return _generation; }

    // ---- THE M5 PERSISTENCE SEAM --------------------------------------------
    // The whole ledger as ONE CBOR blob (registry `trust_ledger_keys`), which
    // is exactly the shape the feasibility pass mandates for NVS: one blob in
    // the existing `slopsync` namespace, under trust_ledger_max_bytes (1900 =
    // one flash page), WRITTEN ONLY ON CHANGE, and gated on `ota_active` the
    // same way savePairing() already is — flash-cache writes during an OTA
    // reset the chip. `dirty()` is the "only on change" half; the firmware
    // clears it after a successful commit.
    size_t encodeLedger(std::span<std::byte> out) const;
    bool decodeLedger(std::span<const std::byte> in);
    // ONE ledger item, the unit a BLOB_REQ on the paired-devices store serves.
    size_t encodeEntry(const PairedEntry& e, std::span<std::byte> out) const;

    bool dirty() const { return _dirty; }
    void clearDirty() { _dirty = false; }

    PendingPairingList<>& pending() { return _pending; }
    const PendingPairingList<>& pending() const { return _pending; }

private:
    std::array<PairedEntry, kMaxPaired> _entries{};
    PendingPairingList<> _pending{};
    std::span<const char> _pin{};
    uint32_t _windowOpenedMs = 0;
    uint32_t _presenceOpenedMs = 0;
    uint32_t _wallSeconds = 0;
    uint16_t _generation = 1;
    AccessLevel _presenceDefaultRole = AccessLevel::control;
    AccessLevel _autoKeepMax = AccessLevel::watch;
    bool _windowOpen = false;
    bool _presenceOpen = false;
    bool _knockApproveEnabled = true;
    bool _dirty = false;
    uint8_t _failCount = 0;

    void touch() {
        ++_generation;
        _dirty = true;
    }
};

// ---- PairingManager ---------------------------------------------------------
// method bodies. Defined inline here (no companion _impl file exists for this header, unlike Hub/Client).

inline PairingManager::PairedEntry* PairingManager::findByInstance(std::span<const std::byte> instance_id) {
    if (instance_id.size() != limits::instance_id_bytes) return nullptr;
    for (auto& e : _entries) {
        if (e.used && std::equal(e.instance_id.begin(), e.instance_id.end(), instance_id.begin())) return &e;
    }
    return nullptr;
}

inline const PairingManager::PairedEntry* PairingManager::findByInstance(
    std::span<const std::byte> instance_id) const {
    return const_cast<PairingManager*>(this)->findByInstance(instance_id);
}

// ---- windows ----------------------------------------------------------------

inline void PairingManager::openWindow(std::span<const char> pinAscii, uint32_t nowMs) {
    _pin = pinAscii;
    _windowOpenedMs = nowMs;
    _windowOpen = true;
    _failCount = 0;
}

inline void PairingManager::closeWindow() { _windowOpen = false; }

inline bool PairingManager::windowOpen(uint32_t nowMs) const {
    if (!_windowOpen) return false;
    // §12.2: "opens a pairing_window_default_s (120 s) window" — auto-expiry,
    // wrap-safe per §7.2 (util/serial_arithmetic.hpp's timeReached).
    return !timeReached(nowMs, _windowOpenedMs + limits::pairing_window_default_s * 1000u);
}

inline void PairingManager::openPresenceWindow(uint32_t nowMs) {
    _presenceOpenedMs = nowMs;
    _presenceOpen = true;
}

inline void PairingManager::closePresenceWindow() { _presenceOpen = false; }

inline bool PairingManager::presenceWindowOpen(uint32_t nowMs) const {
    if (!_presenceOpen) return false;
    // Same duration constant as the PIN window on purpose: RFC-027 calls this
    // "a short single-grant window" and 120 s is short. One named duration
    // beats two that would drift.
    return !timeReached(nowMs, _presenceOpenedMs + limits::pairing_window_default_s * 1000u);
}

inline bool PairingManager::hasConfigureToken() const {
    for (const auto& e : _entries) {
        if (e.used && e.role == AccessLevel::configure) return true;
    }
    return false;
}

inline AccessLevel PairingManager::presenceGrantRole() const {
    return hasConfigureToken() ? _presenceDefaultRole : AccessLevel::configure;
}

inline uint8_t PairingManager::offeredModes(uint32_t nowMs) const {
    uint8_t m = 0;
    if (_knockApproveEnabled) m |= pairing_modes::knock_approve;
    if (windowOpen(nowMs)) m |= pairing_modes::pin_proof;
    if (presenceWindowOpen(nowMs)) m |= pairing_modes::push_to_pair;
    return m;
}

// ---- granting ---------------------------------------------------------------

inline PairingManager::PairedEntry* PairingManager::grant(std::span<const std::byte> instance_id, AccessLevel role,
                                                          uint8_t pairingMode, IRandom& rng,
                                                          std::span<std::byte> tokenOut) {
    if (instance_id.size() != limits::instance_id_bytes) return nullptr;
    PairedEntry* e = findByInstance(instance_id);
    if (e == nullptr) {
        for (auto& cand : _entries) {
            if (!cand.used) { e = &cand; break; }
        }
        if (e == nullptr) return nullptr;  // ledger exhausted: nothing sane to grant
        *e = PairedEntry{};
        std::memcpy(e->instance_id.data(), instance_id.data(), e->instance_id.size());
        e->firstSeen = _wallSeconds;
    }
    e->used = true;
    rng.fill(std::span<std::byte>(e->token));
    e->role = role;
    e->pairingMode = pairingMode;
    e->state = trust_states::trusted;  // an approval IS the answer to a tripwire
    e->lastSeen = _wallSeconds;
    if (tokenOut.size() >= e->token.size()) {
        std::memcpy(tokenOut.data(), e->token.data(), e->token.size());
    }
    touch();
    return e;
}

inline PairingManager::PairOutcome PairingManager::handlePairReq(std::span<const std::byte> instance_id,
                                                                 std::span<const std::byte> pin_proof,
                                                                 std::span<const std::byte> nonce,
                                                                 IRandom& rng, uint32_t nowMs,
                                                                 AccessLevel grantRole,
                                                                 std::span<std::byte> tokenOut,
                                                                 ICrypto& crypto) {
    if (!windowOpen(nowMs)) return PairOutcome::WindowClosed;
    if (instance_id.size() != limits::instance_id_bytes) return PairOutcome::Denied;

    auto expected = pairingPinProof(_pin, nonce);
    // RFC-028.3: token/proof checks are constant-time, protocol-wide. This was
    // a std::equal before M4b, which leaks the matching prefix length of a
    // secret through timing.
    const bool match = crypto.constantTimeEqual(pin_proof, std::span<const std::byte>(expected));

    if (!match) {
        ++_failCount;
        if (_failCount >= 3) _windowOpen = false;  // §12.2: "three failures close the window"
        return PairOutcome::Denied;
    }

    PairedEntry* e = grant(instance_id, grantRole, pairing_modes::pin_proof, rng, tokenOut);
    if (e == nullptr) return PairOutcome::Denied;

    _failCount = 0;
    return PairOutcome::Granted;
}

// ---- validation + tripwire --------------------------------------------------

inline AccessLevel PairingManager::validate(std::span<const std::byte> instance_id,
                                            std::span<const std::byte> token, ICrypto& crypto) const {
    if (token.size() != limits::token_bytes) return AccessLevel::watch;
    const PairedEntry* e = findByInstance(instance_id);
    if (!e) return AccessLevel::watch;
    if (!crypto.constantTimeEqual(std::span<const std::byte>(e->token), token)) return AccessLevel::watch;
    // A suspended (RECOGNIZED-PENDING) device is admitted, but only to look.
    if (e->state == trust_states::recognized_pending) return AccessLevel::watch;
    return e->role;
}

inline AccessLevel PairingManager::validateProof(std::span<const std::byte> instance_id,
                                                 std::span<const std::byte> proof,
                                                 std::span<const std::byte> nonce, ICrypto& crypto,
                                                 bool* proofMatched) const {
    if (proofMatched) *proofMatched = false;
    // The proof is fixed at 16 bytes by RFC-029 item 6 (same truncation the PIN
    // proof uses); the nonce is the 8-byte WELCOME nonce. Both are structural,
    // so a wrong size is a refusal and not something to pad or truncate into
    // shape.
    if (proof.size() != kTrustTokenProofBytes) return AccessLevel::watch;
    if (nonce.size() != 8) return AccessLevel::watch;
    const PairedEntry* e = findByInstance(instance_id);
    if (!e) return AccessLevel::watch;

    std::array<std::byte, 32> mac{};
    if (!crypto.hmacSha256(std::span<const std::byte>(e->token), nonce, std::span<std::byte>(mac))) {
        return AccessLevel::watch;
    }
    if (!crypto.constantTimeEqual(std::span<const std::byte>(mac.data(), kTrustTokenProofBytes), proof)) {
        return AccessLevel::watch;
    }
    if (proofMatched) *proofMatched = true;
    // Identical to validate()'s tail: a suspended (RECOGNIZED-PENDING) device
    // is admitted, but only to look. Proving you hold the token answers "are
    // you who you say" — it does not answer "should the version change you just
    // reported be trusted", which is a person's decision.
    if (e->state == trust_states::recognized_pending) return AccessLevel::watch;
    return e->role;
}

inline PairingManager::HelloObservation PairingManager::observeHello(std::span<const std::byte> instance_id,
                                                                     std::string_view client_kind,
                                                                     std::string_view client_name,
                                                                     std::string_view client_ver,
                                                                     bool hasClientVer,
                                                                     uint8_t presentationMode) {
    HelloObservation out{};
    PairedEntry* e = findByInstance(instance_id);
    if (e == nullptr) return out;
    out.known = true;
    out.suspendedRole = e->role;

    // Descriptive fields are refreshed unconditionally — they are labels, not
    // credentials, and a roster showing a stale name helps nobody.
    e->kind.assign(client_kind);
    e->name.assign(client_name);
    e->presentationMode = presentationMode;
    e->lastSeen = _wallSeconds;

    // THE TRIPWIRE. A device that reports NO version can never trip it — stated
    // plainly because that is a real gap, not an oversight: there is nothing to
    // compare, and refusing to admit version-less clients would exile every
    // coin-cell potato the whole tier system exists to include.
    const bool alreadySuspended = (e->state == trust_states::recognized_pending);
    if (hasClientVer && !e->version.sameAs(client_ver) && !e->version.empty() && !alreadySuspended) {
        out.versionChanged = true;
        if (uint8_t(e->role) <= uint8_t(_autoKeepMax)) {
            // Policy: at or below the auto-keep ceiling, re-keep silently. The
            // default ceiling is `watch`, so a viewer that updated does not
            // nag an operator about a capability it never had.
            e->version.assign(client_ver);
            touch();
        } else {
            e->state = trust_states::recognized_pending;
            out.suspended = true;
            touch();
        }
    } else if (hasClientVer && e->version.empty()) {
        // First version ever seen for this device: RECORD it, do not trip. The
        // wire needs a baseline before it can be crossed.
        e->version.assign(client_ver);
        touch();
    } else if (!alreadySuspended) {
        _dirty = true;  // last_seen moved; roster generation did not
    }

    out.effectiveRole = (e->state == trust_states::recognized_pending) ? AccessLevel::watch : e->role;
    return out;
}

// ---- ledger management ------------------------------------------------------

inline bool PairingManager::revoke(std::span<const std::byte> instance_id) {
    PairedEntry* e = findByInstance(instance_id);
    if (!e) return false;
    *e = PairedEntry{};
    touch();
    return true;
}

inline size_t PairingManager::entryCount() const {
    size_t n = 0;
    for (const auto& e : _entries) {
        if (e.used) ++n;
    }
    return n;
}

inline const PairingManager::PairedEntry* PairingManager::entry(size_t i) const {
    size_t n = 0;
    for (const auto& e : _entries) {
        if (e.used) {
            if (n == i) return &e;
            ++n;
        }
    }
    return nullptr;
}

inline const PairingManager::PairedEntry* PairingManager::slot(size_t i) const {
    if (i >= kMaxPaired) return nullptr;
    return _entries[i].used ? &_entries[i] : nullptr;
}

inline bool PairingManager::importEntry(const PairedEntry& e) {
    for (auto& s : _entries) {
        if (!s.used) {
            s = e;
            s.used = true;
            touch();
            return true;
        }
    }
    return false;
}

// ---- CBOR (registry `trust_ledger_keys`) ------------------------------------

inline size_t PairingManager::encodeEntry(const PairedEntry& e, std::span<std::byte> out) const {
    // NOTE what is ABSENT: the TOKEN. The ledger item is read by any
    // `configure` session over BLOB_REQ, and shipping a live credential to a
    // reader — even an authorized one — turns "list its own paired devices"
    // into "hand out everyone's keys". A configure session can REVOKE any entry; it
    // has no business impersonating one.
    CborWriter w(out);
    uint32_t n = 5;  // instance_id, role, state, presentation_mode, pairing_mode
    if (!e.kind.empty()) ++n;
    if (!e.name.empty()) ++n;
    if (!e.version.empty()) ++n;
    if (e.firstSeen) ++n;
    if (e.lastSeen) ++n;
    w.mapHeader(n);
    w.key(uint64_t(trust_ledger::instance_id)).bstrVal(std::span<const std::byte>(e.instance_id));
    if (!e.kind.empty()) w.key(uint64_t(trust_ledger::kind)).tstrVal(e.kind.view());
    if (!e.name.empty()) w.key(uint64_t(trust_ledger::name)).tstrVal(e.name.view());
    if (!e.version.empty()) w.key(uint64_t(trust_ledger::version)).tstrVal(e.version.view());
    if (e.firstSeen) w.key(uint64_t(trust_ledger::first_seen)).uintVal(e.firstSeen);
    if (e.lastSeen) w.key(uint64_t(trust_ledger::last_seen)).uintVal(e.lastSeen);
    w.key(uint64_t(trust_ledger::role)).uintVal(uint8_t(e.role));
    w.key(uint64_t(trust_ledger::state)).uintVal(e.state);
    w.key(uint64_t(trust_ledger::presentation_mode)).uintVal(e.presentationMode);
    w.key(uint64_t(trust_ledger::pairing_mode)).uintVal(e.pairingMode);
    return w.size();
}

inline size_t PairingManager::encodeLedger(std::span<std::byte> out) const {
    // The PERSISTENCE form, unlike encodeEntry(): this one DOES carry tokens,
    // because it never leaves the device — it is the NVS blob. Never send it on
    // the wire.
    CborWriter w(out);
    w.arrayHeader(uint32_t(entryCount()));
    for (const auto& e : _entries) {
        if (!e.used) continue;
        uint32_t n = 6;
        if (!e.kind.empty()) ++n;
        if (!e.name.empty()) ++n;
        if (!e.version.empty()) ++n;
        if (e.firstSeen) ++n;
        if (e.lastSeen) ++n;
        w.mapHeader(n);
        w.key(uint64_t(trust_ledger::instance_id)).bstrVal(std::span<const std::byte>(e.instance_id));
        if (!e.kind.empty()) w.key(uint64_t(trust_ledger::kind)).tstrVal(e.kind.view());
        if (!e.name.empty()) w.key(uint64_t(trust_ledger::name)).tstrVal(e.name.view());
        if (!e.version.empty()) w.key(uint64_t(trust_ledger::version)).tstrVal(e.version.view());
        if (e.firstSeen) w.key(uint64_t(trust_ledger::first_seen)).uintVal(e.firstSeen);
        if (e.lastSeen) w.key(uint64_t(trust_ledger::last_seen)).uintVal(e.lastSeen);
        w.key(uint64_t(trust_ledger::role)).uintVal(uint8_t(e.role));
        w.key(uint64_t(trust_ledger::state)).uintVal(e.state);
        w.key(uint64_t(trust_ledger::presentation_mode)).uintVal(e.presentationMode);
        w.key(uint64_t(trust_ledger::pairing_mode)).uintVal(e.pairingMode);
        // Key 11 is NOT in the wire registry: the token is a persistence-only
        // field, deliberately numbered outside `trust_ledger_keys` so that a
        // decoder pointed at wire bytes can never mistake one for the other,
        // and so §4.3's skip-unknown rule silently drops it if it ever leaked
        // into a wire path.
        w.key(uint64_t(11)).bstrVal(std::span<const std::byte>(e.token));
    }
    return w.size();
}

inline bool PairingManager::decodeLedger(std::span<const std::byte> in) {
    CborReader r(in);
    auto nR = r.readArrayHeader();
    if (!nR) return false;
    if (nR.value() > kMaxPaired) return false;  // RFC-028.2: length fields are never trusted

    std::array<PairedEntry, kMaxPaired> staged{};
    for (uint32_t i = 0; i < nR.value(); ++i) {
        auto mR = r.readMapHeader();
        if (!mR) return false;
        PairedEntry e{};
        bool gotInstance = false;
        for (uint32_t f = 0; f < mR.value(); ++f) {
            auto kR = r.readKey();
            if (!kR) return false;
            switch (kR.value()) {
                case trust_ledger::instance_id: {
                    auto v = r.readBstr();
                    if (!v || v.value().size() != e.instance_id.size()) return false;
                    std::memcpy(e.instance_id.data(), v.value().data(), e.instance_id.size());
                    gotInstance = true;
                    break;
                }
                case trust_ledger::kind: {
                    auto v = r.readTstr();
                    if (!v || v.value().size() > kLedgerKindMaxBytes) return false;
                    e.kind.assign(v.value());
                    break;
                }
                case trust_ledger::name: {
                    auto v = r.readTstr();
                    if (!v || v.value().size() > kLedgerNameMaxBytes) return false;
                    e.name.assign(v.value());
                    break;
                }
                case trust_ledger::version: {
                    auto v = r.readTstr();
                    if (!v || v.value().size() > kLedgerVersionMaxBytes) return false;
                    e.version.assign(v.value());
                    break;
                }
                case trust_ledger::first_seen: {
                    auto v = r.readUint();
                    if (!v) return false;
                    e.firstSeen = uint32_t(v.value());
                    break;
                }
                case trust_ledger::last_seen: {
                    auto v = r.readUint();
                    if (!v) return false;
                    e.lastSeen = uint32_t(v.value());
                    break;
                }
                case trust_ledger::role: {
                    auto v = r.readUint();
                    if (!v || v.value() > uint64_t(AccessLevel::configure)) return false;
                    e.role = AccessLevel(uint8_t(v.value()));
                    break;
                }
                case trust_ledger::state: {
                    auto v = r.readUint();
                    if (!v || v.value() > trust_states::recognized_pending) return false;
                    e.state = uint8_t(v.value());
                    break;
                }
                case trust_ledger::presentation_mode: {
                    auto v = r.readUint();
                    if (!v || v.value() > 0xFF) return false;
                    e.presentationMode = uint8_t(v.value());
                    break;
                }
                case trust_ledger::pairing_mode: {
                    auto v = r.readUint();
                    if (!v || v.value() > 0xFF) return false;
                    e.pairingMode = uint8_t(v.value());
                    break;
                }
                case 11: {  // persistence-only token, see encodeLedger
                    auto v = r.readBstr();
                    if (!v || v.value().size() != e.token.size()) return false;
                    std::memcpy(e.token.data(), v.value().data(), e.token.size());
                    break;
                }
                default: {
                    auto sv = r.skipValue();  // §4.3
                    if (!sv) return false;
                    break;
                }
            }
        }
        if (!gotInstance) return false;
        e.used = true;
        staged[i] = e;
    }

    // ALL-OR-NOTHING. A half-applied ledger is an authorization decision nobody
    // made: staging first means a truncated or corrupt NVS blob leaves the hub
    // with the ledger it already had (or an empty one at boot), never with an
    // arbitrary prefix of someone else's trust list.
    _entries = staged;
    touch();
    return true;
}

}  // namespace slopsync
