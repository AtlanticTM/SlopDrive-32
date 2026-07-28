#pragma once

// BenchHub — the config-driven slopsync::HubDelegate at the heart of
// SlopBench. Builds a live Catalog32 from a parsed HubConfig (config/
// ConfigTypes.h), then implements the generic write plane entirely off that
// catalog's own runtime bookkeeping: no channel id or field name is ever
// hardcoded here (contrast sim/slopsim's MachineSim, which mirrors ONE real
// device's semantics on purpose — this file mirrors WHATEVER the config
// says).
// Constraints:
//   - Single-threaded like every slopsync::Hub owner: tick() is the only
//     entry point that may touch _hub/_catalog/_port; nothing else may call
//     into the hub concurrently (see net/WsServerPort.h's own threading
//     rule, which this class relies on unchanged).
//   - The HubConfig passed to the constructor MUST outlive this object —
//     CatalogEntry/LayoutField/SchemaField hold string_views into it.
//   - No per-field settingKey stored/effective distinction beyond the
//     mirror link below: SlopBench is a test double, not a settings-model
//     reference implementation.
// See: sim/slopbench/README.md (config format + write-plane behavior).

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "slopsync/channel/catalog.hpp"
#include "slopsync/hub/hub.hpp"

#include "common/HostPlatform.h"
#include "common/SessionLog.h"
#include "config/ConfigTypes.h"
#include "net/WsServerPort.h"

namespace slopbench {

class BenchHub final : public slopsync::HubDelegate {
public:
    BenchHub(SessionLog& log, const HubConfig& cfg);

    bool begin(uint16_t port);
    void shutdown();

    // One pump: WS port loop -> hub.update -> animation -> pending-mirror
    // flush (the fake STATE-echo delay, task item 3). Call as fast as the
    // owner's loop likes; there is no motion planner here to starve.
    void tick();

    slopsync::Hub& hub() { return _hub; }
    const HubConfig& config() const { return _cfg; }
    bool catalogOk() const { return _catalog.ok(); }
    size_t catalogEncodedBytes() const { return _hub.catalogEncodedBytes(); }

    // ---- TUI read surface (hub thread only; no locking needed — the TUI
    // runs inline in the same loop as tick(), see main.cpp) -----------------
    struct FieldValue {
        std::string name;
        std::string text;   // already formatted for display
    };
    struct StateSnapshot {
        uint16_t id = 0;
        std::string name;
        std::vector<FieldValue> fields;
    };
    std::vector<StateSnapshot> snapshotStates() const;

    struct WriteLogEntry {
        uint32_t atMs = 0;
        std::string channelName;
        std::string summary;
    };
    std::vector<WriteLogEntry> recentWrites(size_t n) const;

    // Port-slot-indexed, deliberately NOT correlated with the hub's own
    // internal slot array: attachTransport() picks the hub's slot by its own
    // free/evictable-stale policy, which need not match this port's slot
    // index (sim/slopsim's own TUI makes the same simplifying choice — see
    // MachineSim::snapshot()). `sessionCount()` on the hub gives the total.
    struct SessionRow {
        uint8_t slot = 0;
        bool inUse = false;
        std::string peer;
    };
    std::vector<SessionRow> sessionRows() const;
    size_t sessionCount() const { return _hub.sessionCount(); }

    // ---- HubDelegate --------------------------------------------------------
    // SlopBench is a write-plane/catalog test double, not an auth conformance
    // tool: every session is granted `configure` regardless of token, so a
    // test client exercises INTENT clamping/echo/mirroring instead of fighting
    // a pairing ceremony. Judgment call — see README "cut for leanness".
    slopsync::AccessLevel validateToken(std::span<const std::byte> instance_id,
                                        std::span<const std::byte> token, bool hasToken) override;
    slopsync::Result<slopsync::IntentValueMap, slopsync::NackCode> applyIntent(
        uint16_t channel_id, const slopsync::IntentValueMap& requested, slopsync::AccessLevel role,
        bool& cfgChanged) override;
    // No motion to stop: a dumb hub has nothing behind ESTOP but the latch
    // the library already owns.
    void onEstop(uint8_t cause, uint8_t origin) override { (void)cause; (void)origin; }
    void onSessionJoined(uint32_t session_id) override;
    void onSessionLeft(uint32_t session_id) override;

private:
    // ---- Runtime tables (built right after the catalog, from the same cfg) --
    struct StateRuntime {
        const ChannelConfig* cfg = nullptr;
        std::vector<float> values;
        std::vector<std::string> strValues;
        bool dirty = false;
    };
    struct IntentFieldLink {
        uint8_t key = 0;
        bool numeric = false;
        bool hasMin = false, hasMax = false;
        float min = 0.0f, max = 0.0f;
        bool hasTarget = false;
        uint16_t targetStateId = 0;
        size_t targetFieldIndex = 0;
        bool targetIsString = false;
    };
    struct IntentRuntime {
        const ChannelConfig* cfg = nullptr;
        uint32_t echoDelayMs = 0;
        std::vector<IntentFieldLink> fields;
    };
    struct PendingMirror {
        uint16_t stateId = 0;
        size_t fieldIndex = 0;
        bool isString = false;
        float value = 0.0f;
        std::string strValue;
        uint32_t dueMs = 0;
    };

    void buildRuntime();
    StateRuntime* findState(uint16_t id);
    IntentRuntime* findIntent(uint16_t id);
    void publishStateChannel(StateRuntime& sr);
    void tickAnimations(uint32_t nowMs);
    void tickPendingMirrors(uint32_t nowMs);
    void logWrite(const std::string& channelName, const std::string& summary, uint32_t nowMs);

    SessionLog& _log;
    const HubConfig& _cfg;
    HostClock _clock;
    HostRandom _rng;
    // Declaration order is load-bearing: Hub::Hub() encodes the catalog (and
    // computes its etag) ONCE, synchronously, reading `_catalog` by
    // reference at that exact moment. Catalog authoring therefore happens
    // INSIDE `_hub`'s own member-initializer expression (BenchHub.cpp's
    // buildCatalogInto(), called from the constructor's init list) so
    // `_catalog` is fully populated before Hub ever reads it — same idiom
    // sim/slopsim's MachineSim uses for the identical reason. A catalog
    // mutation from the constructor BODY (which runs after every member is
    // already initialized) would be silently invisible to the served bytes.
    slopsync::Catalog32 _catalog;
    slopsync::Hub _hub;
    SlopBenchWsPort _port;

    uint32_t _startMs = 0;

    std::vector<StateRuntime> _states;
    std::vector<IntentRuntime> _intents;
    std::vector<PendingMirror> _pending;

    static constexpr size_t kWriteLogCapacity = 64;
    std::deque<WriteLogEntry> _writeLog;
};

}  // namespace slopbench
