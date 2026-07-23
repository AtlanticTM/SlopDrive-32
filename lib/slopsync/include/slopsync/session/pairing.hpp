// slopsync-core — the §12.2 pairing ceremony + token store.
//
// Ceremony: app opens a window (PIN displayed on a trusted surface) →
// client sends PAIR_REQ{instance_id, pin_proof = HMAC-SHA256(PIN, its
// WELCOME nonce)[0..16]} → correct proof inside the window issues a random
// 16-byte token bound to the instance_id → PAIR_GRANT{token, roles}.
// Wrong proof or closed window → PAIRING_DENIED; three failures close the
// window. Tokens persist hub-side (the app is responsible for NVS-backing
// the store via exportEntry/importEntry) and are checked at every HELLO.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

#include "slopsync/core/rng.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/util/serial_arithmetic.hpp"
#include "slopsync/wire/hmac_sha256.hpp"

namespace slopsync {

class PairingManager {
public:
    static constexpr size_t kMaxPaired = 8;

    struct PairedEntry {
        std::array<std::byte, limits::instance_id_bytes> instance_id{};
        std::array<std::byte, limits::token_bytes> token{};
        AccessLevel role = AccessLevel::controller;
        bool used = false;
    };

    // ---- Window (§12.2) ----------------------------------------------------
    // `pinAscii` must stay valid while the window is open (points at app
    // memory, e.g. the digits also shown on the OLED/WebUI).
    void openWindow(std::span<const char> pinAscii, uint32_t nowMs);
    void closeWindow();
    bool windowOpen(uint32_t nowMs) const;   // auto-expires after limits::pairing_window_default_s

    // ---- PAIR_REQ handling -------------------------------------------------
    enum class PairOutcome : uint8_t { Granted, Denied, WindowClosed };
    // Verifies proof against (PIN, nonce). On success: creates/replaces the
    // instance's entry with a fresh rng token at `grantRole`, returns Granted
    // and fills tokenOut. Three consecutive failures close the window (§12.2).
    PairOutcome handlePairReq(std::span<const std::byte> instance_id,
                              std::span<const std::byte> pin_proof,
                              std::span<const std::byte> nonce,
                              IRandom& rng, uint32_t nowMs,
                              AccessLevel grantRole,
                              std::span<std::byte> tokenOut);

    // ---- Token validation (HELLO path, §12.2) ------------------------------
    AccessLevel validate(std::span<const std::byte> instance_id,
                         std::span<const std::byte> token) const;  // viewer if no match

    // ---- Store management (revocation UI, NVS persistence adapters) --------
    bool revoke(std::span<const std::byte> instance_id);
    size_t entryCount() const;
    const PairedEntry* entry(size_t i) const;
    bool importEntry(const PairedEntry& e);  // app restores from NVS at boot

private:
    std::array<PairedEntry, kMaxPaired> _entries{};
    std::span<const char> _pin{};
    uint32_t _windowOpenedMs = 0;
    bool _windowOpen = false;
    uint8_t _failCount = 0;

    PairedEntry* findByInstance(std::span<const std::byte> instance_id);
    const PairedEntry* findByInstance(std::span<const std::byte> instance_id) const;
};

// ============================================================================
// PairingManager — method bodies (§12.2). Defined inline here (no companion
// _impl file exists for this header, unlike Hub/Client).
// ============================================================================

inline PairingManager::PairedEntry* PairingManager::findByInstance(std::span<const std::byte> instance_id) {
    if (instance_id.size() != limits::instance_id_bytes) return nullptr;
    for (auto& e : _entries) {
        if (e.used && std::equal(e.instance_id.begin(), e.instance_id.end(), instance_id.begin())) return &e;
    }
    return nullptr;
}

inline const PairingManager::PairedEntry* PairingManager::findByInstance(
    std::span<const std::byte> instance_id) const {
    if (instance_id.size() != limits::instance_id_bytes) return nullptr;
    for (const auto& e : _entries) {
        if (e.used && std::equal(e.instance_id.begin(), e.instance_id.end(), instance_id.begin())) return &e;
    }
    return nullptr;
}

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

inline PairingManager::PairOutcome PairingManager::handlePairReq(std::span<const std::byte> instance_id,
                                                                  std::span<const std::byte> pin_proof,
                                                                  std::span<const std::byte> nonce,
                                                                  IRandom& rng, uint32_t nowMs,
                                                                  AccessLevel grantRole,
                                                                  std::span<std::byte> tokenOut) {
    if (!windowOpen(nowMs)) return PairOutcome::WindowClosed;
    if (instance_id.size() != limits::instance_id_bytes) return PairOutcome::Denied;

    auto expected = pairingPinProof(_pin, nonce);
    bool match = pin_proof.size() == expected.size() && std::equal(pin_proof.begin(), pin_proof.end(), expected.begin());

    if (!match) {
        ++_failCount;
        if (_failCount >= 3) _windowOpen = false;  // §12.2: "three failures close the window"
        return PairOutcome::Denied;
    }

    // Success: create-or-replace this instance_id's entry (§12.2: "a random
    // 16-byte token bound to the client's instance_id, persisted on both
    // ends"). Re-pairing an already-paired instance simply reissues a fresh
    // token at the requested role.
    PairedEntry* slot = findByInstance(instance_id);
    if (!slot) {
        for (auto& e : _entries) {
            if (!e.used) { slot = &e; break; }
        }
    }
    if (!slot) return PairOutcome::Denied;  // store exhausted: nothing sane to grant

    slot->used = true;
    std::memcpy(slot->instance_id.data(), instance_id.data(), slot->instance_id.size());
    rng.fill(std::span<std::byte>(slot->token));
    slot->role = grantRole;

    if (tokenOut.size() >= slot->token.size()) {
        std::memcpy(tokenOut.data(), slot->token.data(), slot->token.size());
    }

    _failCount = 0;
    return PairOutcome::Granted;
}

inline AccessLevel PairingManager::validate(std::span<const std::byte> instance_id,
                                             std::span<const std::byte> token) const {
    if (token.size() != limits::token_bytes) return AccessLevel::viewer;
    const PairedEntry* e = findByInstance(instance_id);
    if (!e) return AccessLevel::viewer;
    if (!std::equal(e->token.begin(), e->token.end(), token.begin())) return AccessLevel::viewer;
    return e->role;
}

inline bool PairingManager::revoke(std::span<const std::byte> instance_id) {
    PairedEntry* e = findByInstance(instance_id);
    if (!e) return false;
    *e = PairedEntry{};
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

inline bool PairingManager::importEntry(const PairedEntry& e) {
    for (auto& slot : _entries) {
        if (!slot.used) {
            slot = e;
            slot.used = true;
            return true;
        }
    }
    return false;
}

}  // namespace slopsync
