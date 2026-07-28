// SlopSyncCrypto — P-256 ECDSA hub identity: NVS-backed keygen, sign, verify.
//
// Constraints:
// - NVS private-key write is skipped while OTA is active (a flash-cache
//   access during an OTA write window can reset the chip); the key
//   regenerates and persists on the next boot instead.
// - espRngCb (hardware TRNG) blinds scalar multiplication only — the ECDSA
//   nonce is RFC 6979 deterministic, so RNG quality here affects
//   side-channel hardening, not key secrecy.
// - verifyP256() returns false for both "invalid" and "unsupported"; a
//   caller that needs to tell them apart checks publicKey()/ready() first.

#include "SlopSyncCrypto.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_random.h>
#include <esp_timer.h>

#include <mbedtls/ecdsa.h>
#include <mbedtls/sha256.h>

#include <cstring>

#include "SystemState.h"
#include "sloplog/sloplog.h"

namespace slopdrive {

namespace {

constexpr const char* kNvsNamespace = "slopsync";
constexpr const char* kNvsKeyPriv = "p256d";
constexpr size_t kScalarBytes = 32;
constexpr size_t kRawSigBytes = 64;

// The blinding/keygen RNG. esp_fill_random is the IDF hardware TRNG and is
// properly seeded once the RF subsystem is running — SlopSync starts after WiFi
// in main.cpp's setup(), so by the time this can be called the entropy source is
// real. Using it directly avoids carrying an mbedtls_entropy + ctr_drbg pair
// (~2 KB of context) for a job the silicon already does.
//
// NOTE ON WHY RNG QUALITY IS NOT LOAD-BEARING FOR THE SIGNATURE ITSELF: the
// nonce comes from RFC 6979 (deterministic), so this RNG only randomizes the
// scalar-multiplication blinding. A weak RNG here costs side-channel hardening,
// not key secrecy.
int espRngCb(void* /*ctx*/, unsigned char* out, size_t len) {
    esp_fill_random(out, len);
    return 0;
}

}  // namespace

EspCrypto::EspCrypto() {
    mbedtls_ecp_group_init(&_grp);
    mbedtls_mpi_init(&_d);
    mbedtls_ecp_point_init(&_Q);
    _ctxInit = true;
}

EspCrypto::~EspCrypto() {
    if (!_ctxInit) return;
    mbedtls_ecp_point_free(&_Q);
    mbedtls_mpi_free(&_d);
    mbedtls_ecp_group_free(&_grp);
}

bool EspCrypto::begin(SystemState& state) {
    if (_ready.load(std::memory_order_acquire)) return true;
    if (mbedtls_ecp_group_load(&_grp, MBEDTLS_ECP_DP_SECP256R1) != 0) {
        SLOGE("slopsync", "P-256 group load failed — hub identity DISABLED this boot");
        return false;
    }
    if (!loadOrGenerate(state)) return false;
    if (!derivePublic()) return false;
    _ready.store(true, std::memory_order_release);
    return true;
}

bool EspCrypto::loadOrGenerate(SystemState& state) {
    uint8_t d[kScalarBytes] = {};

    {
        Preferences prefs;
        // Read-WRITE open, same reasoning as loadPairing(): a read-only begin()
        // on a namespace that has never been written fails NOT_FOUND and the
        // Preferences library logs a scary E-line operators read as a boot
        // failure.
        if (prefs.begin(kNvsNamespace, false)) {
            if (prefs.getBytesLength(kNvsKeyPriv) == kScalarBytes) {
                prefs.getBytes(kNvsKeyPriv, d, kScalarBytes);
                prefs.end();
                if (mbedtls_mpi_read_binary(&_d, d, kScalarBytes) == 0 &&
                    mbedtls_ecp_check_privkey(&_grp, &_d) == 0) {
                    SLOGI("slopsync", "P-256 hub identity loaded from NVS");
                    return true;
                }
                SLOGW("slopsync", "persisted P-256 key rejected — regenerating");
            } else {
                prefs.end();
            }
        }
    }

    // ---- First boot (or a corrupt key): generate one. ~40-100 ms. -----------
    const int64_t t0 = esp_timer_get_time();
    if (mbedtls_ecp_gen_privkey(&_grp, &_d, espRngCb, nullptr) != 0) {
        SLOGE("slopsync", "P-256 keygen FAILED — hub identity DISABLED this boot");
        return false;
    }
    _keygenUs = uint32_t(esp_timer_get_time() - t0);

    if (mbedtls_mpi_write_binary(&_d, d, kScalarBytes) != 0) return false;

    // Same OTA gate every NVS writer here honors: a flash-cache access inside an
    // OTA write window can reset the chip. Losing the write is harmless — the
    // key is regenerated (and re-persisted) on the next boot; a reset mid-flash
    // is not.
    if (!state.ota_active.load(std::memory_order_relaxed)) {
        Preferences prefs;
        if (prefs.begin(kNvsNamespace, false)) {
            prefs.putBytes(kNvsKeyPriv, d, kScalarBytes);
            prefs.end();
            SLOGI("slopsync", "P-256 hub identity GENERATED and persisted (%u us)", unsigned(_keygenUs));
        } else {
            SLOGW("slopsync", "P-256 identity generated but NVS unavailable — not durable");
        }
    } else {
        SLOGW("slopsync", "P-256 identity generated during OTA — persist deferred to next boot");
    }
    std::memset(d, 0, sizeof(d));
    return true;
}

bool EspCrypto::derivePublic() {
    // Q = d*G. One scalar multiply, same order of magnitude as a sign.
    if (mbedtls_ecp_mul(&_grp, &_Q, &_d, &_grp.G, espRngCb, nullptr) != 0) {
        SLOGE("slopsync", "P-256 public-point derivation failed");
        return false;
    }
    unsigned char buf[33] = {};
    size_t olen = 0;
    if (mbedtls_ecp_point_write_binary(&_grp, &_Q, MBEDTLS_ECP_PF_COMPRESSED, &olen, buf,
                                       sizeof(buf)) != 0 ||
        olen != _pubCompressed.size()) {
        SLOGE("slopsync", "P-256 pubkey export failed (olen=%u)", unsigned(olen));
        return false;
    }
    for (size_t i = 0; i < olen; ++i) _pubCompressed[i] = std::byte(buf[i]);
    SLOGI("slopsync", "hub pubkey %02x%02x%02x%02x… (SEC1 compressed, 33 B)", buf[0], buf[1], buf[2],
          buf[3]);
    return true;
}

size_t EspCrypto::signP256(std::span<const std::byte> message, std::span<std::byte> sigOut) {
    if (!ready() || sigOut.size() < kRawSigBytes) return 0;

    unsigned char hash[32] = {};
    if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(message.data()), message.size(), hash,
                       0) != 0) {
        return 0;
    }

    mbedtls_mpi r, s;
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    const int64_t t0 = esp_timer_get_time();
    // RFC 6979 deterministic ECDSA. The trailing RNG is BLINDING ONLY (see the
    // callback's note) — it is not the nonce source, and mbedtls requires it to
    // be non-NULL.
    const int rc = mbedtls_ecdsa_sign_det_ext(&_grp, &r, &s, &_d, hash, sizeof(hash),
                                              MBEDTLS_MD_SHA256, espRngCb, nullptr);
    size_t written = 0;
    if (rc == 0) {
        unsigned char raw[kRawSigBytes] = {};
        if (mbedtls_mpi_write_binary(&r, raw, 32) == 0 &&
            mbedtls_mpi_write_binary(&s, raw + 32, 32) == 0) {
            for (size_t i = 0; i < kRawSigBytes; ++i) sigOut[i] = std::byte(raw[i]);
            written = kRawSigBytes;
        }
    }
    _lastSignUs.store(uint32_t(esp_timer_get_time() - t0), std::memory_order_relaxed);
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    if (rc != 0) SLOGW("slopsync", "P-256 sign failed rc=%d", rc);
    return written;
}

bool EspCrypto::verifyP256(std::span<const std::byte> pubkey, std::span<const std::byte> message,
                           std::span<const std::byte> sig) {
    // "False means invalid OR unsupported" (crypto.hpp) — a caller that cares
    // about the difference checks publicKey()/ready() first.
    if (!_ctxInit || sig.size() != kRawSigBytes) return false;
    if (pubkey.size() != 33 && pubkey.size() != 65) return false;

    // The group is loaded by begin(); a hub that never generated a key can still
    // VERIFY, so load on demand rather than requiring ready().
    if (_grp.id != MBEDTLS_ECP_DP_SECP256R1 &&
        mbedtls_ecp_group_load(&_grp, MBEDTLS_ECP_DP_SECP256R1) != 0) {
        return false;
    }

    unsigned char hash[32] = {};
    if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(message.data()), message.size(), hash,
                       0) != 0) {
        return false;
    }

    mbedtls_ecp_point Q;
    mbedtls_mpi r, s;
    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    bool ok = false;
    if (mbedtls_ecp_point_read_binary(&_grp, &Q,
                                      reinterpret_cast<const unsigned char*>(pubkey.data()),
                                      pubkey.size()) == 0 &&
        mbedtls_ecp_check_pubkey(&_grp, &Q) == 0 &&
        mbedtls_mpi_read_binary(&r, reinterpret_cast<const unsigned char*>(sig.data()), 32) == 0 &&
        mbedtls_mpi_read_binary(&s, reinterpret_cast<const unsigned char*>(sig.data()) + 32, 32) ==
            0) {
        ok = (mbedtls_ecdsa_verify(&_grp, hash, sizeof(hash), &Q, &r, &s) == 0);
    }
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&Q);
    return ok;
}

size_t EspCrypto::publicKey(std::span<std::byte> out) {
    if (!ready() || out.size() < _pubCompressed.size()) return 0;
    for (size_t i = 0; i < _pubCompressed.size(); ++i) out[i] = _pubCompressed[i];
    return _pubCompressed.size();
}

}  // namespace slopdrive
