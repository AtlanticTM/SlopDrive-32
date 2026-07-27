// ============================================================================
// test_main.cpp — conformance tests for THE SHIPPED DEVICE CATALOG
// (include/comms/SlopSyncCatalog.h), as opposed to the frozen mini-catalog
// fixture every other suite exercises.
//
// Why this suite exists: every wire-visible promise this machine makes to a
// generic client lives in that one header, and until M4a nothing in the test
// tree ever built it. The library suites can be perfectly green while the
// machine advertises a catalog that contradicts the hub's own encoders — and
// the two failures that would cause are (a) a client that cannot decode the
// SAFETY channel, and (b) a client that greys the wrong controls on a
// safety-critical surface. Both are silent.
//
// The catalog is hardware-free by construction (field descriptors, no Arduino),
// so it builds natively with nothing but the library + `-Iinclude/comms`.
// ============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "SlopSyncCatalog.h"
#include "slopsync/channel/catalog.hpp"
#include "slopsync/conformance/catalog_check.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/hub/hub.hpp"
#include "slopsync/wire/catalog_codec.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <vector>

using namespace slopsync;

namespace {

// One shared build per test case; a Catalog32 is tens of KiB, so it is always
// an out-param and never a return value (the stack-bomb rule this codebase
// learned the hard way).
struct DeviceCatalog {
    Catalog32 c{};
    // The FULLY-FEATURED machine (current sensor + power monitor) is the
    // default fixture, because that is what the shipping rig is; the
    // feature-gating cases below build the other three combinations
    // explicitly.
    DeviceCatalog() { REQUIRE(slopdrive::buildSlopDriveCatalog(c, {true, true})); }
};

const SchemaField* schemaFieldByKey(const Catalog32& cat, const CatalogEntry& e, uint8_t key) {
    for (const SchemaField& f : cat.schemaFields(e)) {
        if (f.key == key) return &f;
    }
    return nullptr;
}

const LayoutField* layoutFieldByName(const Catalog32& cat, const CatalogEntry& e, std::string_view name) {
    for (const LayoutField& f : cat.layoutFields(e)) {
        if (f.name == name) return &f;
    }
    return nullptr;
}

// Every role string a field may carry must be one the REGISTRY defines, OR
// match the one OPEN convention the registry itself documents: RFC-019's
// `action.<name>` (registry.yaml field_roles preamble — "the dotted namespace
// is open, and unregistered roles are legal" for that specific prefix; the
// fixed vocabulary below is everything else). `role` is a free string on the
// wire, which is exactly why an unregistered value has to fail HERE: a client
// can only upgrade to a bespoke widget for a role it recognizes, so a typo'd
// role is silently identical to no role at all.
bool isRegisteredRole(std::string_view r) {
    using namespace slopsync::field_roles;
    static constexpr std::string_view kAll[] = {
        limit_user_speed, limit_user_accel, limit_input_speed, limit_input_accel,
        limit_input_jerk, window_min, window_max, telemetry_position, telemetry_velocity,
        telemetry_current, telemetry_power_bus, telemetry_temp, telemetry_uptime,
        identity_name, meta_enabled_mask, meta_reset_gen,
        pattern_running, pattern_select, pattern_speed, pattern_depth, pattern_stroke,
        pattern_sensation,
    };
    for (std::string_view k : kAll) {
        if (k == r) return true;
    }
    return r.substr(0, 7) == "action.";
}

}  // namespace

// ============================================================================
// The catalog builds, is self-consistent, and satisfies the conformance
// checker — the baseline every other case here assumes.
// ============================================================================
TEST_CASE("device catalog: builds, sorts ascending, and passes checkCatalog") {
    DeviceCatalog dc;
    CHECK(dc.c.ok());
    // 15 -> 21 at M4b: the trust administration surface (0x0009 session-admin,
    // 0x000A pending-pairing, 0x000B pairing-events, 0x000C paired-devices +
    // 0x000D its roster) plus 0x000E safety-events. This number moving means
    // the device's etag moved, which is expected and is exactly how a client
    // learns to re-fetch — it is NOT the frozen conformance mini-catalog, whose
    // 775 B / F4 A2 8F BB 58 CE D1 6A pins are deliberately untouched by all of
    // this (M4b adds no entry to that fixture).
    // 21 -> 25 at M5a: 0x0086 plan-strip, 0x0087 power (feature-gated, present
    // in this fixture), 0x0088 slopmotion-diag, 0x0089 motion-anomaly.
    // 25 -> 26 at M5b: 0x0008 log (RFC-017) — declaring it is what makes
    // Hub::publishLog() do anything at all on this device.
    // 26 -> 28 at M5b: the 0x008A machine-modes / 0x0104 modes-set settings
    // pair, which moved blend / stream-speed / overshoot-clamp off the legacy
    // HTTP plane and onto SlopSync. Same rule as every bump above — the etag
    // moved, and that is the client's resync trigger, not a break.
    // 28 -> 33 at M5c: the SlopMotion tuning surface came off HTTP —
    // 0x008B/0x008C/0x008D (three cards, one `tuning` category, so they render
    // as ONE tab per SPEC §8.8) plus their shared writer 0x0105, plus 0x0106
    // machine-admin for the non-motion device actions.
    // 33 -> 41: the advanced-pattern channel set. 0x008E pattern-advanced (the
    // 8 base controls) plus 0x008F..0x0094 (six per-base-control cyclic
    // Modifier cards, one channel apiece — see SlopSyncCatalog.h for why that
    // split is by subsystem and not by bit-packing), all sharing category
    // `user` with 0x0082, plus their shared writer 0x0107.
    // 41 -> 44 (M5): the RFC-021 `pattern.frayd` preset store — 0x0095 STORE
    // descriptor, 0x0096 its roster STATE, 0x0108 the save/load/delete/rename
    // writer. Retires POST /api/pattern/presets, the last HTTP writer.
    CHECK(dc.c.count == 44);
    // RFC-017: the log channel must carry a replay depth, or a client that
    // connects after a fault sees nothing of what happened.
    const slopsync::CatalogEntry* logE = dc.c.find(slopsync::channels::log);
    REQUIRE(logE != nullptr);
    CHECK(logE->cls == slopsync::ChannelClass::EVENT);
    CHECK(logE->access == slopsync::AccessLevel::watch);
    CHECK(logE->hasReplayDepth);
    CHECK(logE->replayDepth == uint16_t(slopsync::limits::log_replay_depth_default));

    // §8.3's etag is order-sensitive, so ascending id order is not a style
    // preference — two hubs with identical content in different order would
    // publish different etags and force pointless re-transfers.
    for (uint16_t i = 1; i < dc.c.count; ++i) {
        CAPTURE(i);
        CHECK(dc.c.entries[i - 1].id < dc.c.entries[i].id);
    }

    // 64 KiB scratch: the M5a annotation block took the encoded device catalog
    // past 10 KB, and an undersized buffer here fails as "encode returned 0",
    // which reads like a codec bug rather than a too-small array. Heap, not
    // stack — this is a host test, but the habit is the point.
    std::vector<std::byte> scratch(65536);
    auto report = conformance::checkCatalog(dc.c, std::span<std::byte>(scratch));
    CHECK(report.ok());
}

// ============================================================================
// ITEM 0 — stream_kind: the segment channel must be NON-DECIMABLE.
// ============================================================================
TEST_CASE("device catalog: 0x0085 is segment-class, 0x0084 is not") {
    DeviceCatalog dc;

    const CatalogEntry* seg = dc.c.find(slopdrive::ch::motion_segment);
    REQUIRE(seg != nullptr);
    CHECK(seg->streamKind == stream_kinds::segments);
    // The classification the shedding table actually consults. A dropped
    // POSITION SAMPLE is recoverable by interpolating its neighbours; a
    // dropped TIMED SEGMENT is a permanently lost motion command, because the
    // sample carries its own duration_ms and therefore commands a time extent
    // rather than reporting an instant.
    CHECK(dc.c.isSegmentClass(*seg));

    const CatalogEntry* pts = dc.c.find(slopdrive::ch::motion_input);
    REQUIRE(pts != nullptr);
    // Left at the DEFAULT deliberately, and asserted as such: absent-means-
    // samples is the rule, and this catalog demonstrates it rather than
    // marking it redundantly (an explicit 0 would also be omitted on the wire,
    // so marking it would prove nothing and cost a reader's attention).
    CHECK(pts->streamKind == stream_kinds::samples);
    CHECK_FALSE(dc.c.isSegmentClass(*pts));
}

// ============================================================================
// ITEM 4 — the safety snapshot's shape must match Hub::buildSafetyPayload().
// ============================================================================
TEST_CASE("device catalog: 0x0003 is the 9-byte safety snapshot, modes appended last") {
    DeviceCatalog dc;
    const CatalogEntry* e = dc.c.find(channels::safety);
    REQUIRE(e != nullptr);
    CHECK(e->cls == ChannelClass::STATE);
    CHECK(e->defaultPriority == Priority::critical);
    // Open to viewers: a watcher that cannot see the latch cannot honour it.
    CHECK(e->access == AccessLevel::watch);

    auto fields = dc.c.layoutFields(*e);
    REQUIRE(fields.size() == 5);
    CHECK(fields[0].name == "word");
    CHECK(fields[1].name == "cause");
    CHECK(fields[2].name == "owner_session");
    CHECK(fields[3].name == "estop_seq");
    // APPENDED LAST — that is what keeps bytes 0..7 at their old offsets and
    // makes an old client's prefix parse correct.
    CHECK(fields[4].name == "modes");
    CHECK(fields[4].type == PackedFieldType::bitfield8);

    // The number Hub::buildSafetyPayload() emits. If these ever disagree the
    // machine publishes a payload its own catalog cannot decode.
    CHECK(dc.c.layoutWireSize(*e) == 9);

    auto bits = dc.c.bitLabels(fields[4]);
    REQUIRE(bits.size() == 2);
    CHECK(bits[0] == "override");   // safety_mode_bits::OVERRIDE
    CHECK(bits[1] == "bypass");     // safety_mode_bits::BYPASS
}

// ============================================================================
// ITEM 2 — per-op access, expressed in the catalog so a GENERIC client can
// render 0x0005 correctly without hardcoding this device.
// ============================================================================
TEST_CASE("device catalog: 0x0005 declares the role exemption via option_access") {
    DeviceCatalog dc;
    const CatalogEntry* e = dc.c.find(channels::safety_intents);
    REQUIRE(e != nullptr);

    // The FLOOR is watch. This looks alarming and is the entire point: without
    // it, a role-exempt estop is unreachable, and "the person in the room
    // cannot stop the machine" is the failure mode.
    CHECK(e->access == AccessLevel::watch);

    const SchemaField* op = schemaFieldByKey(dc.c, *e, 1);
    REQUIRE(op != nullptr);
    CHECK(op->name == "op");
    REQUIRE(op->hasOptionAccess);

    auto labels = dc.c.optionLabels(*op);
    auto access = dc.c.optionAccess(*op);
    // Index alignment is LITERAL: element i describes WIRE VALUE i. safety_ops
    // starts at 1, so index 0 is an unnamed placeholder that is never an op.
    REQUIRE(labels.size() == 11);
    REQUIRE(access.size() == labels.size());
    // Index 0 is a non-op placeholder. It is NAMED (an empty option label is
    // undecodable by a conforming client) and gated at `control`, so the one
    // wire value with no registry meaning is never the cheapest to reach.
    CHECK(labels[0] == "reserved");
    CHECK(access[0] == AccessLevel::control);

    // Names come from the registry, so a client can show the same verb this
    // repo's generated constants use.
    CHECK(labels[safety_ops::estop_clear] == "estop_clear");
    CHECK(labels[safety_ops::stop] == "stop");
    CHECK(labels[safety_ops::hold] == "hold");
    CHECK(labels[safety_ops::pause] == "pause");
    CHECK(labels[safety_ops::resume] == "resume");
    CHECK(labels[safety_ops::estop] == "estop");
    CHECK(labels[safety_ops::override_on] == "override_on");
    CHECK(labels[safety_ops::override_off] == "override_off");
    CHECK(labels[safety_ops::bypass_on] == "bypass_on");
    CHECK(labels[safety_ops::bypass_off] == "bypass_off");

    // THE EXEMPTION, asserted op by op.
    CHECK(access[safety_ops::estop] == AccessLevel::watch);
    CHECK(access[safety_ops::stop] == AccessLevel::watch);

    const uint8_t needsControl[] = {safety_ops::estop_clear, safety_ops::hold,        safety_ops::pause,
                                    safety_ops::resume,      safety_ops::override_on, safety_ops::override_off,
                                    safety_ops::bypass_on,   safety_ops::bypass_off};
    for (uint8_t o : needsControl) {
        CAPTURE(int(o));
        CHECK(access[o] == AccessLevel::control);
    }

    // Rate-capped, so the exemption cannot become a loop-stop weapon (§9.3;
    // the hub enforces it, this only proves the catalog advertises a bound).
    CHECK(e->maxRateHz > 0.0f);
    CHECK(e->defaultPriority == Priority::critical);
}

// ============================================================================
// ITEM 5 — the bench home ops exist, are named, and are control-gated.
// ============================================================================
TEST_CASE("device catalog: 0x0103 home declares the bench ops with a stroke field") {
    DeviceCatalog dc;
    const CatalogEntry* e = dc.c.find(slopdrive::ch::home);
    REQUIRE(e != nullptr);
    CHECK(e->access == AccessLevel::control);

    const SchemaField* op = schemaFieldByKey(dc.c, *e, 1);
    REQUIRE(op != nullptr);
    auto labels = dc.c.optionLabels(*op);
    REQUIRE(labels.size() == 4);
    CHECK(labels[0] == "reserved");
    CHECK(labels[1] == "home");
    CHECK(labels[2] == "force_home");       // *** CLEARS THE E-STOP LATCH ***
    CHECK(labels[3] == "clear_override");

    // Every op on this channel is control-gated — force_home in particular,
    // since it both asserts an UNMEASURED stroke window and un-latches an
    // e-stop. There is no exemption here and there must never be one.
    auto access = dc.c.optionAccess(*op);
    REQUIRE(access.size() == labels.size());
    for (size_t i = 0; i < access.size(); ++i) {
        CAPTURE(i);
        CHECK(access[i] == AccessLevel::control);
    }

    const SchemaField* stroke = schemaFieldByKey(dc.c, *e, 2);
    REQUIRE(stroke != nullptr);
    CHECK(stroke->name == "stroke");
    CHECK(stroke->type == CborFieldType::f32_t);
    CHECK(stroke->unit == "mm");
    CHECK(stroke->hasMin);
    CHECK(stroke->hasMax);
    CHECK(stroke->min >= 1.0f);   // a zero/negative bench stroke is not a window
}

// ============================================================================
// The catalog round-trips through its own codec — the annotations added above
// are not just in-memory struct fields, they actually reach the wire and come
// back. Without this, a client would receive a catalog with no option_access
// and silently fall back to offering every op to everyone.
// ============================================================================
TEST_CASE("device catalog: encode -> decode preserves option_access and stream_kind") {
    DeviceCatalog dc;
    std::vector<std::byte> buf(65536);
    size_t n = encodeCatalog(dc.c, buf);
    REQUIRE(n > 0);

    Catalog32 back{};
    REQUIRE(decodeCatalog(std::span<const std::byte>(buf).first(n), back).isOk());
    REQUIRE(back.count == dc.c.count);

    const CatalogEntry* seg = back.find(slopdrive::ch::motion_segment);
    REQUIRE(seg != nullptr);
    CHECK(back.isSegmentClass(*seg));
    const CatalogEntry* pts = back.find(slopdrive::ch::motion_input);
    REQUIRE(pts != nullptr);
    CHECK_FALSE(back.isSegmentClass(*pts));

    const CatalogEntry* safetyIntents = back.find(channels::safety_intents);
    REQUIRE(safetyIntents != nullptr);
    CHECK(safetyIntents->access == AccessLevel::watch);
    const SchemaField* op = schemaFieldByKey(back, *safetyIntents, 1);
    REQUIRE(op != nullptr);
    REQUIRE(op->hasOptionAccess);
    auto access = back.optionAccess(*op);
    REQUIRE(access.size() == 11);
    CHECK(access[safety_ops::estop] == AccessLevel::watch);
    CHECK(access[safety_ops::stop] == AccessLevel::watch);
    CHECK(access[safety_ops::hold] == AccessLevel::control);

    const CatalogEntry* safety = back.find(channels::safety);
    REQUIRE(safety != nullptr);
    CHECK(back.layoutWireSize(*safety) == 9);

    // Deterministic: re-encoding the decoded copy must be byte-identical, or
    // the etag is not a function of catalog CONTENT (§8.3).
    std::vector<std::byte> buf2(65536);
    size_t n2 = encodeCatalog(back, buf2);
    REQUIRE(n2 == n);
    CHECK(std::equal(buf.begin(), buf.begin() + n, buf2.begin()));
}

// ============================================================================
// M5a — EVERY LAYOUT FITS THE 242 B STATE FLOOR.
//
// A STATE payload that does not fit limits::min_transport_payload unfragmented
// is a channel that simply does not work on the smallest conforming transport
// (§9.1). Asserted over EVERY layout entry rather than the ones this milestone
// touched, because the failure is silent on WS and only appears on ESP-NOW/BLE.
// ============================================================================
TEST_CASE("device catalog: every STATE/STREAM layout fits 242 B unfragmented") {
    DeviceCatalog dc;
    for (uint16_t i = 0; i < dc.c.count; ++i) {
        const CatalogEntry& e = dc.c.entries[i];
        if (!e.usesLayout()) continue;
        CAPTURE(e.id);
        CHECK(dc.c.layoutWireSize(e) <= limits::min_transport_payload);
    }
}

// ============================================================================
// M5a — the exact byte counts the firmware's publishTelemetry() writes.
//
// These are the numbers a `std::array<std::byte, N>` in SlopSyncHubService.cpp
// is sized with. If a layout grows here and the publisher does not, the machine
// publishes a payload SHORTER than its own catalog claims — which decodes as a
// truncated prefix and looks like plausible data, not like an error.
// ============================================================================
TEST_CASE("device catalog: published layout sizes match the firmware's encoders") {
    DeviceCatalog dc;
    struct { uint16_t id; size_t bytes; } expect[] = {
        {slopdrive::ch::motion,         9},   // + raw_10um (M5a)
        {slopdrive::ch::machine_config, 33},  // + enabled_mask (M5a)
        {slopdrive::ch::pattern_state,  19},  // + enabled_mask (M5a)
        {slopdrive::ch::odometer,       20},  // + energy_wh, session_ms (M5a)
        {slopdrive::ch::plan_strip,     18},
        {slopdrive::ch::power,           8},  // 6 without a power monitor
        {slopdrive::ch::motion_diag,    88},   // + anom_waveform_smoothed (slopmotion 0.8.0)
        {slopsync::channels::hub_status, 14}, // + log_dropped (M5b, RFC-017 §9.4)
    };
    for (auto& x : expect) {
        CAPTURE(x.id);
        const CatalogEntry* e = dc.c.find(x.id);
        REQUIRE(e != nullptr);
        CHECK(dc.c.layoutWireSize(*e) == x.bytes);
    }
}

// ============================================================================
// M5a — APPEND-ONLY EVOLUTION. The released prefix of every grown layout keeps
// its field ORDER, and therefore its byte offsets. This is the whole contract
// that lets an old client keep decoding after an etag bump, so it is asserted
// name-by-name rather than trusted to review.
// ============================================================================
TEST_CASE("device catalog: M5a growth is append-only on 0x0080/0x0081/0x0082/0x0083") {
    DeviceCatalog dc;

    auto names = [&](uint16_t id) {
        std::vector<std::string_view> v;
        const CatalogEntry* e = dc.c.find(id);
        REQUIRE(e != nullptr);
        for (const LayoutField& f : dc.c.layoutFields(*e)) v.push_back(f.name);
        return v;
    };

    CHECK(names(slopdrive::ch::motion) ==
          std::vector<std::string_view>{"pos_10um", "tgt_10um", "speed", "flags", "raw_10um"});
    CHECK(names(slopdrive::ch::machine_config) ==
          std::vector<std::string_view>{"window_min", "window_max", "user_speed", "user_accel",
                                        "input_speed", "input_accel", "max_rail", "input_jerk",
                                        "enabled_mask"});
    CHECK(names(slopdrive::ch::pattern_state) ==
          std::vector<std::string_view>{"running", "pattern", "speed", "depth", "stroke",
                                        "sensation", "enabled_mask"});
    CHECK(names(slopdrive::ch::odometer) ==
          std::vector<std::string_view>{"strokes", "distance_m", "peak_mm_s", "energy_wh",
                                        "session_ms"});
}

// ============================================================================
// RFC-009 — EVERY setting_key RESOLVES in its entry's settingChannel.
//
// The single most load-bearing invariant this milestone adds: a settings UI is
// built by reading a STATE field's `setting_key` and writing THAT key on the
// entry's `settingChannel`. A key that resolves to nothing produces a control
// that renders perfectly and drives nothing — the exact defect class CLAUDE.md
// calls a shipping-broken control.
// ============================================================================
TEST_CASE("device catalog: every setting_key resolves in its declared settingChannel") {
    DeviceCatalog dc;
    int annotated = 0;
    for (uint16_t i = 0; i < dc.c.count; ++i) {
        const CatalogEntry& e = dc.c.entries[i];
        if (!e.usesLayout()) continue;
        for (const LayoutField& f : dc.c.layoutFields(e)) {
            if (!f.hasSettingKey) continue;
            ++annotated;
            CAPTURE(e.id);
            CAPTURE(f.name);
            // A field carrying a write key on an entry that names no INTENT
            // channel is unwritable: the key has no address space.
            REQUIRE(e.hasSettingChannel);
            const CatalogEntry* intent = dc.c.find(e.settingChannel);
            REQUIRE(intent != nullptr);
            CHECK(intent->cls == ChannelClass::INTENT);
            CHECK(schemaFieldByKey(dc.c, *intent, f.settingKey) != nullptr);
        }
    }
    // 7 limits (0x0081) + 6 pattern controls (0x0082).
    // 13 -> 16 at M5b: blend_mode, stream_speed_mode and overshoot_clamp on
    // 0x008A. (A fourth, `transport`, was written and then RETIRED before it
    // shipped — SlopSync is the only way in now, so an input-source selector
    // had nothing left to select.)
    // 16 -> 36 at M5c: 20 SlopMotion live-tune knobs, every one of which used
    // to be reachable ONLY via POST /api/slopmotion. That endpoint is now 410
    // Gone — this count IS the "no privileged client" invariant in numeric form.
    // 36 -> 80 for the advanced-pattern channel set: 8 base controls (0x008E)
    // + 36 modifier-cycle fields (6 controls × 6 sub-fields, 0x008F..0x0094),
    // all writable ONLY through 0x0107 now that /api/pattern is also 410 Gone.
    CHECK(annotated == 80);
}

// ============================================================================
// RFC-009 — every `role` on this machine is a REGISTERED role.
// ============================================================================
TEST_CASE("device catalog: every field role is a registered field_roles value") {
    DeviceCatalog dc;
    int roled = 0;
    for (uint16_t i = 0; i < dc.c.count; ++i) {
        const CatalogEntry& e = dc.c.entries[i];
        if (e.usesLayout()) {
            for (const LayoutField& f : dc.c.layoutFields(e)) {
                if (f.role.empty()) continue;
                ++roled;
                CAPTURE(e.id);
                CAPTURE(f.name);
                CAPTURE(f.role);
                CHECK(isRegisteredRole(f.role));
            }
        } else if (e.usesSchema()) {
            for (const SchemaField& f : dc.c.schemaFields(e)) {
                if (f.role.empty()) continue;
                ++roled;
                CAPTURE(e.id);
                CAPTURE(f.name);
                CHECK(isRegisteredRole(f.role));
            }
        }
    }
    CHECK(roled > 0);
}

// ============================================================================
// RFC-009 / RFC-006 — the roles the MFP plugin and the graphing CLI look
// fields up BY, asserted to exist exactly once each. That lookup is the whole
// point of the vocabulary: locate the limit by role, never by channel id, and
// the same code works against any conforming hub.
// ============================================================================
TEST_CASE("device catalog: the limit + window roles are discoverable and unique") {
    DeviceCatalog dc;
    using namespace slopsync::field_roles;
    const std::string_view wanted[] = {limit_user_speed,  limit_user_accel, limit_input_speed,
                                       limit_input_accel, limit_input_jerk, window_min, window_max};
    for (std::string_view want : wanted) {
        CAPTURE(want);
        int hits = 0;
        for (uint16_t i = 0; i < dc.c.count; ++i) {
            const CatalogEntry& e = dc.c.entries[i];
            if (!e.usesLayout()) continue;
            for (const LayoutField& f : dc.c.layoutFields(e)) {
                if (f.role == want) ++hits;
            }
        }
        CHECK(hits == 1);
    }
}

// ============================================================================
// RFC-003 — the STORED-vs-EFFECTIVE distinction, which IS the presence of
// setting_key and nothing else. `max_rail` is derived machine truth: it has no
// config-set key, so a client must render it read-only and must NEVER write it
// back into a setting's shadow. Adopting an EFFECTIVE value as stored config is
// the slopsync-js bug that produced RFC-003, so this is pinned in a test.
// ============================================================================
TEST_CASE("device catalog: max_rail is read-only, its siblings are settings") {
    DeviceCatalog dc;
    const CatalogEntry* e = dc.c.find(slopdrive::ch::machine_config);
    REQUIRE(e != nullptr);
    CHECK(e->hasSettingChannel);
    CHECK(e->settingChannel == slopdrive::ch::config_set);
    CHECK(e->hasCategory);
    CHECK(e->category == setting_categories::limits);

    const LayoutField* rail = layoutFieldByName(dc.c, *e, "max_rail");
    REQUIRE(rail != nullptr);
    CHECK_FALSE(rail->hasSettingKey);   // *** the distinction ***
    CHECK_FALSE(rail->desc.empty());    // still explained to the user
    CHECK(rail->unit == "mm");
    CHECK(rail->role.empty());

    // Every other numeric field here IS a setting, with a default and bounds.
    const char* settings[] = {"window_min", "window_max", "user_speed",
                              "user_accel", "input_speed", "input_accel", "input_jerk"};
    for (const char* nm : settings) {
        CAPTURE(nm);
        const LayoutField* f = layoutFieldByName(dc.c, *e, nm);
        REQUIRE(f != nullptr);
        CHECK(f->hasSettingKey);
        CHECK(f->dflt.has());
        CHECK(f->hasMin);
        CHECK(f->hasMax);
        CHECK(f->hasStep);
        CHECK_FALSE(f->group.empty());
        CHECK_FALSE(f->desc.empty());
        CHECK_FALSE(f->role.empty());
    }
}

// ============================================================================
// RFC-009 item 4 — enabled_mask exists on BOTH settings STATE channels, is
// role-tagged so a generic client recognizes it without knowing this device,
// and names the field each bit gates.
// ============================================================================
TEST_CASE("device catalog: both settings channels carry a role-tagged enabled_mask") {
    DeviceCatalog dc;

    struct { uint16_t id; size_t bits; } cases[] = {
        {slopdrive::ch::machine_config, 7},
        {slopdrive::ch::pattern_state,  6},
    };
    for (auto& c : cases) {
        CAPTURE(c.id);
        const CatalogEntry* e = dc.c.find(c.id);
        REQUIRE(e != nullptr);
        auto fields = dc.c.layoutFields(*e);
        REQUIRE(!fields.empty());
        // APPENDED LAST — that is what keeps every released offset intact.
        const LayoutField& mask = fields.back();
        CHECK(mask.name == "enabled_mask");
        CHECK(mask.type == PackedFieldType::bitfield8);
        CHECK(mask.role == slopsync::field_roles::meta_enabled_mask);
        CHECK_FALSE(mask.hasSettingKey);   // the mask is state, never a setting

        // Bit i gates the i-th SETTING-ANNOTATED field of the same layout, so
        // the labels must be exactly those fields' names, in order. This is the
        // check that catches "someone inserted a setting in the middle".
        auto bits = dc.c.bitLabels(mask);
        REQUIRE(bits.size() == c.bits);
        size_t n = 0;
        for (const LayoutField& f : fields) {
            if (!f.hasSettingKey) continue;
            REQUIRE(n < bits.size());
            CAPTURE(f.name);
            CHECK(bits[n] == f.name);
            ++n;
        }
        CHECK(n == c.bits);
    }
}

// ============================================================================
// RFC-009 gap 3 — the u8 single-select renders as NAMES. Without options a
// client can only show "3", and the user would have to own the firmware source
// to know what pattern 3 is.
// ============================================================================
TEST_CASE("device catalog: pattern is a named select matching PatternEngine") {
    DeviceCatalog dc;
    const CatalogEntry* e = dc.c.find(slopdrive::ch::pattern_state);
    REQUIRE(e != nullptr);
    CHECK(e->category == setting_categories::user);
    CHECK(e->settingChannel == slopdrive::ch::pattern_cmd);

    const LayoutField* p = layoutFieldByName(dc.c, *e, "pattern");
    REQUIRE(p != nullptr);
    auto opts = dc.c.optionLabels(*p);
    // PatternEngine::CORE_PATTERN_COUNT — the seven vendored patterns. The
    // build-flagged extended patterns are deliberately NOT advertised: this
    // header cannot see their compile flags, and offering a choice the running
    // firmware might clamp away is a lie in the forbidden direction.
    REQUIRE(opts.size() == 7);
    CHECK(opts[0] == "Simple Stroke");      // wire value 0
    CHECK(opts[1] == "Teasing Pounding");
    CHECK(opts[2] == "Robo Stroke");
    CHECK(opts[4] == "Deeper");
    CHECK(opts[5] == "Stop'n'Go");
    CHECK(opts[6] == "Insist");             // wire value 6
    // The declared max must be the last legal index, or a generic client
    // renders a choice the machine will clamp.
    CHECK(p->hasMax);
    CHECK(p->max == doctest::Approx(float(opts.size() - 1)));
}

// ============================================================================
// RFC-009 rendering checklist — the SETTINGS SURFACE IS COMPLETE.
//
// The question this whole milestone answers is "could a client that has never
// seen this device build a correct settings page from the catalog alone?".
// This is that question as an assertion: for every settings-bearing channel,
// EVERY writable field must carry enough to choose a widget, label it, explain
// it, bound it, and reset it. A single unannotated field is a control the
// client can only render as a bare number with no name.
// ============================================================================
TEST_CASE("device catalog: every setting is fully renderable from the catalog alone") {
    DeviceCatalog dc;
    for (uint16_t i = 0; i < dc.c.count; ++i) {
        const CatalogEntry& e = dc.c.entries[i];
        if (!e.usesLayout() || !e.hasSettingChannel) continue;
        // A settings channel must be filed under a tab.
        CAPTURE(e.id);
        CHECK(e.hasCategory);
        // Device-defined categories (>=128) MUST carry a label; the spec-set
        // ones must not need one.
        if (e.category >= 128) CHECK_FALSE(e.categoryLabel.empty());
        for (const LayoutField& f : dc.c.layoutFields(e)) {
            if (!f.hasSettingKey) continue;
            CAPTURE(f.name);
            CHECK_FALSE(f.desc.empty());                       // help affordance
            CHECK(f.desc.size() <= limits::desc_max_bytes);    // registry cap
            CHECK_FALSE(f.group.empty());                      // card heading
            CHECK(f.dflt.has());                               // factory reset
            // Widget choice: a select needs options, everything else needs a
            // numeric range. There is deliberately no widget hint on the wire.
            if (!dc.c.optionLabels(f).empty()) {
                CHECK(f.type == PackedFieldType::u8);
            } else {
                CHECK(f.hasMin);
                CHECK(f.hasMax);
                CHECK(f.max > f.min);
            }
        }
    }
}

// ============================================================================
// M5a — 0x0086 plan-strip: the planner's current segment.
// ============================================================================
TEST_CASE("device catalog: 0x0086 plan-strip is an elevated diagnostics STATE") {
    DeviceCatalog dc;
    const CatalogEntry* e = dc.c.find(slopdrive::ch::plan_strip);
    REQUIRE(e != nullptr);
    CHECK(e->cls == ChannelClass::STATE);
    CHECK(e->dir == Direction::h2c);
    CHECK(e->access == AccessLevel::watch);
    CHECK(e->defaultPriority == Priority::elevated);
    CHECK(e->maxRateHz >= 45.0f);
    CHECK(e->category == setting_categories::diagnostics);
    // STATE, so stream_kind does not apply and must be left at the default —
    // an explicit `samples` on a STATE entry would imply a classification that
    // isSegmentClass() never reads.
    CHECK(e->streamKind == stream_kinds::samples);
    CHECK_FALSE(dc.c.isSegmentClass(*e));

    auto f = dc.c.layoutFields(*e);
    REQUIRE(f.size() == 8);
    CHECK(f[0].name == "flags");
    CHECK(f[0].type == PackedFieldType::bitfield8);
    auto bits = dc.c.bitLabels(f[0]);
    REQUIRE(bits.size() == 3);
    CHECK(bits[0] == "active");
    CHECK(bits[1] == "live_mode");
    CHECK(bits[2] == "grad_mode");
    // slopmotion::Mode order — Idle 0, Waveform 1, Chase 2, Settle 3.
    auto style = dc.c.optionLabels(f[1]);
    REQUIRE(style.size() == 4);
    CHECK(style[0] == "idle");
    CHECK(style[1] == "waveform");
    CHECK(style[2] == "chase");
    CHECK(style[3] == "settle");
    // Nothing here is writable — it is a read-out of what the planner did.
    for (const LayoutField& lf : f) {
        CAPTURE(lf.name);
        CHECK_FALSE(lf.hasSettingKey);
    }
}

// ============================================================================
// RFC-016 in practice — CAPABILITY DISCOVERY IS CATALOG INTROSPECTION.
//
// The power channel exists IFF the hardware does. A hub with no sensor must
// not advertise a channel that would publish zeros forever, because a client
// cannot tell "0.0 A" from "no sensor" — the same lie as the WebUI's dead
// anomaly gauges this milestone exists to kill.
// ============================================================================
TEST_CASE("device catalog: 0x0087 power is declared only when the hardware exists") {
    Catalog32 none{};
    REQUIRE(slopdrive::buildSlopDriveCatalog(none, {false, false}));
    CHECK(none.find(slopdrive::ch::power) == nullptr);
    // ...and the ABSENCE must not disturb anything else: ascending order and
    // the conformance check hold either way.
    for (uint16_t i = 1; i < none.count; ++i) CHECK(none.entries[i - 1].id < none.entries[i].id);

    // A power monitor with no current sensor is still no power channel: the
    // bus/current fields are the channel's reason to exist.
    Catalog32 tempOnly{};
    REQUIRE(slopdrive::buildSlopDriveCatalog(tempOnly, {false, true}));
    CHECK(tempOnly.find(slopdrive::ch::power) == nullptr);

    // Shunt but no thermal sensor: 3 fields, no die_c10 column of zeros.
    Catalog32 shunt{};
    REQUIRE(slopdrive::buildSlopDriveCatalog(shunt, {true, false}));
    const CatalogEntry* e = shunt.find(slopdrive::ch::power);
    REQUIRE(e != nullptr);
    CHECK(e->fieldCount == 3);
    CHECK(shunt.layoutWireSize(*e) == 6);
    CHECK(layoutFieldByName(shunt, *e, "die_c10") == nullptr);

    // The full rig: 4 fields, and the roles a generic client locates them by.
    DeviceCatalog dc;
    const CatalogEntry* full = dc.c.find(slopdrive::ch::power);
    REQUIRE(full != nullptr);
    CHECK(full->defaultPriority == Priority::background);
    CHECK(full->maxRateHz <= 10.0f);
    CHECK(full->fieldCount == 4);
    const LayoutField* die = layoutFieldByName(dc.c, *full, "die_c10");
    REQUIRE(die != nullptr);
    CHECK(die->role == slopsync::field_roles::telemetry_temp);
    CHECK(die->scale == doctest::Approx(10.0f));
    const LayoutField* bus = layoutFieldByName(dc.c, *full, "bus_mV");
    REQUIRE(bus != nullptr);
    CHECK(bus->role == slopsync::field_roles::telemetry_power_bus);
}

// ============================================================================
// M5a — 0x0088 slopmotion-diag, incl. RFC-019's observable reset generation.
// ============================================================================
TEST_CASE("device catalog: 0x0088 carries the per-kind breakdown and a reset_gen") {
    DeviceCatalog dc;
    const CatalogEntry* e = dc.c.find(slopdrive::ch::motion_diag);
    REQUIRE(e != nullptr);
    CHECK(e->cls == ChannelClass::STATE);
    CHECK(e->defaultPriority == Priority::background);
    CHECK(e->category == setting_categories::diagnostics);

    // One named field per slopmotion::AnomalyType, in the enum's own order.
    const char* kinds[] = {"anom_none",
                           "anom_plan_failed",
                           "anom_settle",
                           "anom_endvel_clamped",
                           "anom_deadline_stretched",
                           "anom_waveform_fallback",
                           "anom_waveform_scaled",
                           "anom_waveform_centred",
                           "anom_handoff_bounded",
                           "anom_waveform_smoothed"};
    for (const char* nm : kinds) {
        CAPTURE(nm);
        const LayoutField* f = layoutFieldByName(dc.c, *e, nm);
        REQUIRE(f != nullptr);
        CHECK(f->type == PackedFieldType::u32);
    }

    // RFC-019: the reset must be observable to EVERY subscriber, which is what
    // the role tag advertises. Without it a watcher sees the counters jump
    // backwards and cannot tell a reset from a reboot.
    const LayoutField* rg = layoutFieldByName(dc.c, *e, "reset_gen");
    REQUIRE(rg != nullptr);
    CHECK(rg->role == slopsync::field_roles::meta_reset_gen);
    CHECK(rg->type == PackedFieldType::u16);
    CHECK_FALSE(rg->hasSettingKey);   // it is a generation counter, not a knob
}

// ============================================================================
// M5a — 0x0089 motion-anomaly: THE FIRST DEVICE-AUTHORED EVENT CHANNEL, and
// therefore the proof that the M3b `body` (40) sub-map grammar fix works.
//
// Under the pre-fix grammar (kind-specific fields at the frame's top level)
// this channel could not have been authored without a registry PR for its own
// field keys — exactly the coupling the self-describing catalog exists to
// prevent. Every key below comes from the channel's OWN schema.
// ============================================================================
TEST_CASE("device catalog: 0x0089 motion-anomaly is a device-authored EVENT channel") {
    DeviceCatalog dc;
    const CatalogEntry* e = dc.c.find(slopdrive::ch::motion_anomaly);
    REQUIRE(e != nullptr);
    CHECK(e->cls == ChannelClass::EVENT);
    CHECK(e->dir == Direction::h2c);
    CHECK(e->access == AccessLevel::watch);
    CHECK(e->maxRateHz == 0.0f);       // EVENTs are edge-driven, never paced
    // An anomaly is an EDGE; §9.4's default (no replay) is right for it, and
    // the 0x0088 counters are the durable record. That duality is the design.
    CHECK_FALSE(e->hasReplayDepth);

    // The body keys are this channel's own — no registry number was invented.
    const SchemaField* kind = schemaFieldByKey(dc.c, *e, slopdrive::anom_body::kind);
    REQUIRE(kind != nullptr);
    CHECK(kind->name == "kind");
    auto labels = dc.c.optionLabels(*kind);
    // Index-aligned with slopmotion::AnomalyType, which is append-only.
    REQUIRE(labels.size() == 10);
    CHECK(labels[0] == "none");
    CHECK(labels[1] == "plan_failed");
    CHECK(labels[2] == "settle");
    CHECK(labels[3] == "endvel_clamped");
    CHECK(labels[4] == "deadline_stretched");
    CHECK(labels[5] == "waveform_fallback");
    CHECK(labels[6] == "waveform_scaled");
    CHECK(labels[7] == "waveform_centred");
    // M4d (RFC-008): the guard's own kind. Its label must exist here or a
    // generic client prints the ordinal "8" for the one anomaly that is about
    // the CLIENT's own content.
    CHECK(labels[8] == "handoff_bounded");
    // slopmotion 0.8.0: the budgeted policies' own kind — the span's end handle
    // was lerped toward its chord to make the shape legal. Unlabelled it would
    // print as "9" for the one anomaly that reports a deliberate TRADEOFF the
    // operator configured, rather than a limit the machine ran into.
    CHECK(labels[9] == "waveform_smoothed");

    for (uint8_t k : {slopdrive::anom_body::seq, slopdrive::anom_body::target,
                      slopdrive::anom_body::detail, slopdrive::anom_body::t_us}) {
        CAPTURE(int(k));
        CHECK(schemaFieldByKey(dc.c, *e, k) != nullptr);
    }
    CHECK(schemaFieldByKey(dc.c, *e, slopdrive::anom_body::target)->type == CborFieldType::f32_t);
}

// ============================================================================
// M5a — the annotation block SURVIVES THE WIRE. In-memory struct fields prove
// nothing: a client only ever sees the decoded catalog, so an annotation the
// codec drops is an annotation that does not exist.
// ============================================================================
TEST_CASE("device catalog: annotations survive encode -> decode") {
    DeviceCatalog dc;
    std::vector<std::byte> buf(65536);
    size_t n = encodeCatalog(dc.c, buf);
    REQUIRE(n > 0);

    Catalog32 back{};
    REQUIRE(decodeCatalog(std::span<const std::byte>(buf).first(n), back).isOk());
    REQUIRE(back.count == dc.c.count);

    const CatalogEntry* cfg = back.find(slopdrive::ch::machine_config);
    REQUIRE(cfg != nullptr);
    CHECK(cfg->hasSettingChannel);
    CHECK(cfg->settingChannel == slopdrive::ch::config_set);
    CHECK(cfg->hasCategory);
    CHECK(cfg->category == setting_categories::limits);

    const LayoutField* us = layoutFieldByName(back, *cfg, "user_speed");
    REQUIRE(us != nullptr);
    CHECK(us->hasSettingKey);
    CHECK(us->settingKey == 3);
    CHECK(us->role == slopsync::field_roles::limit_user_speed);
    CHECK(us->group == "Manual limits");
    CHECK_FALSE(us->desc.empty());
    CHECK(us->dflt.kind == SettingDefault::Kind::Float);
    CHECK(us->dflt.asFloat() == doctest::Approx(50.0f));
    CHECK(us->hasStep);

    const LayoutField* jerk = layoutFieldByName(back, *cfg, "input_jerk");
    REQUIRE(jerk != nullptr);
    CHECK((jerk->flags & setting_flags::advanced) != 0);

    const LayoutField* rail = layoutFieldByName(back, *cfg, "max_rail");
    REQUIRE(rail != nullptr);
    CHECK_FALSE(rail->hasSettingKey);   // read-only survives as an ABSENCE

    const LayoutField* mask = layoutFieldByName(back, *cfg, "enabled_mask");
    REQUIRE(mask != nullptr);
    CHECK(mask->role == slopsync::field_roles::meta_enabled_mask);
    CHECK(back.bitLabels(*mask).size() == 7);

    const CatalogEntry* pat = back.find(slopdrive::ch::pattern_state);
    REQUIRE(pat != nullptr);
    const LayoutField* p = layoutFieldByName(back, *pat, "pattern");
    REQUIRE(p != nullptr);
    auto opts = back.optionLabels(*p);
    REQUIRE(opts.size() == 7);
    CHECK(opts[1] == "Teasing Pounding");

    const CatalogEntry* anom = back.find(slopdrive::ch::motion_anomaly);
    REQUIRE(anom != nullptr);
    const SchemaField* kind = schemaFieldByKey(back, *anom, slopdrive::anom_body::kind);
    REQUIRE(kind != nullptr);
    CHECK(back.optionLabels(*kind).size() == 10);

    // Deterministic re-encode: the etag is a function of CONTENT (§8.3), and
    // an annotation that round-trips lossily would break that quietly.
    std::vector<std::byte> buf2(65536);
    size_t n2 = encodeCatalog(back, buf2);
    REQUIRE(n2 == n);
    CHECK(std::equal(buf.begin(), buf.begin() + n, buf2.begin()));
}

// ============================================================================
// M5a — the conformance checker is CLEAN for every feature combination, incl.
// limits::catalog_max_entry_bytes (4096). A heavily-annotated entry with long
// descs is the one thing that can blow that cap, and 0x0088 (24 fields) plus
// 0x0081 (9 annotated fields) are the two candidates.
// ============================================================================
TEST_CASE("device catalog: conformance is clean under every feature combination") {
    std::vector<std::byte> scratch(65536);
    for (int f = 0; f < 4; ++f) {
        CAPTURE(f);
        Catalog32 c{};
        REQUIRE(slopdrive::buildSlopDriveCatalog(c, {(f & 1) != 0, (f & 2) != 0}));
        auto report = conformance::checkCatalog(c, std::span<std::byte>(scratch));
        CHECK(report.ok());
    }
}

// ============================================================================
// M5a — THE CATALOG MUST FIT THE HUB'S ENCODE SCRATCH.
//
// This is the test that would have saved a live probe run. encodeCatalog()
// returns 0 when the buffer is too small, and Hub's constructor has no way to
// report that: it hashes the zero bytes into an etag, serves an empty catalog,
// and every other symptom of health remains. The device's annotations pushed
// the encoding past the old 8 KiB default and the machine simply advertised
// nothing while answering HELLO cheerfully.
//
// Checked for EVERY feature combination, because the power channel changes the
// size, and with real headroom asserted — a catalog at 99% of the buffer is a
// catalog one desc away from silently vanishing.
// ============================================================================
TEST_CASE("device catalog: encodes inside the hub's catalog scratch, with headroom") {
    std::vector<std::byte> buf(Hub::catalogScratchCapacity());
    for (int f = 0; f < 4; ++f) {
        CAPTURE(f);
        Catalog32 c{};
        REQUIRE(slopdrive::buildSlopDriveCatalog(c, {(f & 1) != 0, (f & 2) != 0}));
        size_t n = encodeCatalog(c, buf);
        CAPTURE(n);
        CAPTURE(Hub::catalogScratchCapacity());
        CHECK(n > 0);                                          // 0 == did not fit
        CHECK(n <= Hub::catalogScratchCapacity() * 8 / 10);    // >=20% headroom
    }
}
