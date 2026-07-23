// slopsync_demo — a narrated end-to-end SlopSync session, desktop-only.
// Everything below runs the REAL library: real Hub, real Clients, real wire
// bytes over the in-process binding with real injected packet loss. The only
// theater is the printf. Build & run (from repo root):
//   g++ -std=gnu++2b -O2 -I lib/slopsync/include examples/slopsync_demo/demo.cpp -o slopsync_demo && ./slopsync_demo
#include <cstdio>
#include <cstring>
#include <optional>

#include "slopsync/slopsync.h"
#include "slopsync/conformance/mini_catalog.hpp"
#include "slopsync/conformance/catalog_check.hpp"

using namespace slopsync;

static ManualClock g_clock;
static uint32_t nowMs() { return g_clock.nowUs() / 1000u; }
static void say(const char* who, const char* msg) {
    std::printf("  [%6u ms] %-8s %s\n", nowMs(), who, msg);
}
static void scene(const char* title) { std::printf("\n== %s ==\n", title); }

// ---------------------------------------------------------------------------
// The "machine": what the firmware's delegate will be, in miniature.
// ---------------------------------------------------------------------------
struct Machine final : HubDelegate {
    float speed = 300.0f;
    bool patternRunning = false;
    bool motionStopped = false;

    Result<IntentValueMap, NackCode> applyIntent(uint16_t ch, const IntentValueMap& req,
                                                 AccessLevel, bool& cfgChanged) override {
        IntentValueMap applied;
        if (ch == 0x0084) {
            for (uint32_t i = 0; i < req.count; ++i) {
                if (req.fields[i].key == 1 && req.fields[i].value.kind == IntentValue::Kind::F32) {
                    float want = req.fields[i].value.f32_val;
                    speed = want < 0.f ? 0.f : (want > 400.f ? 400.f : want);  // the ceiling clamp
                    char line[96];
                    std::snprintf(line, sizeof line, "MACHINE: speed request %.0f -> APPLIED %.0f (ceiling 400)", double(want), double(speed));
                    say("hub", line);
                    patternRunning = true;
                    motionStopped = false;
                    applied.fields[applied.count++] = {1, IntentValue::ofF32(speed)};
                }
            }
            cfgChanged = true;
            return Result<IntentValueMap, NackCode>::ok(applied);
        }
        return Result<IntentValueMap, NackCode>::err(NackCode::UNSUPPORTED_OP);
    }
    void onEstop(uint8_t cause, uint8_t) override {
        motionStopped = true;
        patternRunning = false;
        say("hub", cause == 0 ? "MACHINE: !! ESTOP — MOTOR HALTED (user) !!" : "MACHINE: !! ESTOP — MOTOR HALTED !!");
    }
    std::optional<uint8_t> sourceForChannel(uint16_t ch) override {
        return ch == 0x0084 ? std::optional<uint8_t>(1) : std::nullopt;
    }
    SourceLossPolicy sourcePolicy(uint8_t) override { return SourceLossPolicy::Continue; }  // pattern-continues!
    void onSourceOwnership(uint8_t src, uint32_t owner, uint8_t reason) override {
        char line[96];
        const char* why = reason == 1 ? "TAKEOVER" : reason == 0 ? "acquired" : "released";
        std::snprintf(line, sizeof line, "MACHINE: control source %u %s (session %08X)%s",
                      src, why, owner, (owner == 0 && patternRunning) ? " — pattern keeps running" : "");
        say("hub", line);
    }
    bool canClearEstop() override { return true; }  // demo machine reports velocity zero
};

// ---------------------------------------------------------------------------
// A narrating client delegate.
// ---------------------------------------------------------------------------
struct Watcher final : ClientDelegate {
    const char* name;
    std::array<std::byte, 16> token{};
    bool hasToken = false;
    explicit Watcher(const char* n) : name(n) {}

    void onStateChange(ClientSessionState s) override {
        if (s == ClientSessionState::LIVE) say(name, "-> LIVE (all retained state adopted; ground truth on screen)");
        else if (s == ClientSessionState::SYNCING) say(name, "-> SYNCING (adopting device state...)");
    }
    void onState(uint16_t ch, uint16_t, std::span<const std::byte> p) override {
        if (ch == 0x0003 && !p.empty()) {
            uint8_t word = uint8_t(p[0]);
            if (word & safety_bits::ESTOP) say(name, "sees SAFETY: ESTOP LATCHED (red banner)");
            else if (word & safety_bits::STOP) say(name, "sees SAFETY: STOP");
            else say(name, "sees SAFETY: clear");
        }
    }
    void onEcho(uint16_t, const IntentValueMap& applied, uint16_t cfgGen) override {
        for (uint32_t i = 0; i < applied.count; ++i)
            if (applied.fields[i].key == 1) {
                char line[96];
                std::snprintf(line, sizeof line, "got ECHO: applied %.0f (cfg_gen %u) — UI shows the TRUTH, not the wish",
                              double(applied.fields[i].value.f32_val), cfgGen);
                say(name, line);
            }
    }
    void onNack(const NackMsg& n) override {
        char line[64];
        std::snprintf(line, sizeof line, "got NACK 0x%04X", uint16_t(n.code));
        say(name, line);
    }
    void onPendingDropped(uint16_t id) override {
        char line[80];
        std::snprintf(line, sizeof line, "pending intent #%u died with the session (will reconcile, never blind-resend)", id);
        say(name, line);
    }
    void onPairGrant(std::span<const std::byte> t, AccessLevel) override {
        std::memcpy(token.data(), t.data(), token.size());
        hasToken = true;
        say(name, "PAIRED! 16-byte controller token issued and stored");
    }
};

int main() {
    std::printf("SlopSync live demo — real Hub, real Clients, real bytes, injected chaos\n");

    // Catalog = the frozen conformance fixture + safety-intents; checked first.
    auto cat = conformance::miniCatalog();
    cat.entries[cat.count] = cat.entries[3];  // clone an INTENT entry shape
    cat.entries[cat.count].id = channels::safety_intents;  // 0x0005... must stay ascending!
    // 0x0005 sorts between 0x0003 and 0x0080: rebuild ascending
    for (uint16_t i = cat.count; i > 1; --i) cat.entries[i] = cat.entries[i - 1];
    cat.entries[1] = cat.entries[2];  // scratch — simpler: construct clean below
    cat = conformance::miniCatalog();
    {   // insert 0x0005 after 0x0003 (index 0), shifting the rest
        for (uint16_t i = cat.count; i > 1; --i) cat.entries[i] = cat.entries[i - 1];
        CatalogEntry& e = cat.entries[1];
        e = CatalogEntry{};
        e.id = channels::safety_intents; e.name = "safety-intents";
        e.cls = ChannelClass::INTENT; e.dir = Direction::c2h;
        e.access = AccessLevel::controller; e.maxRateHz = 10.0f;
        e.defaultPriority = Priority::critical;
        e.fieldCount = 1;
        e.schema[0] = {.key = 1, .name = "op", .type = CborFieldType::uint_t, .unit = ""};
        cat.count++;
    }
    auto conf = conformance::checkCatalog(cat);
    std::printf("catalog conformance: %s (%u violations)\n", conf.ok() ? "CLEAN" : "VIOLATIONS", unsigned(conf.count));

    XorShift32 rng(0xB007CAFE);
    Machine machine;
    Hub hub(cat, g_clock, rng, machine);

    InProcessLink linkUi(g_clock, rng), linkPhone(g_clock, rng), linkRemote(g_clock, rng);
    hub.attachTransport(linkUi.endpointA());
    hub.attachTransport(linkPhone.endpointA());
    hub.attachTransport(linkRemote.endpointA());

    // Machine boots with state already published (retained store = the shadow).
    std::array<std::byte, 8> safety0{};  // word=0,cause=0,owner=0,seq=0
    hub.publishState(0x0003, safety0);
    std::array<std::byte, 2> motion0{std::byte{0x01}, std::byte{0}};  // homed
    hub.publishState(0x0082, motion0);

    auto ident = [](const char* kind, uint8_t seed) {
        ClientIdentity id;
        for (size_t i = 0; i < id.instance_id.size(); ++i) id.instance_id[i] = std::byte(uint8_t(seed + i));
        id.client_kind = kind; id.client_name = kind;
        return id;
    };

    Watcher uiW("webui"), phoneW("phone"), remoteW("remote");
    Client ui(ident("webui", 0x10), linkUi.endpointB(), g_clock, rng, uiW);
    Client phone(ident("phone", 0x20), linkPhone.endpointB(), g_clock, rng, phoneW);
    Client remote(ident("remote", 0x30), linkRemote.endpointB(), g_clock, rng, remoteW);
    for (Client* c : {&ui, &phone, &remote}) {
        c->addSubscriptionWish(0x0003, 0.0f, Priority::critical);
        c->addSubscriptionWish(0x0082, 10.0f, Priority::normal);
    }

    bool phoneAlive = true;
    std::optional<Client> phone2, remote2;
    auto pump = [&](uint32_t ms) {
        for (uint32_t i = 0; i < ms; ++i) {
            g_clock.advanceUs(1000);
            hub.update(g_clock.nowUs());
            ui.update(g_clock.nowUs());
            if (phoneAlive) (phone2 ? phone2->update(g_clock.nowUs()) : phone.update(g_clock.nowUs()));
            (remote2 ? remote2->update(g_clock.nowUs()) : remote.update(g_clock.nowUs()));
        }
    };

    scene("SCENE 1 — the WebUI connects (viewer, no pairing needed)");
    ui.connect();
    pump(50);

    scene("SCENE 2 — pairing: hub shows PIN 4821, phone proves it (HMAC, never sends the PIN)");
    const char pin[] = {'4', '8', '2', '1'};
    hub.openPairingWindow(std::span<const char>(pin, 4));
    phone.connect();
    pump(50);
    { auto proof = pairingPinProof(std::span<const char>(pin, 4), phone.nonce());
      phone.sendPairReq(proof); }
    pump(50);
    say("phone", "reconnecting with the token to claim controller role...");
    phone.disconnect(); pump(10);
    { ClientIdentity id2 = ident("phone", 0x20);
      std::memcpy(id2.token.data(), phoneW.token.data(), id2.token.size());
      id2.hasToken = true;
      // fresh client object, same durable identity + token (a "reboot")
      phone2.emplace(id2, linkPhone.endpointB(), g_clock, rng, phoneW);
      phone2->addSubscriptionWish(0x0003, 0.0f, Priority::critical);
      phone2->connect();
      pump(50);
      scene("SCENE 3 — the phone asks for 420; the machine's ceiling says otherwise");
      IntentValueMap v; v.count = 1; v.fields[0] = {1, IntentValue::ofF32(420.0f)};
      phone2->sendIntent(0x0084, v);
      pump(50);

      scene("SCENE 4 — phone dies mid-session; the PATTERN KEEPS RUNNING (hub-autonomous)");
      say("phone", "*battery dies* (transport goes silent)");
      phoneAlive = false;
      // stop pumping phone2 as well: it shares linkPhone; hub-side deadman fires
      pump(800);
      std::printf("        machine.patternRunning = %s  <-- the product decision, live\n",
                  machine.patternRunning ? "TRUE (still stroking, nobody interrupted)" : "false");

      scene("SCENE 5 — the remote takes over (paired earlier off-screen) and then... ESTOP through 25% packet loss");
      hub.openPairingWindow(std::span<const char>(pin, 4));
      remote.connect();
      pump(60);
      auto proofR = pairingPinProof(std::span<const char>(pin, 4), remote.nonce());
      remote.sendPairReq(proofR);
      pump(60);
      say("remote", "reconnecting with controller token...");
      remote.disconnect(); pump(10);
      ClientIdentity idR = ident("remote", 0x30);
      std::memcpy(idR.token.data(), remoteW.token.data(), idR.token.size());
      idR.hasToken = true;
      remote2.emplace(idR, linkRemote.endpointB(), g_clock, rng, remoteW);
      remote2->addSubscriptionWish(0x0003, 0.0f, Priority::critical);
      remote2->connect();
      pump(80);
      IntentValueMap v2; v2.count = 1; v2.fields[0] = {1, IntentValue::ofF32(250.0f)};
      remote2->sendIntent(0x0084, v2, std::nullopt, /*takeover=*/true);
      pump(60);

      say("remote", "...and now the WiFi turns cursed: 25% of ALL packets start dying");
      linkRemote.profileA().loss_pct = 25;
      linkRemote.profileB().loss_pct = 25;
      say("remote", "USER SLAMS THE E-STOP (through the packet loss)");
      remote2->initiateEstop(0 /*user*/);
      pump(400);
      std::printf("        hub.estopLatched=%s  motionStopped=%s  remote observed latch=%s\n",
                  hub.estopLatched() ? "true" : "false",
                  machine.motionStopped ? "true" : "false",
                  (hub.estopLatched() && !remote2->estopSendFailed())
                      ? "true (repeat-until-latch beat the loss)" : "NOT YET");

      scene("SCENE 6 — clear the latch (authorized, motion at rest) and say goodnight");
      hub.clearEstop();
      pump(60);
    }

    std::printf("\nfin. Every line above was real protocol traffic. Sleep well. :3\n");
    return 0;
}
