// RecordingStore — see RecordingStore.h for the two artifact kinds and the
// forward-compatibility contract.

#include "machine/RecordingStore.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>   // GetModuleFileNameA — the exe-relative shelf default
#endif

#include "machine/MotionCore.h"

namespace slopsim {

namespace fs = std::filesystem;

namespace {
constexpr const char* kDefaultDir = "slopsim-recordings";
constexpr const char* kRunSuffix  = ".run.csv";
constexpr const char* kRecSuffix  = ".csv";

// Counts data rows without holding the file: a run of an hour is ~3.6 M rows and
// the picker only wants the number.
//
// MEMOIZED, keyed on (path, size, mtime), because the analyzer POLLS the shelf
// while it is open — see the picker's own note. Without the memo every poll
// re-read every run file end to end (a modest 10 k-sample run is ~470 KB), which
// is a silly amount of disk to spend on a number that only changes when the file
// does. The mutex is here because cpp-httplib serves from a thread pool and
// list() is const, so two requests can land in this cache at once.
std::mutex g_rowsM;
std::map<std::string, std::pair<std::pair<uintmax_t, fs::file_time_type>, uint32_t>> g_rows;

uint32_t countRowsUncached(const fs::path& p) {
    std::FILE* f = std::fopen(p.string().c_str(), "rb");
    if (!f) return 0;
    uint32_t rows = 0;
    char buf[16384];
    size_t n;
    bool atLineStart = true, skipping = false;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < n; ++i) {
            if (atLineStart) {
                // '#' is a settings line, 't' opens the `t_s,...` column header.
                skipping = (buf[i] == '#' || buf[i] == 't' || buf[i] == '\n' || buf[i] == '\r');
                atLineStart = false;
            }
            if (buf[i] == '\n') {
                if (!skipping) ++rows;
                atLineStart = true;
            }
        }
    }
    std::fclose(f);
    return rows;
}

uint32_t countRows(const fs::path& p) {
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    const auto mtime = fs::last_write_time(p, ec);
    if (ec) return countRowsUncached(p);   // can't key it; just do the work
    const std::string key = p.string();
    {
        std::lock_guard<std::mutex> lk(g_rowsM);
        auto it = g_rows.find(key);
        if (it != g_rows.end() && it->second.first.first == size &&
            it->second.first.second == mtime)
            return it->second.second;
    }
    const uint32_t rows = countRowsUncached(p);
    {
        std::lock_guard<std::mutex> lk(g_rowsM);
        g_rows[key] = {{size, mtime}, rows};
    }
    return rows;
}
}  // namespace

namespace {

// Can we actually create this directory AND put a file in it? Asked properly,
// with a real write, because the interesting failures (an unwritable system
// directory, a read-only volume) all let create_directories succeed or report
// "already exists" and only bite at fopen time.
bool probeWritable(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec)) return false;
    const fs::path probe = dir / ".slopsim-write-probe";
    std::FILE* f = std::fopen(probe.string().c_str(), "wb");
    if (!f) return false;
    std::fclose(f);
    fs::remove(probe, ec);
    return true;
}

// Directory holding the running executable. This is the DEFAULT shelf's home,
// and deliberately not the working directory: slopsim is launched from a shim
// that does not cd, and from a shortcut or an elevated shell the working
// directory is C:\Windows\System32 — which is unwritable, so every save failed
// while the picker still offered the name back. An exe-relative shelf follows
// the "standalone exe, copy it anywhere" property the rest of this tool has.
fs::path exeDir() {
    std::error_code ec;
#ifdef _WIN32
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return fs::path(std::string(buf, n)).parent_path();
#else
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return self.parent_path();
#endif
    return fs::current_path(ec);
}

// Last resort when even the exe's own directory is read-only (an exe dropped in
// Program Files). Per-user, always writable, and named so it is findable.
fs::path userFallbackDir() {
    const char* base =
#ifdef _WIN32
        std::getenv("LOCALAPPDATA");
#else
        std::getenv("HOME");
#endif
    if (base && *base) return fs::path(base) / "slopsim" / kDefaultDir;
    std::error_code ec;
    return fs::current_path(ec) / kDefaultDir;
}

}  // namespace

RecordingStore::RecordingStore(std::string dir) {
    // RESOLUTION ORDER:
    //   1. --recordings <dir>, if given. An explicit choice is never overridden —
    //      if it is unwritable the operator is told, not silently relocated.
    //   2. %LOCALAPPDATA%/slopsim/slopsim-recordings (or $HOME/...).
    //   3. <exe dir>/slopsim-recordings, if the environment gave us nothing.
    //
    // THE WORKING DIRECTORY IS NOT IN THAT LIST, and that is the whole point. It
    // used to be the default, and slopsim is launched from a shim that does not
    // cd — so from a shortcut, the Run dialog or an elevated shell the shelf
    // resolved to C:\Windows\System32\slopsim-recordings, which is unwritable.
    // Every save then failed while the picker still offered the name back.
    //
    // THE PER-USER DIRECTORY BEATS THE EXE'S OWN, for one reason: the exe lives
    // in build/, and these files are REGRESSION FIXTURES whose entire value is
    // still being there in a month. A clean rebuild must not delete them. The
    // cost is that they are not sitting next to the binary, which is paid off by
    // printing the resolved path at boot, in the analyzer's picker, and in every
    // rec.* palette reply.
    std::error_code ec;
    if (!dir.empty()) {
        fs::path abs = fs::absolute(fs::path(dir), ec);
        _dir = (ec ? fs::path(dir) : abs).string();
        _writable = probeWritable(_dir);
        return;
    }
    const fs::path prim = userFallbackDir();
    if (probeWritable(prim)) {
        _dir = prim.string();
        _writable = true;
        return;
    }
    const fs::path alt = exeDir() / kDefaultDir;
    _dir = alt.string();
    _writable = probeWritable(alt);
}

std::string RecordingStore::sanitizeName(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        // Anything else — separators, drive colons, spaces, control bytes,
        // non-ASCII — collapses to '-'. Whitelist, never a blacklist of the
        // traversal spellings someone thought of.
        out.push_back(ok ? c : '-');
    }
    // Leading dots would allow ".." and hidden files; trailing dots/spaces are a
    // Windows path hazard on their own.
    size_t b = 0;
    while (b < out.size() && (out[b] == '.' || out[b] == '-')) ++b;
    size_t e = out.size();
    while (e > b && (out[e - 1] == '.' || out[e - 1] == ' ')) --e;
    out = out.substr(b, e - b);
    // Strip the suffixes the caller re-adds, so "take3.csv" and "take3" are the
    // same shelf entry instead of yielding "take3.csv.csv".
    for (const char* suf : {kRunSuffix, kRecSuffix}) {
        const size_t sl = std::char_traits<char>::length(suf);
        if (out.size() > sl && out.compare(out.size() - sl, sl, suf) == 0) {
            out.resize(out.size() - sl);
            break;
        }
    }
    if (out.size() > 96) out.resize(96);
    return out;
}

std::string RecordingStore::pathFor(const std::string& name, bool is_run) const {
    const std::string safe = sanitizeName(name);
    if (safe.empty()) return {};
    return (fs::path(_dir) / (safe + (is_run ? kRunSuffix : kRecSuffix))).string();
}

std::vector<StoredArtifact> RecordingStore::list() const {
    std::vector<StoredArtifact> out;
    std::error_code ec;
    fs::directory_iterator it(_dir, ec), end;
    if (ec) return out;
    std::vector<std::pair<fs::file_time_type, StoredArtifact>> stamped;
    // PER-QUERY error codes, never one shared across the loop. The
    // std::filesystem overloads only ever SET an error_code, never clear it, so
    // sharing `ec` between the per-entry queries and the iterator's own
    // increment meant a single file with an unreadable timestamp poisoned the
    // `if (ec)` guard on the NEXT iteration and silently truncated the shelf —
    // a listing that is short but looks complete, which is precisely how a
    // recording that saved correctly appears to have vanished.
    for (; it != end; it.increment(ec)) {
        if (ec) break;                       // the iterator itself gave up
        std::error_code fec;
        if (!it->is_regular_file(fec) || fec) continue;
        const std::string file = it->path().filename().string();
        StoredArtifact a;
        const size_t rl = std::char_traits<char>::length(kRunSuffix);
        if (file.size() > rl && file.compare(file.size() - rl, rl, kRunSuffix) == 0) {
            a.is_run = true;
            a.name = file.substr(0, file.size() - rl);
        } else if (file.size() > 4 && file.compare(file.size() - 4, 4, kRecSuffix) == 0) {
            a.name = file.substr(0, file.size() - 4);
        } else {
            continue;
        }
        a.file = file;
        a.bytes = uint64_t(it->file_size(fec));
        a.rows = countRows(it->path());
        // A file whose timestamp will not read still LISTS — it just sorts as
        // oldest. Dropping it would be the same silent truncation in a smaller
        // costume.
        const auto stamp = it->last_write_time(fec);
        stamped.emplace_back(fec ? fs::file_time_type::min() : stamp, std::move(a));
    }
    std::sort(stamped.begin(), stamped.end(),
              [](const auto& l, const auto& r) { return l.first > r.first; });
    out.reserve(stamped.size());
    for (auto& s : stamped) out.push_back(std::move(s.second));
    return out;
}

size_t RecordingStore::write(const std::string& name, bool is_run, const std::string& header,
                             const std::vector<std::string>& lines) const {
    const std::string path = pathFor(name, is_run);
    if (path.empty()) return 0;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return 0;
    if (!header.empty()) std::fputs(header.c_str(), f);
    for (const auto& l : lines) std::fputs(l.c_str(), f);
    std::fclose(f);
    return lines.size();
}

// ---- RunSettings ------------------------------------------------------------

void RunSettings::set(const std::string& k, const std::string& v) {
    for (auto& p : kv) {
        if (p.first == k) {
            p.second = v;
            return;
        }
    }
    kv.emplace_back(k, v);
}

void RunSettings::set(const std::string& k, double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    set(k, std::string(buf));
}

const std::string* RunSettings::find(const std::string& k) const {
    for (const auto& p : kv)
        if (p.first == k) return &p.second;
    return nullptr;
}

// ---- Run files --------------------------------------------------------------

size_t writeRun(const RecordingStore& store, const std::string& name, const RunSettings& settings,
                const std::vector<float>& trace) {
    const std::string path = store.pathFor(name, true);
    if (path.empty()) return 0;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return 0;
    std::fprintf(f, "%s\n", kRunMagic);
    for (const auto& p : settings.kv) std::fprintf(f, "# %s=%s\n", p.first.c_str(), p.second.c_str());
    // APPEND ONLY, and the last two columns are why this list is not shorter:
    // plan_kind and eng_vel_norm are the two columns that say WHICH planner drew
    // a stroke and what velocity it planned, and a run written without them
    // recalls a trace that cannot be diagnosed — only looked at. A frozen run is
    // the thing an operator comes back to weeks later, so it has to carry the
    // whole record. readRun tolerates the shorter legacy layout.
    std::fputs("t_s,pos_mm,tgt_mm,vel_mm_s,cmd_norm,raw_norm,plan_kind,eng_vel_norm\n", f);
    const size_t rows = trace.size() / kTraceStride;
    for (size_t i = 0; i < rows; ++i) {
        const float* r = &trace[i * kTraceStride];
        // Same precision the live CSV export uses, so a run and an /export of
        // the same motion are comparable line for line.
        std::fprintf(f, "%.3f,%.3f,%.3f,%.2f,%.4f,%.4f,%d,%.4f\n", double(r[0]), double(r[1]),
                     double(r[2]), double(r[3]), double(r[4]), double(r[5]),
                     int(r[6]), double(r[7]));
    }
    std::fclose(f);
    return rows;
}

bool readRun(const RecordingStore& store, const std::string& name, RunSettings& settings,
             std::vector<float>& trace, std::string& err) {
    const std::string path = store.pathFor(name, true);
    if (path.empty()) {
        err = "bad run name";
        return false;
    }
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    settings = RunSettings{};
    trace.clear();
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] == '#') {
            // Every `# key=value` is kept VERBATIM, including keys this build
            // has never heard of — a run saved by a newer slopsim still opens
            // here, showing settings this one cannot set. That is the forward
            // compatibility contract, and it costs exactly this branch.
            std::string s(line + 1);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            const size_t sp = s.find_first_not_of(' ');
            if (sp == std::string::npos) continue;
            s = s.substr(sp);
            const size_t eq = s.find('=');
            if (eq == std::string::npos) continue;   // the magic line
            settings.set(s.substr(0, eq), s.substr(eq + 1));
            continue;
        }
        if (line[0] == 't' || line[0] == '\n' || line[0] == '\r' || line[0] == '\0') continue;
        // Six columns are the LEGACY layout and still load — the two appended
        // ones simply stay 0, which reads as plan_kind `idle`. A run written
        // before they existed is therefore recalled, not rejected; it just
        // cannot answer which planner drew a stroke.
        float r[kTraceStride] = {};
        const int got = std::sscanf(line, "%f,%f,%f,%f,%f,%f,%f,%f", &r[0], &r[1], &r[2], &r[3],
                                    &r[4], &r[5], &r[6], &r[7]);
        if (got != 6 && got != int(kTraceStride)) continue;
        for (float v : r) trace.push_back(v);
    }
    std::fclose(f);
    if (trace.empty()) {
        err = "run has no samples";
        return false;
    }
    return true;
}

}  // namespace slopsim
