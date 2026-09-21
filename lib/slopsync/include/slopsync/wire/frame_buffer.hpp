// slopsync-core — FrameBuffer: an owned, fixed-capacity, complete frame
// (header + payload) as handed across the ITransport boundary.
//
// Capacity rationale: 512 bytes. The wire format is designed so no frame ever
// NEEDS to be larger — data-plane frames fit the 242-byte ESP-NOW floor by
// rule (SPEC §9.1, §5.4), catalog chunks are 192+header, and oversized
// control frames fragment (§5.6). Hubs advertise `max_frame` ≤ this in
// WELCOME's limits. A fixed capacity keeps every queue slot heap-free on the
// ESP32 (SPEC §13.1's binding contract is span-in/FrameBuffer-out).
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/wire/frame_header.hpp"

namespace slopsync {

inline constexpr size_t kFrameBufferCapacity = 512;

class FrameBuffer {
public:
    FrameBuffer() = default;

    // Copy `frame` in; truncates nothing — oversized input yields empty().
    static FrameBuffer from(std::span<const std::byte> frame) {
        FrameBuffer b;
        if (frame.size() <= kFrameBufferCapacity) {
            std::memcpy(b._data.data(), frame.data(), frame.size());
            b._size = uint16_t(frame.size());
        }
        return b;
    }

    std::span<std::byte> writable() { return {_data.data(), kFrameBufferCapacity}; }
    void setSize(size_t n) { _size = uint16_t(n <= kFrameBufferCapacity ? n : 0); }

    std::span<const std::byte> bytes() const { return {_data.data(), _size}; }
    size_t size() const { return _size; }
    bool empty() const { return _size == 0; }

    // Convenience: header view (nullopt if too short — or if this is the
    // 12-byte ESTOP frame, which callers must magic-check FIRST, §5.5).
    std::optional<FrameHeader> header() const { return decodeFrameHeader(bytes()); }
    std::span<const std::byte> payload() const {
        return _size >= kHeaderBytes ? bytes().subspan(kHeaderBytes) : std::span<const std::byte>{};
    }

private:
    std::array<std::byte, kFrameBufferCapacity> _data{};
    uint16_t _size = 0;
};

}  // namespace slopsync
