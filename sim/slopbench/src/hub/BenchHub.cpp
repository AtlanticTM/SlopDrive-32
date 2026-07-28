// BenchHub — config-driven HubDelegate implementation (see BenchHub.h for
// the catalog-authoring contract).

#include "hub/BenchHub.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "slopsync/channel/log_channel.hpp"
#include "slopsync/channel/safety_events_channel.hpp"
#include "slopsync/channel/trust_channels.hpp"
#include "slopsync/wire/packed/layout_codec.hpp"

namespace slopbench {

using slopsync::AccessLevel;
using slopsync::CatalogEntry;
using slopsync::CborFieldType;
using slopsync::ChannelClass;
using slopsync::Direction;
using slopsync::IntentValue;
using slopsync::IntentValueMap;
using slopsync::LayoutField;
using slopsync::NackCode;
using slopsync::PackedFieldType;
using slopsync::Priority;
using slopsync::Result;
using slopsync::SchemaField;
using slopsync::SettingDefault;
using slopsync::StoreDescriptor;

namespace {

PackedFieldType toPackedType(LayoutKind k) {
    switch (k) {
        case LayoutKind::U8: return PackedFieldType::u8;
        case LayoutKind::I8: return PackedFieldType::i8;
        case LayoutKind::U16: return PackedFieldType::u16;
        case LayoutKind::I16: return PackedFieldType::i16;
        case LayoutKind::U32: return PackedFieldType::u32;
        case LayoutKind::I32: return PackedFieldType::i32;
        case LayoutKind::F32: return PackedFieldType::f32;
        case LayoutKind::Bitfield8: return PackedFieldType::bitfield8;
        case LayoutKind::Str16: return PackedFieldType::str16;
        case LayoutKind::Str32: return PackedFieldType::str32;
        case LayoutKind::Str64: return PackedFieldType::str64;
    }
    return PackedFieldType::f32;
}

CborFieldType toCborType(SchemaKind k) {
    switch (k) {
        case SchemaKind::Uint: return CborFieldType::uint_t;
        case SchemaKind::Int: return CborFieldType::int_t;
        case SchemaKind::F32: return CborFieldType::f32_t;
        case SchemaKind::Bool: return CborFieldType::bool_t;
        case SchemaKind::Tstr: return CborFieldType::tstr_t;
        case SchemaKind::Bstr: return CborFieldType::bstr_t;
    }
    return CborFieldType::f32_t;
}

AccessLevel toAccessLevel(AccessKind k) {
    switch (k) {
        case AccessKind::Watch: return AccessLevel::watch;
        case AccessKind::Control: return AccessLevel::control;
        case AccessKind::Configure: return AccessLevel::configure;
    }
    return AccessLevel::watch;
}

ChannelClass toChannelClass(ClassKind k) {
    switch (k) {
        case ClassKind::State: return ChannelClass::STATE;
        case ClassKind::Intent: return ChannelClass::INTENT;
        case ClassKind::Event: return ChannelClass::EVENT;
        case ClassKind::Stream: return ChannelClass::STREAM;
        case ClassKind::Store: return ChannelClass::STORE;
    }
    return ChannelClass::STATE;
}

Direction defaultDirectionFor(ClassKind k) {
    switch (k) {
        case ClassKind::State:
        case ClassKind::Event:
        case ClassKind::Store: return Direction::h2c;
        case ClassKind::Intent:
        case ClassKind::Stream: return Direction::c2h;
    }
    return Direction::h2c;
}

float numericValueOf(const IntentValue& v, bool* isNumeric) {
    *isNumeric = true;
    switch (v.kind) {
        case IntentValue::Kind::F32: return v.f32_val;
        case IntentValue::Kind::U64: return float(v.u64_val);
        case IntentValue::Kind::I64: return float(v.i64_val);
        default: break;
    }
    *isNumeric = false;
    return 0.0f;
}

IntentValue withNumericValue(IntentValue v, float physical) {
    switch (v.kind) {
        case IntentValue::Kind::F32: v.f32_val = physical; break;
        case IntentValue::Kind::U64: v.u64_val = physical >= 0.0f ? uint64_t(std::llround(double(physical))) : 0; break;
        case IntentValue::Kind::I64: v.i64_val = int64_t(std::llround(double(physical))); break;
        default: break;
    }
    return v;
}

std::string formatFieldValue(const FieldConfig& f, float physical, const std::string& str) {
    char buf[64];
    if (f.isStringLayout()) return str;
    if (f.layoutType == LayoutKind::Bitfield8) {
        std::snprintf(buf, sizeof(buf), "0x%02X", unsigned(physical) & 0xFF);
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "%.3f", double(physical));
    return buf;
}

// ---- Catalog authoring ------------------------------------------------------
// Free functions, not BenchHub methods: Hub::Hub() encodes the catalog (and
// its etag) ONCE, synchronously, reading its Catalog32& argument at that
// exact moment — see BenchHub.h's comment on the `_catalog`/`_hub` member
// order. Authoring therefore has to finish BEFORE `_hub` is constructed,
// which means it cannot run in BenchHub's constructor BODY (every member
// already exists by then); it runs inside `_hub`'s own member-initializer
// expression instead, via buildCatalogInto() below. Category-id and store-id
// assignment state lives in a small local helper object, not on BenchHub,
// because nothing outside catalog authoring ever needs it again.

class CatalogBuilder {
public:
    explicit CatalogBuilder(slopsync::Catalog32& c) : _c(c) {}

    void build(const HubConfig& cfg) {
        _c.clear();
        addHandAuthoredSpecCore();
        if (!slopsync::addLogChannel(_c)) return;
        if (!slopsync::addTrustChannels(_c)) return;
        if (!slopsync::addSafetyEventsChannel(_c)) return;

        // Which intent (if any) writes each state channel, and that intent's
        // field-name -> wire-key map -- computed once so addConfigChannel()
        // can annotate the RFC-009 setting_key/setting_channel link below
        // without a per-field lookup pass of its own.
        std::map<uint16_t, const ChannelConfig*> writerFor;
        for (const auto& ch : cfg.channels) {
            if (ch.cls == ClassKind::Intent && ch.hasWrites) writerFor.emplace(ch.writesChannelId, &ch);
        }

        std::vector<const ChannelConfig*> order;
        order.reserve(cfg.channels.size());
        for (const auto& ch : cfg.channels) order.push_back(&ch);
        std::sort(order.begin(), order.end(),
                 [](const ChannelConfig* a, const ChannelConfig* b) { return a->id < b->id; });

        for (const ChannelConfig* ch : order) {
            std::map<std::string, uint8_t> keyByName;
            uint16_t writerId = 0;
            auto wf = writerFor.find(ch->id);
            if (ch->cls == ClassKind::State && wf != writerFor.end()) {
                writerId = wf->second->id;
                for (const auto& f : wf->second->fields) keyByName[f.name] = f.key;
            }
            addConfigChannel(*ch, writerId, (ch->cls == ClassKind::State && !keyByName.empty()) ? &keyByName : nullptr);
        }
    }

private:
    uint8_t categoryIdFor(const std::string& name) {
        static const std::map<std::string, uint8_t> kNamed = {
            {"control", 1}, {"motion", 2}, {"safety", 3}, {"limits", 4}, {"library", 5},
            {"playback", 6}, {"auxiliary", 7}, {"automation", 8}, {"tuning", 9},
            {"hardware", 10}, {"network", 11}, {"session", 12}, {"system", 13}, {"other", 14},
        };
        auto it = kNamed.find(name);
        if (it != kNamed.end()) return it->second;

        auto assigned = _categoryIds.find(name);
        if (assigned != _categoryIds.end()) return assigned->second;
        const uint8_t id = _nextVendorCategory++;
        _categoryIds[name] = id;
        return id;
    }

    void addHandAuthoredSpecCore() {
        // Verbatim shape of sim/slopsim's SlopSimCatalog.h spec-core block: the
        // hub library hardcodes these exact layouts (safety word, control-owner
        // source/owner pairs, safety-intents ops, hub-status fields,
        // session-events fields) — see hub.hpp's own comments on each. Copied,
        // not shared, because every conformant hub authors this block itself.
        slopsync::Catalog32& c = _c;

        c.addEntry({.id = slopsync::channels::safety, .name = "safety",
                    .cls = ChannelClass::STATE, .dir = Direction::h2c,
                    .access = AccessLevel::watch, .maxRateHz = 0.0f,
                    .defaultPriority = Priority::critical});
        c.addBitfieldField({.name = "word", .type = PackedFieldType::bitfield8, .unit = "flag", .scale = 1.0f},
                           {"estop", "stop", "hold", "pause"});
        c.addLayoutField({.name = "cause", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f});
        c.addLayoutField({.name = "owner_session", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f});
        c.addLayoutField({.name = "estop_seq", .type = PackedFieldType::u16, .unit = "count", .scale = 1.0f});
        c.addBitfieldField({.name = "modes", .type = PackedFieldType::bitfield8, .unit = "flag", .scale = 1.0f},
                           {"override", "bypass"});

        c.addEntry({.id = slopsync::channels::control_owner, .name = "control-owner",
                    .cls = ChannelClass::STATE, .dir = Direction::h2c,
                    .access = AccessLevel::watch, .maxRateHz = 0.0f,
                    .defaultPriority = Priority::critical});
        c.addLayoutField({.name = "src0", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f});
        c.addLayoutField({.name = "owner0", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f});
        c.addLayoutField({.name = "src1", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f});
        c.addLayoutField({.name = "owner1", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f});
        c.addLayoutField({.name = "src2", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f});
        c.addLayoutField({.name = "owner2", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f});
        c.addLayoutField({.name = "src3", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f});
        c.addLayoutField({.name = "owner3", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f});

        c.addEntry({.id = slopsync::channels::safety_intents, .name = "safety-intents",
                    .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                    .access = AccessLevel::watch, .maxRateHz = 20.0f,
                    .defaultPriority = Priority::critical});
        c.addSelectSchemaField({.key = 1, .name = "op", .type = CborFieldType::uint_t, .unit = ""},
                               {"reserved", "estop_clear", "stop", "hold", "pause", "resume",
                                "estop", "override_on", "override_off", "bypass_on", "bypass_off"},
                               {AccessLevel::control, AccessLevel::control, AccessLevel::watch,
                                AccessLevel::control, AccessLevel::control, AccessLevel::control,
                                AccessLevel::watch, AccessLevel::control, AccessLevel::control,
                                AccessLevel::control, AccessLevel::control});

        c.addEntry({.id = slopsync::channels::hub_status, .name = "hub-status",
                    .cls = ChannelClass::STATE, .dir = Direction::h2c,
                    .access = AccessLevel::watch, .maxRateHz = 1.0f,
                    .defaultPriority = Priority::background});
        c.addLayoutField({.name = "heap_free", .type = PackedFieldType::u32, .unit = "B", .scale = 1.0f});
        c.addLayoutField({.name = "uptime_s", .type = PackedFieldType::u32, .unit = "s", .scale = 1.0f});
        c.addLayoutField({.name = "rssi", .type = PackedFieldType::i8, .unit = "dBm", .scale = 1.0f});
        c.addLayoutField({.name = "sessions", .type = PackedFieldType::u8, .unit = "count", .scale = 1.0f});
        c.addLayoutField({.name = "log_dropped", .type = PackedFieldType::u32, .unit = "count", .scale = 1.0f});

        c.addEntry({.id = slopsync::channels::session_events, .name = "session-events",
                    .cls = ChannelClass::EVENT, .dir = Direction::h2c,
                    .access = AccessLevel::watch, .maxRateHz = 0.0f,
                    .defaultPriority = Priority::normal});
        c.addSchemaField({.key = 1, .name = "source", .type = CborFieldType::uint_t, .unit = ""});
        c.addSchemaField({.key = 2, .name = "session", .type = CborFieldType::uint_t, .unit = ""});
    }

    void addConfigChannel(const ChannelConfig& ch, uint16_t writerChannelId,
                          const std::map<std::string, uint8_t>* writerFieldKeys) {
        CatalogEntry header{};
        header.id = ch.id;
        header.name = ch.name;
        header.cls = toChannelClass(ch.cls);
        header.dir = defaultDirectionFor(ch.cls);
        header.access = toAccessLevel(ch.access);
        header.maxRateHz = ch.rateHz;
        header.defaultPriority = Priority::normal;
        if (!ch.category.empty()) {
            header.hasCategory = true;
            header.category = categoryIdFor(ch.category);
            if (header.category >= 0x40) header.categoryLabel = ch.category;
        }
        (void)writerChannelId;

        CatalogEntry* entry = _c.addEntry(header);
        if (!entry) return;  // overflow already latched on the catalog; begin() reports it

        switch (ch.cls) {
            case ClassKind::State:
            case ClassKind::Stream: {
                for (const auto& f : ch.fields) {
                    LayoutField lf{};
                    lf.name = f.name;
                    lf.type = toPackedType(f.layoutType);
                    lf.unit = f.unit;
                    lf.scale = 1.0f;
                    lf.hasMin = f.hasMin;
                    lf.hasMax = f.hasMax;
                    lf.min = f.min;
                    lf.max = f.max;
                    if (f.hasDefault && f.isNumericLayout()) lf.dflt = SettingDefault::ofFloat(f.defaultValue);
                    // RFC-009 stored/effective split, cheaply: a STATE field this
                    // hub also accepts writes for (via some intent's `writes`
                    // link) advertises WHICH intent key writes it, so a generic
                    // client can discover the relationship from the catalog
                    // alone rather than from this tool's own config file.
                    if (writerFieldKeys) {
                        auto it = writerFieldKeys->find(f.name);
                        if (it != writerFieldKeys->end()) {
                            lf.hasSettingKey = true;
                            lf.settingKey = it->second;
                        }
                    }
                    _c.addLayoutField(lf);
                }
                if (writerFieldKeys && !writerFieldKeys->empty()) {
                    entry->hasSettingChannel = true;
                    entry->settingChannel = writerChannelId;
                }
                break;
            }
            case ClassKind::Intent:
            case ClassKind::Event: {
                for (const auto& f : ch.fields) {
                    SchemaField sf{};
                    sf.key = f.key;
                    sf.name = f.name;
                    sf.type = toCborType(f.schemaType);
                    sf.unit = f.unit;
                    sf.hasMin = f.hasMin;
                    sf.hasMax = f.hasMax;
                    sf.min = f.min;
                    sf.max = f.max;
                    if (f.hasDefault) {
                        sf.dflt = (f.schemaType == SchemaKind::Bool) ? SettingDefault::ofBool(f.defaultValue != 0.0f)
                                                                      : SettingDefault::ofFloat(f.defaultValue);
                    }
                    _c.addSchemaField(sf);
                }
                break;
            }
            case ClassKind::Store: {
                StoreDescriptor d{};
                d.storeId = _nextStoreId++;
                d.kind = ch.storeKind;
                d.capacity = ch.storeCapacity;
                d.perItemMax = ch.storePerItemMax;
                d.nameMax = ch.storeNameMax;
                _c.addStoreDescriptor(d);
                break;
            }
        }
    }

    slopsync::Catalog32& _c;
    std::map<std::string, uint8_t> _categoryIds;
    uint8_t _nextVendorCategory = 0x40;
    // Starts at 2: addTrustChannels() always claims store id 1 for the
    // paired-devices ledger (its own default parameter) whether or not this
    // hub's config declares any store of its own.
    uint8_t _nextStoreId = 2;
};

// Populates `c` from `cfg` and returns it (Catalog32& in, Catalog32& out —
// the SAME reference) so this can be called directly inside a member-
// initializer expression: `_hub(buildCatalogInto(_catalog, cfg), ...)`. See
// BenchHub.h's comment on `_catalog`/`_hub` declaration order for why that
// placement, not the constructor body, is where this must run.
slopsync::Catalog32& buildCatalogInto(slopsync::Catalog32& c, const HubConfig& cfg) {
    CatalogBuilder builder(c);
    builder.build(cfg);
    return c;
}

}  // namespace

BenchHub::BenchHub(SessionLog& log, const HubConfig& cfg)
    : _log(log), _cfg(cfg), _hub(buildCatalogInto(_catalog, cfg), _clock, _rng, *this) {
    buildRuntime();
    _startMs = _clock.nowMs32();
    _hub.setIdentity(_cfg.product, _cfg.fwVersion, _cfg.name);
    // Seed every declared STATE channel's retained snapshot once at startup
    // (its configured defaults, or all-zero absent one). Without this, a
    // channel nothing has animated or written yet has NEVER called
    // publishState() -- the RetainedStore has nothing to hand a new
    // subscriber, so "declared but silent since boot" would read as "the hub
    // is broken" rather than "nobody has touched this yet".
    for (auto& sr : _states) publishStateChannel(sr);
}

bool BenchHub::begin(uint16_t port) {
    if (!_catalog.ok()) {
        _log.logf('E', "catalog overflow -- shrink the config or grow Catalog32's pool sizes");
        return false;
    }
    if (_hub.catalogEncodedBytes() == 0) {
        _log.logf('E', "catalog encoded to 0 bytes -- see hub.hpp catalogEncodedBytes() comment");
        return false;
    }
    return _port.begin(&_hub, port, &_log);
}

void BenchHub::shutdown() { _port.stop(); }

// ---- Runtime tables ---------------------------------------------------------

void BenchHub::buildRuntime() {
    for (const auto& ch : _cfg.channels) {
        if (ch.cls == ClassKind::State) {
            StateRuntime sr;
            sr.cfg = &ch;
            sr.values.resize(ch.fields.size(), 0.0f);
            sr.strValues.resize(ch.fields.size());
            for (size_t i = 0; i < ch.fields.size(); ++i) {
                if (ch.fields[i].hasDefault) sr.values[i] = ch.fields[i].defaultValue;
            }
            _states.push_back(std::move(sr));
        }
    }

    for (const auto& ch : _cfg.channels) {
        if (ch.cls != ClassKind::Intent) continue;
        IntentRuntime ir;
        ir.cfg = &ch;
        ir.echoDelayMs = ch.hasEchoDelay ? ch.echoDelayMs : _cfg.echoDelayMs;

        StateRuntime* target = ch.hasWrites ? findState(ch.writesChannelId) : nullptr;
        for (const auto& f : ch.fields) {
            IntentFieldLink link;
            link.key = f.key;
            link.numeric = f.isNumericSchema();
            link.hasMin = f.hasMin;
            link.hasMax = f.hasMax;
            link.min = f.min;
            link.max = f.max;
            if (target) {
                for (size_t i = 0; i < target->cfg->fields.size(); ++i) {
                    if (target->cfg->fields[i].name == f.name) {
                        link.hasTarget = true;
                        link.targetStateId = target->cfg->id;
                        link.targetFieldIndex = i;
                        link.targetIsString = target->cfg->fields[i].isStringLayout();
                        break;
                    }
                }
            }
            ir.fields.push_back(link);
        }
        _intents.push_back(std::move(ir));
    }
}

BenchHub::StateRuntime* BenchHub::findState(uint16_t id) {
    for (auto& s : _states) if (s.cfg->id == id) return &s;
    return nullptr;
}

BenchHub::IntentRuntime* BenchHub::findIntent(uint16_t id) {
    for (auto& i : _intents) if (i.cfg->id == id) return &i;
    return nullptr;
}

void BenchHub::publishStateChannel(StateRuntime& sr) {
    std::array<std::byte, slopsync::limits::min_transport_payload> buf{};
    size_t offset = 0;
    for (size_t i = 0; i < sr.cfg->fields.size(); ++i) {
        const FieldConfig& f = sr.cfg->fields[i];
        LayoutField lf{};
        lf.type = toPackedType(f.layoutType);
        lf.scale = 1.0f;
        const size_t n = lf.wireSize();
        if (offset + n > buf.size()) break;  // conformance floor, never expected in practice
        auto out = std::span<std::byte>(buf.data() + offset, n);
        if (f.isStringLayout()) slopsync::packStringField(lf, sr.strValues[i], out);
        else slopsync::packField(lf, sr.values[i], out);
        offset += n;
    }
    _hub.publishState(sr.cfg->id, std::span<const std::byte>(buf.data(), offset));
    sr.dirty = false;
}

void BenchHub::tickAnimations(uint32_t nowMs) {
    const float t = float(nowMs - _startMs) / 1000.0f;
    for (auto& sr : _states) {
        bool any = false;
        for (size_t i = 0; i < sr.cfg->fields.size(); ++i) {
            const FieldConfig& f = sr.cfg->fields[i];
            if (f.anim == AnimKind::None) continue;
            any = true;
            if (f.anim == AnimKind::Sine) {
                sr.values[i] = f.animB + f.animA * std::sin(2.0f * 3.14159265f * f.animHz * t);
            } else {  // Ramp: a triangle wave between animA (min) and animB (max)
                const float period = f.animHz > 0.0f ? 1.0f / f.animHz : 1.0f;
                float phase = std::fmod(t, period) / period;
                if (phase < 0.0f) phase += 1.0f;
                const float tri = phase < 0.5f ? phase * 2.0f : 2.0f - phase * 2.0f;
                sr.values[i] = f.animA + (f.animB - f.animA) * tri;
            }
        }
        if (any) sr.dirty = true;
    }
}

void BenchHub::tickPendingMirrors(uint32_t nowMs) {
    std::vector<PendingMirror> stillPending;
    stillPending.reserve(_pending.size());
    for (auto& pm : _pending) {
        if (int32_t(nowMs - pm.dueMs) < 0) {
            stillPending.push_back(std::move(pm));
            continue;
        }
        StateRuntime* sr = findState(pm.stateId);
        if (sr && pm.fieldIndex < sr->values.size()) {
            if (pm.isString) sr->strValues[pm.fieldIndex] = pm.strValue;
            else sr->values[pm.fieldIndex] = pm.value;
            sr->dirty = true;
        }
    }
    _pending.swap(stillPending);

    for (auto& sr : _states) {
        if (sr.dirty) publishStateChannel(sr);
    }
}

void BenchHub::logWrite(const std::string& channelName, const std::string& summary, uint32_t nowMs) {
    _writeLog.push_back({nowMs, channelName, summary});
    if (_writeLog.size() > kWriteLogCapacity) _writeLog.pop_front();
}

void BenchHub::tick() {
    const uint32_t nowMs = _clock.nowMs32();
    _port.loop(nowMs);
    _hub.update(_clock.nowUs());
    tickAnimations(nowMs);
    tickPendingMirrors(nowMs);
}

// ---- HubDelegate ------------------------------------------------------------

AccessLevel BenchHub::validateToken(std::span<const std::byte>, std::span<const std::byte>, bool) {
    // See the class-level comment: SlopBench grants every session `configure`
    // on purpose. It is a write-plane/catalog test double, not an auth
    // conformance harness.
    return AccessLevel::configure;
}

Result<IntentValueMap, NackCode> BenchHub::applyIntent(uint16_t channel_id, const IntentValueMap& requested,
                                                       AccessLevel, bool& cfgChanged) {
    cfgChanged = false;
    IntentRuntime* rt = findIntent(channel_id);
    if (!rt) return Result<IntentValueMap, NackCode>::err(NackCode::UNKNOWN_CHANNEL);

    const uint32_t nowMs = _clock.nowMs32();
    IntentValueMap applied{};
    std::string summary;

    for (uint32_t i = 0; i < requested.count && i < slopsync::kIntentMaxValueFields; ++i) {
        const auto& rf = requested.fields[i];
        const IntentFieldLink* link = nullptr;
        for (const auto& L : rt->fields) if (L.key == rf.key) { link = &L; break; }

        IntentValue v = rf.value;
        float physical = 0.0f;
        bool wasNumeric = false;
        if (link && link->numeric) {
            physical = numericValueOf(v, &wasNumeric);
            if (wasNumeric) {
                const float before = physical;
                if (link->hasMin && physical < link->min) physical = link->min;
                if (link->hasMax && physical > link->max) physical = link->max;
                v = withNumericValue(v, physical);
                if (!summary.empty()) summary += ", ";
                char buf[80];
                if (physical != before) std::snprintf(buf, sizeof(buf), "key%u=%.3f(clamped from %.3f)", unsigned(rf.key), double(physical), double(before));
                else std::snprintf(buf, sizeof(buf), "key%u=%.3f", unsigned(rf.key), double(physical));
                summary += buf;
            }
        } else if (v.kind == IntentValue::Kind::Tstr) {
            if (!summary.empty()) summary += ", ";
            summary += "key" + std::to_string(rf.key) + "=\"" + std::string(v.tstr_val) + "\"";
        }

        applied.fields[applied.count++] = {rf.key, v};

        if (link && link->hasTarget) {
            PendingMirror pm;
            pm.stateId = link->targetStateId;
            pm.fieldIndex = link->targetFieldIndex;
            pm.isString = link->targetIsString;
            if (pm.isString && v.kind == IntentValue::Kind::Tstr) pm.strValue = std::string(v.tstr_val);
            else pm.value = wasNumeric ? physical : 0.0f;
            pm.dueMs = nowMs + rt->echoDelayMs;
            _pending.push_back(std::move(pm));
        }
    }

    logWrite(rt->cfg->name, summary.empty() ? "(no fields)" : summary, nowMs);
    return Result<IntentValueMap, NackCode>::ok(applied);
}

void BenchHub::onSessionJoined(uint32_t session_id) {
    _log.logf('I', "session %u joined", unsigned(session_id));
}

void BenchHub::onSessionLeft(uint32_t session_id) {
    _log.logf('I', "session %u left", unsigned(session_id));
}

// ---- TUI read surface -------------------------------------------------------

std::vector<BenchHub::StateSnapshot> BenchHub::snapshotStates() const {
    std::vector<StateSnapshot> out;
    out.reserve(_states.size());
    for (const auto& sr : _states) {
        StateSnapshot snap;
        snap.id = sr.cfg->id;
        snap.name = sr.cfg->name;
        for (size_t i = 0; i < sr.cfg->fields.size(); ++i) {
            snap.fields.push_back({sr.cfg->fields[i].name, formatFieldValue(sr.cfg->fields[i], sr.values[i], sr.strValues[i])});
        }
        out.push_back(std::move(snap));
    }
    return out;
}

std::vector<BenchHub::WriteLogEntry> BenchHub::recentWrites(size_t n) const {
    std::vector<WriteLogEntry> out;
    size_t start = _writeLog.size() > n ? _writeLog.size() - n : 0;
    for (size_t i = start; i < _writeLog.size(); ++i) out.push_back(_writeLog[i]);
    return out;
}

std::vector<BenchHub::SessionRow> BenchHub::sessionRows() const {
    std::vector<SessionRow> out;
    for (uint8_t i = 0; i < SlopBenchWsPort::kSlots; ++i) {
        auto info = _port.slotInfo(i);
        if (!info.inUse) continue;
        out.push_back({i, true, info.peer});
    }
    return out;
}

}  // namespace slopbench
