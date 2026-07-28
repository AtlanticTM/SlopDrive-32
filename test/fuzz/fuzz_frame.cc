// fuzz_frame — target: frame header + fragmentation reassembly
// (wire/frame_header.hpp, wire/fragmentation.hpp) — plus the ESTOP frame and
// the COBS serial framing that sit at the same layer.
//
// Length fields are the classic trust-the-attacker bug, and the Reassembler
// is where an attacker gets to choose an INDEX and a LENGTH that together
// pick a write offset. It is also STATEFUL, so the driver below replays a
// SEQUENCE of fragments from one input rather than a single frame: slot
// reuse, eviction, out-of-order arrival, the pending-last buffer and the
// timeout path are only reachable across calls.
#include "fuzz_common.hpp"

using namespace slopsync;
using namespace slopfuzz;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FuzzInput fi(data, size);

    // --- Stateless header/raw decodes on the whole input -------------------
    auto whole = asBytes(data, size);
    if (auto h = decodeFrameHeader(whole)) {
        sink(h->type);
        sink(h->len);
        sink(uint32_t(h->fragStart()) | uint32_t(h->fragMore()) << 1);
    }
    { auto e = decodeEstop(whole); if (e) sink(uint32_t(e.value().seq)); }
    { auto c = decodeClockRequest(whole); if (c) sink(c.value().t0); }
    { auto c = decodeClockReply(whole); if (c) sink(c.value().t2); }
    { auto b = decodeBeacon(whole); if (b) sink(b.value().boot_id); }
    { auto a = decodeAckMask(whole); if (a) sink(a.value().base_seq); }
    { auto p = decodeProbeFrame(whole); if (p) sink(p.value()); }

    // --- COBS: decode arbitrary bytes, then re-encode whatever came out ----
    {
        std::array<std::byte, 1024> out{};
        auto n = cobsDecode(whole, std::span<std::byte>(out));
        if (n) {
            if (n.value() > out.size()) __builtin_trap();
            sink(touch(std::span<const std::byte>(out.data(), n.value())));
        }
    }

    // --- Stateful reassembly ------------------------------------------------
    Reassembler ra;
    uint32_t nowMs = 0;
    while (fi.remaining() >= 4) {
        FrameHeader h{};
        h.type = fi.u8();
        h.flags = fi.u8();
        h.seq = fi.u16();
        h.channel = fi.u16();
        nowMs += fi.u16();  // adversarial (including wrapping) time steps

        // Fragment payload length chosen by the fuzzer, but clamped by the
        // harness to what is actually left — a harness that over-reads is
        // indistinguishable from a library finding.
        // Deliberately allowed FAR past kFrameBufferCapacity (512). A
        // transport is an interface anyone may implement and a WS binding
        // hands up whatever length the network sent; more practically, an
        // intra-OBJECT overflow (one array of a struct spilling into the
        // next) is invisible to ASan, so the harness has to let the write run
        // long enough to leave the whole Reassembler before the sanitizer can
        // witness it at all.
        size_t want = fi.u16();
        auto payload = fi.take(want % 4096);

        auto r = ra.accept(h, payload, nowMs);
        if (r && r.value().has_value()) {
            const FrameBuffer& fb = r.value().value();
            auto bytes = fb.bytes();
            if (bytes.size() > kFrameBufferCapacity) __builtin_trap();  // TOTALITY
            sink(touch(bytes));
            // A reassembled frame is fed straight back to the header decoder
            // by every real caller.
            if (auto rh = decodeFrameHeader(bytes)) sink(rh->len);
        }
        ra.expireStale(nowMs);
    }

    // --- Fragmenter, adversarial MTU ----------------------------------------
    // Not an untrusted-input surface per se, but it shares the length math
    // and a bad maxFrameBytes is reachable from a mis-negotiated binding.
    {
        FuzzInput f2(data, size);
        uint16_t mtu = f2.u16();
        auto body = f2.rest();
        // Skip the one input shape whose DOCUMENTED behavior is an assert()
        // (a 12-byte ESTOP frame with an MTU below 12): fragmentFrame's own
        // comment says that combination is unreachable in any conformant
        // configuration and the assert is there to say so out loud. Firing a
        // deliberate assert is not a finding.
        const bool estopShaped = body.size() == kEstopFrameBytes && body.size() > mtu &&
                                 body[0] == kEstopMagicByte && body[1] == kEstopMagicByte &&
                                 body[2] == kEstopMagicByte && body[3] == kEstopMagicByte;
        if (!estopShaped) fragmentFrame(body, mtu, [](std::span<const std::byte> frag) {
            if (frag.size() > kFrameBufferCapacity) __builtin_trap();
            sink(touch(frag));
        });
    }
    return 0;
}
