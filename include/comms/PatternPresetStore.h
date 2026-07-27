#pragma once

// ============================================================================
// PatternPresetStore — RFC-021 `pattern.frayd` preset backend.
//
// Firmware-side, device-local (NOT part of lib/slopsync — RFC-021 deliberately
// leaves the STORE backend as "the application's job", see hub.hpp's readBlob
// seam doc). Retires the last HTTP writer, POST /api/pattern/presets.
//
// This class is PURE byte-blob CRUD and knows NOTHING about what a preset
// MEANS — same "opaque payload" philosophy the wire protocol itself uses
// (blob_keys key 7's note: "the hub validates kind + size on import... it
// never inspects the payload itself"). SlopDriveHubDelegate (SlopSyncHubService
// .cpp) is the ONLY place that knows the 40-byte payload is really four
// advpat base scalars plus six modifier blocks — encoding/decoding that
// meaning lives there, next to the PatternEngine it reads from and writes to.
//
// ---- Shape ------------------------------------------------------------------
// kCapacity (24) matches the retired HTTP handler's AP_PRESET_MAX_COUNT for
// parity. The roster STATE (0x0096) is BARE — {generation,count,capacity}
// only, same shape as the trust ledger's 0x000D — a client enumerates names
// via BLOB_REQ per slot (kPayloadBytes is tiny, 40 B, so 24 fetches is cheap).
// An embedded per-slot name preview was the original plan and was cut for a
// real, measured reason: Catalog32's layout-field pool (channel/catalog.hpp,
// capacity 200) had only 11 free slots left on this device, nowhere near the
// 17 a 3-field header + 14 str16 names would have needed. See the 0x0096
// entry's comment in SlopSyncCatalog.h.
// ============================================================================

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace slopdrive {

class PatternPresetStore {
public:
    static constexpr uint8_t kCapacity = 24;
    static constexpr uint8_t kNameMax = 32;      // on-flash + INTENT name cap
    static constexpr uint8_t kPayloadBytes = 40; // 4 base scalars + 6 mods x 6 fields, all 0..100 (u8)

    struct Slot {
        char name[kNameMax] = {};                // NUL-padded; name[0]==0 means unused
        uint8_t payload[kPayloadBytes] = {};

        bool used() const { return name[0] != '\0'; }
    };

    const Slot* slot(uint8_t i) const { return i < kCapacity ? &_slots[i] : nullptr; }
    uint16_t generation() const { return _generation; }
    bool dirty() const { return _dirty; }
    void clearDirty() { _dirty = false; }

    uint8_t count() const {
        uint8_t n = 0;
        for (const auto& s : _slots)
            if (s.used()) ++n;
        return n;
    }

    // Overwrites slot `i` with `name` + the given opaque payload bytes. `slot`
    // IS the address (the client picks it, normally the first free entry from
    // the roster) — there is no "store full" refusal the way the legacy
    // name-keyed handler had one, only an out-of-range slot or a bad name.
    bool save(uint8_t i, std::string_view name, const uint8_t (&payload)[kPayloadBytes]) {
        if (i >= kCapacity || name.empty() || name.size() >= kNameMax) return false;
        Slot& s = _slots[i];
        std::memset(s.name, 0, sizeof(s.name));
        std::memcpy(s.name, name.data(), name.size());
        std::memcpy(s.payload, payload, kPayloadBytes);
        ++_generation;
        _dirty = true;
        return true;
    }

    bool rename(uint8_t i, std::string_view name) {
        if (i >= kCapacity || name.empty() || name.size() >= kNameMax) return false;
        Slot& s = _slots[i];
        if (!s.used()) return false;
        std::memset(s.name, 0, sizeof(s.name));
        std::memcpy(s.name, name.data(), name.size());
        ++_generation;
        _dirty = true;
        return true;
    }

    bool remove(uint8_t i) {
        if (i >= kCapacity) return false;
        Slot& s = _slots[i];
        if (!s.used()) return false;
        s = Slot{};
        ++_generation;
        _dirty = true;
        return true;
    }

    // The raw opaque payload for BLOB_REQ export / the "load" op's decode.
    // nullptr for an out-of-range or empty slot.
    const uint8_t* payload(uint8_t i) const {
        if (i >= kCapacity) return nullptr;
        const Slot& s = _slots[i];
        return s.used() ? s.payload : nullptr;
    }

    // ---- THE NVS PERSISTENCE SEAM -------------------------------------------
    // One fixed-size blob, all-or-nothing — mirrors
    // PairingManager::encodeLedger/decodeLedger exactly, same reasoning: a
    // half-applied store is a set of presets nobody chose.
    static constexpr size_t kEncodedBytes = size_t(kCapacity) * (size_t(kNameMax) + size_t(kPayloadBytes));

    size_t encode(std::span<std::byte> out) const {
        if (out.size() < kEncodedBytes) return 0;
        size_t off = 0;
        for (const auto& s : _slots) {
            std::memcpy(out.data() + off, s.name, kNameMax);
            off += kNameMax;
            std::memcpy(out.data() + off, s.payload, kPayloadBytes);
            off += kPayloadBytes;
        }
        return kEncodedBytes;
    }

    bool decode(std::span<const std::byte> in) {
        if (in.size() != kEncodedBytes) return false;
        std::array<Slot, kCapacity> staged{};
        size_t off = 0;
        for (auto& s : staged) {
            std::memcpy(s.name, in.data() + off, kNameMax);
            s.name[kNameMax - 1] = '\0';  // guard a corrupt/truncated blob
            off += kNameMax;
            std::memcpy(s.payload, in.data() + off, kPayloadBytes);
            off += kPayloadBytes;
        }
        _slots = staged;
        return true;
    }

    // ---- Legacy migration seam ----------------------------------------------
    // Writes a slot directly without bumping generation/dirty per-call — the
    // caller (SlopSyncHubService::loadPresets, one-time at boot) migrates every
    // legacy entry it can and then calls markDirty() ONCE so the very first
    // NVS write is the migration's own write, not N spurious ones.
    bool importRaw(uint8_t i, std::string_view name, const uint8_t (&payload)[kPayloadBytes]) {
        if (i >= kCapacity || name.empty()) return false;
        Slot& s = _slots[i];
        std::memset(s.name, 0, sizeof(s.name));
        const size_t n = name.size() < size_t(kNameMax - 1) ? name.size() : size_t(kNameMax - 1);
        std::memcpy(s.name, name.data(), n);
        std::memcpy(s.payload, payload, kPayloadBytes);
        return true;
    }
    void markDirty() {
        ++_generation;
        _dirty = true;
    }

private:
    std::array<Slot, kCapacity> _slots{};
    uint16_t _generation = 1;
    bool _dirty = false;
};

}  // namespace slopdrive
