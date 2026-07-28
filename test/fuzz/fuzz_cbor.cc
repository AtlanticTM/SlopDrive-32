// fuzz_cbor — target: the deterministic-profile CBOR reader
// (wire/cbor/cbor_reader.hpp).
//
// This is the substrate every control-plane decoder sits on, so it gets its
// own target rather than being reached only incidentally through HELLO et al.
// Two drivers share the input:
//   mode 0 — generic value walk: skipValue() until the input is exhausted or
//            rejected. Exercises nesting, depth, containers and the
//            unknown-key skip path §4.3 depends on.
//   mode 1 — typed walk: a byte-driven sequence of typed reads, so the
//            string/bstr/float/int paths are reached with adversarial heads
//            (the classic "length field says 2^64-1" case lives here).
#include "fuzz_common.hpp"

using namespace slopsync;
using namespace slopfuzz;

static void walkGeneric(std::span<const std::byte> in) {
    CborReader r(in);
    // Bound the loop by input size: every successful skipValue consumes at
    // least one byte, so this terminates. The bound is a harness safety net,
    // not a claim about the reader.
    for (size_t i = 0; i <= in.size(); ++i) {
        size_t before = r.bytesConsumed();
        auto s = r.skipValue();
        if (!s) break;
        if (r.bytesConsumed() <= before) break;  // no progress: stop (never seen; belt+braces)
        if (r.bytesConsumed() > in.size()) __builtin_trap();  // TOTALITY: over-consumed
    }
}

static void walkTyped(std::span<const std::byte> in, FuzzInput& ops) {
    CborReader r(in);
    while (!ops.empty()) {
        switch (ops.u8() % 10) {
            case 0: { auto v = r.readMapHeader(); if (!v) return; break; }
            case 1: { auto v = r.readArrayHeader(); if (!v) return; break; }
            case 2: { auto v = r.readKey(); if (!v) return; break; }
            case 3: { auto v = r.readUint(); if (!v) return; break; }
            case 4: { auto v = r.readInt(); if (!v) return; break; }
            case 5: { auto v = r.readBool(); if (!v) return; break; }
            case 6: { auto v = r.readF32(); if (!v) return; break; }
            case 7: {
                auto v = r.readTstr();
                if (!v) return;
                // Reading the view is the point: a decoder that returned a
                // view past the end of `in` is only caught if we touch it.
                if (v.value().size() > in.size()) __builtin_trap();
                sink(touch(v.value()));
                break;
            }
            case 8: {
                auto v = r.readBstr();
                if (!v) return;
                if (v.value().size() > in.size()) __builtin_trap();
                sink(touch(v.value()));
                break;
            }
            default: { auto v = r.skipValue(); if (!v) return; break; }
        }
        if (r.bytesConsumed() > in.size()) __builtin_trap();  // TOTALITY
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    FuzzInput fi(data, size);
    const uint8_t mode = fi.u8();

    if ((mode & 1) == 0) {
        walkGeneric(asBytes(data + 1, size - 1));
    } else {
        // Split the remainder: first half is the CBOR bytes, second half
        // drives which typed read is attempted next.
        auto rest = fi.rest();
        size_t half = rest.size() / 2;
        FuzzInput ops(reinterpret_cast<const uint8_t*>(rest.data()) + half, rest.size() - half);
        walkTyped(rest.subspan(0, half), ops);
    }
    return 0;
}
