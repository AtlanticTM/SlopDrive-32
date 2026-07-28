// slopsync-core — injected CRYPTO (RFC-028 obligation 3), the third and last
// member of the injected-primitive family alongside IClock (core/clock.hpp)
// and IRandom (core/rng.hpp). Same rules, for the same reason: the library is
// zero-dependency and std-headers-only, so anything that would otherwise want
// mbedtls / WebCrypto / OpenSSL arrives through an interface the APPLICATION
// implements, and the in-library default is deterministic so native tests stay
// hardware-free (SPEC §17.2 — determinism is a conformance requirement).
//
// WHAT THIS IS NOT: it is not a place to put crypto POLICY. The hub decides
// WHAT to hash and WHAT to compare; this decides only HOW those primitives are
// computed, so a host with an accelerator can substitute one without forking
// protocol logic.
//
// -- hmacSha256 --------------------------------------------------------------
// The library ALREADY ships a self-contained, test-vectored HMAC-SHA256
// (wire/hmac_sha256.hpp, the §12.2 PIN-proof path) and the default
// implementation below simply calls it. The delegate exists so a host CAN
// substitute an accelerated one — NOT so every host must reimplement it. A hub
// that never overrides this loses nothing.
//
// -- constantTimeEqual -------------------------------------------------------
// REQUIRED for every token and proof comparison, protocol-wide (RFC-028.3).
// The OTA plane already did this; SlopSync's own token check did not, and
// `std::equal` over a 16-byte secret leaks its matching prefix length in
// timing. The default implementation is branch-free and length-safe; a host
// substituting one MUST keep it branch-free. Note the honest bound: on a
// cleartext LAN transport (§12.1's threat model) a remote timing attack across
// the network is not the realistic vector — the point is that a constant-time
// compare costs nothing and removes the class entirely, including for the
// in-process and serial bindings where an attacker CAN measure precisely.
//
// -- signP256 / verifyP256 / publicKey ---------------------------------------
// THE M4c SEAM, DECLARED ONLY. RFC-029 item 1's durable hub identity (P-256
// keypair at first boot, WELCOME signature over client_nonce || session_id ||
// boot_id, PAIR_GRANT delivering the pubkey) is a LATER milestone. These exist
// now so the shape is fixed and the Hub can carry the reference: the null
// object answers "unsupported" (false / 0), and every caller must treat that
// as a normal, expected answer rather than an error — a coin-cell hub will
// never sign anything.
//
// Placement rule inherited from the feasibility pass, recorded here because
// this is where an implementer will meet it: the S3 has NO ECC accelerator
// (that peripheral is C3/C6/H2). mbedtls software ECDSA is ~30-80 ms per sign
// in ONE uninterruptible call. An implementation of signP256() MUST NOT be
// invoked from an HTTP/WebSocket handler or any real-time task — which is
// exactly why the protocol makes signing ON REQUEST (`trust`.sig_request)
// rather than automatic.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "slopsync/wire/hmac_sha256.hpp"

namespace slopsync {

class ICrypto {
public:
    virtual ~ICrypto() = default;

    // HMAC-SHA256(key, message). Writes exactly 32 bytes into `out`; returns
    // false (and writes nothing) if `out` is smaller than 32.
    virtual bool hmacSha256(std::span<const std::byte> key, std::span<const std::byte> message,
                            std::span<std::byte> out) = 0;

    // Branch-free equality. Unequal LENGTHS return false immediately — a
    // length is not a secret, and comparing a 16-byte token against a 3-byte
    // one has nothing to hide. Equal lengths are compared in constant time
    // with respect to WHERE the first difference is, which is the part that
    // leaks.
    virtual bool constantTimeEqual(std::span<const std::byte> a, std::span<const std::byte> b) const = 0;

    // ---- M4c seam: P-256 (declared, not implemented) ------------------------
    // Deterministic ECDSA (RFC 6979) over SHA-256 of `message`, DER or raw r||s
    // per the implementation's own contract (M4c fixes it; nothing depends on
    // it yet). Returns bytes written, or 0 for "this hub cannot sign".
    virtual size_t signP256(std::span<const std::byte> message, std::span<std::byte> sigOut) {
        (void)message; (void)sigOut;
        return 0;
    }
    // Verify `sig` over `message` against a SEC1 public key. False means
    // "invalid OR unsupported" — a client that cares MUST first check
    // publicKey()/its own capability rather than reading a false as forgery.
    virtual bool verifyP256(std::span<const std::byte> pubkey, std::span<const std::byte> message,
                            std::span<const std::byte> sig) {
        (void)pubkey; (void)message; (void)sig;
        return false;
    }
    // This hub's SEC1-compressed P-256 public key (33 B) for PAIR_GRANT's
    // `trust`.hub_pubkey. Returns bytes written, 0 for "no durable identity".
    virtual size_t publicKey(std::span<std::byte> out) {
        (void)out;
        return 0;
    }
};

// ---------------------------------------------------------------------------
// The NULL OBJECT / default: pure software, deterministic, zero dependencies.
// HMAC and constant-time compare are fully implemented (they cost nothing and
// the protocol genuinely needs them); the P-256 half inherits the base class's
// honest "unsupported".
// ---------------------------------------------------------------------------
class SoftwareCrypto : public ICrypto {
public:
    bool hmacSha256(std::span<const std::byte> key, std::span<const std::byte> message,
                    std::span<std::byte> out) override {
        if (out.size() < 32) return false;
        auto mac = ::slopsync::hmacSha256(key, message);
        for (size_t i = 0; i < mac.size(); ++i) out[i] = mac[i];
        return true;
    }

    bool constantTimeEqual(std::span<const std::byte> a, std::span<const std::byte> b) const override {
        if (a.size() != b.size()) return false;
        // Accumulate every difference; never short-circuit. `volatile` on the
        // accumulator is what stops a compiler from noticing it may bail early
        // once diff is non-zero — without it the branch-free source can be
        // "optimized" back into a branch, which is the classic way a
        // constant-time compare stops being one.
        volatile uint8_t diff = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            diff = uint8_t(diff | (uint8_t(a[i]) ^ uint8_t(b[i])));
        }
        return diff == 0;
    }
};

// The process-wide default instance. A function-local static, so it exists
// only if something actually asks for it and there is no static-init order
// question — this is what lets Hub's new constructor parameter carry a
// reference default and keep every existing call site compiling untouched.
inline ICrypto& defaultCrypto() {
    static SoftwareCrypto c;
    return c;
}

// ---------------------------------------------------------------------------
// Test double, mirroring ManualClock / FixedSequenceRandom: real behavior for
// the primitives that must actually work, plus scriptable answers for the M4c
// half and call counters so a test can PROVE the hub routed a comparison
// through the injected delegate rather than reaching for std::equal.
// ---------------------------------------------------------------------------
class ScriptedCrypto final : public SoftwareCrypto {
public:
    mutable uint32_t compares = 0;
    uint32_t hmacs = 0;
    uint32_t signs = 0;

    // When set, sign/verify pretend a keypair exists (M4c rehearsal).
    bool p256Supported = false;
    std::array<std::byte, 33> pubkey{};

    bool hmacSha256(std::span<const std::byte> key, std::span<const std::byte> message,
                    std::span<std::byte> out) override {
        ++hmacs;
        return SoftwareCrypto::hmacSha256(key, message, out);
    }

    bool constantTimeEqual(std::span<const std::byte> a, std::span<const std::byte> b) const override {
        ++compares;
        return SoftwareCrypto::constantTimeEqual(a, b);
    }

    size_t signP256(std::span<const std::byte> message, std::span<std::byte> sigOut) override {
        if (!p256Supported || sigOut.size() < 64) return 0;
        ++signs;
        // Deliberately NOT ECDSA: a deterministic stand-in that is a pure
        // function of the message, so an M4c-shaped test can assert "the hub
        // signed exactly these bytes" without dragging a curve into the
        // library. Real signing is M4c and belongs in the firmware adapter.
        std::array<std::byte, 32> mac{};
        SoftwareCrypto::hmacSha256(std::span<const std::byte>(pubkey), message, std::span<std::byte>(mac));
        for (size_t i = 0; i < 32; ++i) { sigOut[i] = mac[i]; sigOut[32 + i] = mac[31 - i]; }
        return 64;
    }

    bool verifyP256(std::span<const std::byte> pubkey_, std::span<const std::byte> message,
                    std::span<const std::byte> sig) override {
        if (!p256Supported || sig.size() != 64) return false;
        std::array<std::byte, 32> mac{};
        SoftwareCrypto::hmacSha256(pubkey_, message, std::span<std::byte>(mac));
        return constantTimeEqual(std::span<const std::byte>(mac), sig.subspan(0, 32));
    }

    size_t publicKey(std::span<std::byte> out) override {
        if (!p256Supported || out.size() < pubkey.size()) return 0;
        for (size_t i = 0; i < pubkey.size(); ++i) out[i] = pubkey[i];
        return pubkey.size();
    }
};

}  // namespace slopsync
