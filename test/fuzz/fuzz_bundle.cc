// fuzz_bundle — target: STREAM bundle validation (wire/stream_bundle.hpp) —
// §5.4's caps (n <= 32, span <= 20 ms, strictly-increasing t_off,
// size == n x S).
//
// Scope note found during this gate's first pass: of those four caps,
// BundleView::parse enforces n, span and size — NOT t_off monotonicity. It is
// still TOTAL (no input makes it read out of bounds, which is what this
// target proves); the monotonicity walk lives at the hub's ingress instead
// (hub_impl.hpp step 3b), and a CLIENT parsing an h2c bundle must do its own.
// stream_bundle.hpp's header now says so out loud.
//
// Bundles are the highest-rate untrusted input the device accepts: the
// inbound motion-input channel (0x0084) takes them at up to 333 Hz from any
// granted client. A BundleView that parses and then hands out an
// out-of-range sample() span writes straight into the motion path.
//
// The sample size S is catalog-declared, so it is fuzzer-chosen here (that is
// exactly the degree of freedom a hostile peer has: pick a channel whose S
// you like, then send bytes that disagree with it).
#include "fuzz_common.hpp"

using namespace slopsync;
using namespace slopfuzz;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 2) return 0;
    FuzzInput fi(data, size);
    // 0 is a legal-looking-but-degenerate S and is deliberately included.
    const size_t sampleSize = size_t(fi.u8()) % 33;
    auto in = fi.rest();

    auto r = BundleView::parse(in, sampleSize);
    if (!r) return 0;

    const BundleView& bv = r.value();
    const uint8_t n = bv.sampleCount();
    if (n > limits::bundle_max_samples) __builtin_trap();  // TOTALITY: cap escaped

    uint32_t prev = 0;
    for (uint8_t i = 0; i < n; ++i) {
        const uint32_t t = bv.sampleTimeUs(i);
        auto s = bv.sample(i);
        // TOTALITY: every sample span must lie inside the input we handed in.
        if (s.size() != sampleSize) __builtin_trap();
        if (sampleSize > 0) {
            if (s.data() < in.data() || s.data() + s.size() > in.data() + in.size()) __builtin_trap();
        }
        sink(touch(s));
        sink(t ^ prev);
        prev = t;
    }

    // Writer round-trip on the same input: re-emitting what parsed must stay
    // inside the destination buffer for every fuzzer-chosen S.
    {
        std::array<std::byte, 256> out{};
        BundleWriter w(std::span<std::byte>(out), bv.tBase(), sampleSize);
        for (uint8_t i = 0; i < n; ++i) w.addSample(uint16_t(bv.sampleTimeUs(i) - bv.tBase()), bv.sample(i));
        size_t sz = w.finalize();
        if (sz > out.size()) __builtin_trap();
        sink(touch(std::span<const std::byte>(out.data(), sz)));
    }
    return 0;
}
