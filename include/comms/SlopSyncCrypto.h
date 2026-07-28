#pragma once

// ============================================================================
// SlopSyncCrypto — the firmware's slopsync::ICrypto, RFC-029 item 1 / M4c.
//
// WHAT THIS ADDS OVER THE LIBRARY DEFAULT: nothing but P-256. HMAC-SHA256 and
// the constant-time compare are inherited verbatim from slopsync::SoftwareCrypto
// — they are already test-vectored, they are pure functions, and reimplementing
// them over mbedtls would be new risk for no measurable gain (both run on
// handshake-rate inputs, not on a hot path). crypto.hpp says this explicitly:
// "a host substitutes an accelerated one, it never has to reimplement HMAC".
//
// SIGNATURE FORMAT — this class's contract, which crypto.hpp deliberately left
// to the implementation: RAW r||s, 64 bytes, both big-endian and zero-padded to
// exactly 32 bytes. NOT DER. Fixed-width is what lets a client parse without an
// ASN.1 reader, and 64 fits inside kTrustSigMaxBytes with room to spare.
// verifyP256() accepts the same 64-byte form and a SEC1 public key in either
// compressed (33 B) or uncompressed (65 B) encoding.
//
// DETERMINISTIC by construction (RFC 6979 — MBEDTLS_ECDSA_DETERMINISTIC is on
// in the pinned framework's sdkconfig, verified, not assumed). That matters
// here for a specific reason: an ECDSA nonce that repeats leaks the private
// key, and the alternative is trusting the RNG's quality at the exact moment a
// client happens to connect. RFC 6979 removes the question. The hardware TRNG
// is still used, but only for BLINDING and for the one-time keygen.
//
// COST, MEASURED-CLASS not guessed: the S3 has NO ECC accelerator (that
// peripheral is C3/C6/H2), so this is software ECP with hardware-bignum assist
// (CONFIG_MBEDTLS_HARDWARE_MPI=1). One sign is ~30-80 ms in ONE uninterruptible
// call; one keygen ~40-100 ms. Therefore signP256() MUST NOT be called from the
// hub task, an HTTP handler, or anything real-time — SlopSyncHubService runs it
// on a dedicated low-priority task and shuttles jobs/results through queues.
//
// KEY AT REST: the private scalar lives in NVS (namespace "slopsync", key
// "p256d", 32 raw bytes). NVS is plaintext on a device without flash
// encryption, so anyone with physical flash access can read it and clone this
// hub's identity. That is the accepted bound for v1 — the threat this closes is
// the REMOTE evil twin (a laptop on the LAN claiming to be SlopDrive), not an
// attacker holding the machine. Flash encryption is the belt-and-braces upgrade
// and needs no code change here.
// ============================================================================

#include <mbedtls/ecp.h>
#include <mbedtls/bignum.h>

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "slopsync/core/crypto.hpp"

struct SystemState;

namespace slopdrive {

class EspCrypto final : public slopsync::SoftwareCrypto {
public:
    EspCrypto();
    ~EspCrypto() override;

    EspCrypto(const EspCrypto&) = delete;
    EspCrypto& operator=(const EspCrypto&) = delete;

    // Load the persisted keypair, or generate and persist one on first boot.
    // BLOCKS for ~40-100 ms when it has to generate. Call it exactly once, from
    // the signing task's own stack — never from setup() (the Arduino loop task's
    // 8 KB is shared with everything else that ran before it) and never from a
    // real-time or network task.
    //
    // Returns true when a usable keypair is loaded. A false return is NOT fatal:
    // publicKey()/signP256() then answer 0, which crypto.hpp defines as the
    // normal, expected "this hub cannot sign" answer.
    bool begin(SystemState& state);

    // True once begin() succeeded. Readable from any task.
    bool ready() const { return _ready.load(std::memory_order_acquire); }

    // ---- slopsync::ICrypto — the P-256 half ---------------------------------
    // Signing is SINGLE-TASK by contract: it mutates member MPI scratch. Only
    // SlopSyncHubService's "SlopSyncSign" task may call it.
    size_t signP256(std::span<const std::byte> message, std::span<std::byte> sigOut) override;
    bool verifyP256(std::span<const std::byte> pubkey, std::span<const std::byte> message,
                    std::span<const std::byte> sig) override;
    size_t publicKey(std::span<std::byte> out) override;

    // Diagnostics: how long the last sign actually took, in µs. The whole
    // deferral design rests on this number being large; publishing it is how a
    // future part with an ECC accelerator gets to prove it no longer needs to.
    uint32_t lastSignUs() const { return _lastSignUs.load(std::memory_order_relaxed); }
    uint32_t keygenUs() const { return _keygenUs; }

private:
    bool loadOrGenerate(SystemState& state);
    bool derivePublic();

    mbedtls_ecp_group _grp{};
    mbedtls_mpi _d{};
    mbedtls_ecp_point _Q{};
    bool _ctxInit = false;

    std::array<std::byte, 33> _pubCompressed{};  // SEC1 compressed, the PAIR_GRANT form
    std::atomic<bool> _ready{false};
    std::atomic<uint32_t> _lastSignUs{0};
    uint32_t _keygenUs = 0;
};

}  // namespace slopdrive
