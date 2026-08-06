#pragma once

// RecordingStore — the on-disk shelf the async-tune mode keeps its work on.
// Constraints:
//   TWO ARTIFACT KINDS, and the difference between them is the whole point:
//
//   RECORDING (`<name>.csv`) — a WIRE LOG. The 0x0084/0x0085 commands exactly as
//     they arrived, in MachineSim::ingressCsvHeader()'s schema (so it is also
//     just what GET /api/segments.csv serves, and diffs against the funscript
//     that was supposed to be sent). It is INPUT: replaying it re-runs the
//     engine, so it renders under whatever the engine does TODAY.
//
//   RUN (`<name>.run.csv`) — a FROZEN RESULT: the settings a replay was run
//     under, plus the samples it produced. It is OUTPUT, and recalling it draws
//     the stored points with NO ENGINE INVOLVED.
//
//   Why a run stores its samples instead of re-deriving them from its settings:
//   a recomputed baseline is not a baseline. If recall re-ran the engine, then
//   any later change to slopmotion, the arbiter or a default would silently move
//   the reference line, and the regression check would pass by construction
//   forever. Freezing the points is what makes "verify this has not regressed"
//   a real question with a real answer.
//
//   FORWARD COMPATIBILITY IS A HARD REQUIREMENT ("adding more settings should
//   not break previous runs"). A run's settings ride as `# key=value` comment
//   lines above the sample CSV, read into a plain string map:
//     * a key this build has never heard of        -> kept and displayed
//     * a key the run predates                     -> reported ABSENT
//   Absent is deliberately NOT backfilled with today's default. Backfilling
//   would make an old run claim it was taken under a setting that did not exist
//   when it was taken, which is the same class of lie as a recomputed baseline.
//   There is no schema version to bump and nothing to migrate.
//
//   TRUST BOUNDARY: names arrive from URL query parameters. sanitizeName() is
//   the only thing standing between a query string and the filesystem — it is
//   whitelist-based on purpose, and every write path in the facade goes through
//   it. Do not add a second way to build a path in this directory.

#include <cstdint>
#include <string>
#include <vector>

namespace slopsim {

struct StoredArtifact {
    std::string name;         // as shown in the picker (no directory, no suffix)
    std::string file;         // basename on disk
    bool     is_run = false;  // false = recording (wire log), true = run (result)
    uint64_t bytes = 0;
    uint32_t rows = 0;        // data rows, header/comments excluded
};

class RecordingStore {
public:
    // `dir` is created on demand. Empty = the default `slopsim-recordings` in
    // the working directory.
    explicit RecordingStore(std::string dir = {});

    const std::string& dir() const { return _dir; }
    // False when the resolved shelf cannot be written to. Checked with a real
    // probe file at construction, because the failure this exists for (launching
    // from C:\Windows\System32, where the working directory is unwritable) lets
    // directory creation appear to succeed and only bites at fopen time — which
    // used to surface as a SAVE THAT REPORTED SUCCESS and a replay that could
    // not find its own file.
    bool writable() const { return _writable; }

    // Whitelist to [A-Za-z0-9._-], collapse the rest to '-', strip leading dots
    // and any embedded path separator. Returns empty for a name that survives as
    // nothing, which every caller MUST treat as a refusal — that is the
    // traversal guard, not a formatting nicety.
    static std::string sanitizeName(const std::string& raw);

    // Newest first. Never throws; an unreadable directory lists empty.
    std::vector<StoredArtifact> list() const;

    // Full path for a sanitized name. Empty if the name does not sanitize.
    std::string pathFor(const std::string& name, bool is_run) const;

    // Writes `header` then every line in `lines` (each expected to end in \n).
    // Returns rows written, 0 on failure.
    size_t write(const std::string& name, bool is_run, const std::string& header,
                 const std::vector<std::string>& lines) const;

private:
    std::string _dir;
    bool _writable = false;
};

// ---- Run files --------------------------------------------------------------
// The `# key=value` settings block above a run's samples. Order is preserved so
// a saved run reads in the order the panel presents its controls.
struct RunSettings {
    std::vector<std::pair<std::string, std::string>> kv;

    void set(const std::string& k, const std::string& v);
    void set(const std::string& k, double v, int decimals = 4);
    // Empty when absent. Callers that need to tell "absent" from "empty string"
    // use has().
    const std::string* find(const std::string& k) const;
    bool has(const std::string& k) const { return find(k) != nullptr; }
};

inline constexpr const char* kRunMagic = "# slopsim-run 1";

// Serializes the settings block + the sample header + interleaved f32 samples
// (kTraceStride floats per row, the same layout as /api/trace.bin's body).
size_t writeRun(const RecordingStore& store, const std::string& name,
                const RunSettings& settings, const std::vector<float>& trace);

// Reads a run back. `settings` collects every `# key=value` line VERBATIM,
// including keys this build does not know. `trace` is the interleaved samples.
bool readRun(const RecordingStore& store, const std::string& name, RunSettings& settings,
             std::vector<float>& trace, std::string& err);

}  // namespace slopsim
