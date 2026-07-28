// ConfigParser — .bench file parser implementation (see ConfigParser.h for
// the grammar contract).

#include "config/ConfigParser.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace slopbench {
namespace {

// ---- Small string helpers ---------------------------------------------------

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> tokenize(const std::string& line) {
    std::istringstream iss(line);
    std::vector<std::string> out;
    std::string tok;
    while (iss >> tok) out.push_back(tok);
    return out;
}

// Accepts "0x1000" (hex) or "4096" (decimal). *ok is set false on a malformed
// token; the caller decides whether that is fatal.
uint32_t parseUintTok(const std::string& s, bool* ok) {
    if (s.empty()) { *ok = false; return 0; }
    char* end = nullptr;
    unsigned long v = std::strtoul(s.c_str(), &end, s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') ? 16 : 10);
    *ok = (end != nullptr && *end == '\0');
    return uint32_t(v);
}

float parseFloatTok(const std::string& s, bool* ok) {
    if (s.empty()) { *ok = false; return 0.0f; }
    char* end = nullptr;
    float v = std::strtof(s.c_str(), &end);
    *ok = (end != nullptr && *end == '\0');
    return v;
}

const std::map<std::string, LayoutKind>& layoutKindNames() {
    static const std::map<std::string, LayoutKind> m = {
        {"u8", LayoutKind::U8}, {"i8", LayoutKind::I8}, {"u16", LayoutKind::U16},
        {"i16", LayoutKind::I16}, {"u32", LayoutKind::U32}, {"i32", LayoutKind::I32},
        {"f32", LayoutKind::F32}, {"bitfield8", LayoutKind::Bitfield8},
        {"str16", LayoutKind::Str16}, {"str32", LayoutKind::Str32}, {"str64", LayoutKind::Str64},
    };
    return m;
}

const std::map<std::string, SchemaKind>& schemaKindNames() {
    static const std::map<std::string, SchemaKind> m = {
        {"uint", SchemaKind::Uint}, {"int", SchemaKind::Int}, {"f32", SchemaKind::F32},
        {"bool", SchemaKind::Bool}, {"tstr", SchemaKind::Tstr}, {"bstr", SchemaKind::Bstr},
    };
    return m;
}

// ---- Parser state -----------------------------------------------------------

enum class Mode { None, Hub, Channel };

struct ParseState {
    HubConfig* out;
    Mode mode = Mode::None;
    size_t currentChannel = SIZE_MAX;   // index into out->channels
    int lineNo = 0;
    std::string error;

    bool fail(const std::string& msg) {
        std::ostringstream oss;
        oss << path << ":" << lineNo << ": " << msg;
        error = oss.str();
        return false;
    }
    std::string path;
};

bool classFromKeyword(const std::string& kw, ClassKind* out) {
    if (kw == "state") { *out = ClassKind::State; return true; }
    if (kw == "intent") { *out = ClassKind::Intent; return true; }
    if (kw == "event") { *out = ClassKind::Event; return true; }
    if (kw == "stream") { *out = ClassKind::Stream; return true; }
    if (kw == "store") { *out = ClassKind::Store; return true; }
    return false;
}

bool parseFieldLine(ParseState& st, const std::vector<std::string>& tok) {
    ChannelConfig& ch = st.out->channels[st.currentChannel];
    if (ch.cls == ClassKind::Store) return st.fail("store channels declare no fields (use kind/capacity/per_item_max/name_max)");
    if (tok.size() < 4) return st.fail("field needs at least: field <name> <type> <unit>");

    FieldConfig f;
    f.name = tok[1];
    for (const auto& existing : ch.fields) {
        if (existing.name == f.name) return st.fail("duplicate field name '" + f.name + "' in this channel");
    }
    const std::string& typeStr = tok[2];
    f.unit = (tok[3] == "-") ? "" : tok[3];

    const bool isLayoutClass = (ch.cls == ClassKind::State || ch.cls == ClassKind::Stream);
    if (isLayoutClass) {
        auto it = layoutKindNames().find(typeStr);
        if (it == layoutKindNames().end()) return st.fail("unknown STATE/STREAM field type '" + typeStr + "'");
        f.layoutType = it->second;
    } else {
        auto it = schemaKindNames().find(typeStr);
        if (it == schemaKindNames().end()) return st.fail("unknown INTENT/EVENT field type '" + typeStr + "'");
        f.schemaType = it->second;
    }

    for (size_t i = 4; i < tok.size(); ++i) {
        const std::string& t = tok[i];
        auto eq = t.find('=');
        if (eq != std::string::npos) {
            std::string key = t.substr(0, eq), val = t.substr(eq + 1);
            bool ok = true;
            if (key == "key") {
                uint32_t v = parseUintTok(val, &ok);
                if (!ok || v > 0xFF) return st.fail("field key=... must be 0..255");
                f.hasKey = true;
                f.key = uint8_t(v);
            } else if (key == "default") {
                float v = parseFloatTok(val, &ok);
                if (!ok) return st.fail("field default=... is not a number: '" + val + "'");
                f.hasDefault = true;
                f.defaultValue = v;
            } else {
                return st.fail("unknown field attribute '" + key + "='");
            }
        } else {
            bool ok = true;
            float v = parseFloatTok(t, &ok);
            if (!ok) return st.fail("field: unexpected token '" + t + "' (expected min/max number or key=/default=)");
            if (!f.hasMin) { f.hasMin = true; f.min = v; }
            else if (!f.hasMax) { f.hasMax = true; f.max = v; }
            else return st.fail("field: too many bare numbers (min and max already set)");
        }
    }
    if (f.hasMin != f.hasMax) return st.fail("field: min and max must both be given, or neither");

    if (!isLayoutClass && !f.hasKey) return st.fail("INTENT/EVENT field '" + f.name + "' needs key=N (the wire sub-map key)");
    if (isLayoutClass && f.hasKey) return st.fail("field '" + f.name + "': key= only applies to INTENT/EVENT fields");

    ch.fields.push_back(f);
    return true;
}

bool parseAnimateLine(ParseState& st, const std::vector<std::string>& tok) {
    ChannelConfig& ch = st.out->channels[st.currentChannel];
    if (ch.cls != ClassKind::State) return st.fail("animate is only meaningful on a state channel");
    if (tok.size() != 6) return st.fail("animate needs: animate <field> <sine|ramp> <hz> <p2> <p3>");

    FieldConfig* f = nullptr;
    for (auto& existing : ch.fields) if (existing.name == tok[1]) { f = &existing; break; }
    if (!f) return st.fail("animate: no field '" + tok[1] + "' declared yet in this channel (declare it before animating it)");

    AnimKind kind;
    if (tok[2] == "sine") kind = AnimKind::Sine;
    else if (tok[2] == "ramp") kind = AnimKind::Ramp;
    else return st.fail("animate: kind must be 'sine' or 'ramp', got '" + tok[2] + "'");

    bool ok1 = true, ok2 = true, ok3 = true;
    float hz = parseFloatTok(tok[3], &ok1);
    float p2 = parseFloatTok(tok[4], &ok2);
    float p3 = parseFloatTok(tok[5], &ok3);
    if (!ok1 || !ok2 || !ok3) return st.fail("animate: hz/p2/p3 must all be numbers");

    f->anim = kind;
    f->animHz = hz;
    f->animA = p2;
    f->animB = p3;
    return true;
}

bool parseChannelProp(ParseState& st, const std::vector<std::string>& tok) {
    ChannelConfig& ch = st.out->channels[st.currentChannel];
    const std::string& kw = tok[0];
    if (kw == "field") return parseFieldLine(st, tok);
    if (kw == "animate") return parseAnimateLine(st, tok);
    if (tok.size() != 2) return st.fail("'" + kw + "' takes exactly one value");
    const std::string& v = tok[1];
    bool ok = true;

    if (kw == "category") { ch.category = v; return true; }
    if (kw == "rate_hz") { ch.rateHz = parseFloatTok(v, &ok); if (!ok) return st.fail("rate_hz is not a number"); return true; }
    if (kw == "access") {
        if (v == "watch") ch.access = AccessKind::Watch;
        else if (v == "control") ch.access = AccessKind::Control;
        else if (v == "configure") ch.access = AccessKind::Configure;
        else return st.fail("access must be watch|control|configure, got '" + v + "'");
        return true;
    }
    if (kw == "writes") {
        if (ch.cls != ClassKind::Intent) return st.fail("'writes' only applies to an intent channel");
        uint32_t id = parseUintTok(v, &ok);
        if (!ok || id > 0xFFFF) return st.fail("writes: bad channel id '" + v + "'");
        ch.hasWrites = true;
        ch.writesChannelId = uint16_t(id);
        return true;
    }
    if (kw == "echo_delay_ms") {
        if (ch.cls != ClassKind::Intent) return st.fail("'echo_delay_ms' at channel scope only applies to an intent channel");
        uint32_t ms = parseUintTok(v, &ok);
        if (!ok) return st.fail("echo_delay_ms is not an integer");
        ch.hasEchoDelay = true;
        ch.echoDelayMs = ms;
        return true;
    }
    if (kw == "kind") { if (ch.cls != ClassKind::Store) return st.fail("'kind' only applies to a store channel"); ch.storeKind = v; return true; }
    if (kw == "capacity") {
        if (ch.cls != ClassKind::Store) return st.fail("'capacity' only applies to a store channel");
        uint32_t n = parseUintTok(v, &ok);
        if (!ok || n > 0xFFFF) return st.fail("capacity is not a valid u16");
        ch.storeCapacity = uint16_t(n);
        return true;
    }
    if (kw == "per_item_max") {
        if (ch.cls != ClassKind::Store) return st.fail("'per_item_max' only applies to a store channel");
        ch.storePerItemMax = parseUintTok(v, &ok);
        if (!ok) return st.fail("per_item_max is not an integer");
        return true;
    }
    if (kw == "name_max") {
        if (ch.cls != ClassKind::Store) return st.fail("'name_max' only applies to a store channel");
        uint32_t n = parseUintTok(v, &ok);
        if (!ok || n > 0xFFFF) return st.fail("name_max is not a valid u16");
        ch.storeNameMax = uint16_t(n);
        return true;
    }
    return st.fail("unknown channel property '" + kw + "'");
}

bool parseHubProp(ParseState& st, const std::vector<std::string>& tok) {
    const std::string& kw = tok[0];
    if (tok.size() != 2) return st.fail("'" + kw + "' takes exactly one value");
    const std::string& v = tok[1];
    bool ok = true;
    if (kw == "name") { st.out->name = v; return true; }
    if (kw == "fw_version") { st.out->fwVersion = v; return true; }
    if (kw == "product") { st.out->product = v; return true; }
    if (kw == "echo_delay_ms") {
        st.out->echoDelayMs = parseUintTok(v, &ok);
        if (!ok) return st.fail("echo_delay_ms is not an integer");
        return true;
    }
    return st.fail("unknown hub property '" + kw + "'");
}

// ---- Cross-reference validation (runs after the whole file is parsed) -------

bool validate(HubConfig& cfg, std::string& error) {
    std::set<uint16_t> seenIds;
    for (const auto& ch : cfg.channels) {
        if (ch.id < 0x0080) {
            error = "channel '" + ch.name + "' (id 0x" + [&] {
                std::ostringstream o; o << std::hex << ch.id; return o.str();
            }() + ") is in the spec-core range 0x0001-0x007F, reserved by the protocol";
            return false;
        }
        if (!seenIds.insert(ch.id).second) {
            error = "duplicate channel id 0x" + [&] { std::ostringstream o; o << std::hex << ch.id; return o.str(); }();
            return false;
        }
        if ((ch.cls == ClassKind::Intent || ch.cls == ClassKind::Event) && !ch.fields.empty()) {
            uint8_t lastKey = 0;
            bool first = true;
            for (const auto& f : ch.fields) {
                if (!first && f.key <= lastKey) {
                    error = "channel '" + ch.name + "': field keys must be strictly ascending ('" + f.name + "' key=" +
                            std::to_string(f.key) + " does not follow key=" + std::to_string(lastKey) + ")";
                    return false;
                }
                lastKey = f.key;
                first = false;
            }
        }
    }
    for (const auto& ch : cfg.channels) {
        if (ch.cls != ClassKind::Intent || !ch.hasWrites) continue;
        const ChannelConfig* target = cfg.find(ch.writesChannelId);
        if (!target) {
            error = "intent '" + ch.name + "': writes targets unknown channel id 0x" +
                    [&] { std::ostringstream o; o << std::hex << ch.writesChannelId; return o.str(); }();
            return false;
        }
        if (target->cls != ClassKind::State) {
            error = "intent '" + ch.name + "': writes target '" + target->name + "' is not a state channel";
            return false;
        }
    }
    return true;
}

}  // namespace

bool loadConfigFile(const std::string& path, HubConfig& out, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open '" + path + "'";
        return false;
    }

    out = HubConfig{};
    out.channels.clear();

    ParseState st;
    st.out = &out;
    st.path = path;

    std::string rawLine;
    while (std::getline(in, rawLine)) {
        ++st.lineNo;
        std::string line = rawLine;
        auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;

        std::vector<std::string> tok = tokenize(line);
        const std::string& kw = tok[0];

        ClassKind cls;
        if (kw == "hub") {
            st.mode = Mode::Hub;
            continue;
        }
        if (classFromKeyword(kw, &cls)) {
            if (tok.size() != 3) { error = "line " + std::to_string(st.lineNo) + ": '" + kw + "' needs: " + kw + " <id> <name>"; return false; }
            bool ok = true;
            uint32_t id = parseUintTok(tok[1], &ok);
            if (!ok || id > 0xFFFF) { error = "line " + std::to_string(st.lineNo) + ": bad channel id '" + tok[1] + "'"; return false; }
            ChannelConfig ch;
            ch.id = uint16_t(id);
            ch.name = tok[2];
            ch.cls = cls;
            out.channels.push_back(ch);
            st.mode = Mode::Channel;
            st.currentChannel = out.channels.size() - 1;
            continue;
        }

        bool ok;
        if (st.mode == Mode::Hub) ok = parseHubProp(st, tok);
        else if (st.mode == Mode::Channel) ok = parseChannelProp(st, tok);
        else { error = "line " + std::to_string(st.lineNo) + ": '" + kw + "' outside any hub/state/intent/event/stream/store block"; return false; }

        if (!ok) { error = st.error; return false; }
    }

    return validate(out, error);
}

}  // namespace slopbench
