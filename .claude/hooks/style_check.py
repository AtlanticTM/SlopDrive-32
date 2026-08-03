# Style gate body. Reads the hook JSON on stdin, inspects only the text the
# tool call would ADD (new_string / content), exit 2 blocks the write.
import json
import re
import shutil
import subprocess
import sys
import tempfile

SKIP_PATHS = (
    "/.beads/", "/node_modules/", "/managed_components/", "/site/",
    "license", "/vectors/",
    # vendored third-party trees (machine repo); inert in the spec repo
    "/lib/asynctcp/", "/lib/espasyncwebserver/", "/lib/lcd_st7735/",
    "/lib/ruckig/", "/lib/strokeenginepatterns/",
)

# Fallback only. codespell's en-GB dictionary is preferred when installed
# (operator ruling 2026-07-28: no rival hand-rolled wordlists; this short list
# exists so the hook still catches the common cases without codespell).
BRITISH_RX = re.compile(
    r"\b(behaviours?|colours?|favours?|honours?|centres?|metres?|litres?|"
    r"fibres?|licence[sd]?|defence[sd]?|offence[sd]?|catalogue[sd]?|"
    r"analyse[sd]?|analysing|organis(?:e[sd]?|ing|ations?)|"
    r"initialis(?:e[sd]?|ing)|serialis(?:e[sd]?|ing)|synchronis(?:e[sd]?|ing)|"
    r"normalis(?:e[sd]?|ing)|optimis(?:e[sd]?|ing)|minimis(?:e[sd]?|ing)|"
    r"maximis(?:e[sd]?|ing)|greys?|artefacts?|aluminium|whilst|amongst|"
    r"travell(?:ed|ing|ers?)|cancelled|labelled|modelled|signalled|"
    r"programmes?|tyres?|moulds?|judgements?|acknowledgements?)\b",
    re.IGNORECASE,
)

CODE_EXT = {".c", ".cc", ".cpp", ".h", ".hpp", ".js", ".mjs", ".ts", ".py", ".sh"}
HASH_COMMENT_EXT = {".py", ".sh"}


def comment_ratio(path, text):
    ext = "." + path.rsplit(".", 1)[-1] if "." in path else ""
    if ext not in CODE_EXT:
        return None
    lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
    if len(lines) < 8:
        return None
    if ext in HASH_COMMENT_EXT:
        com = sum(1 for ln in lines if ln.startswith("#"))
    else:
        com = sum(1 for ln in lines if ln.startswith(("//", "/*", "*", "*/")))
    return com / len(lines)


def codespell_hits(text):
    exe = shutil.which("codespell")
    if not exe:
        return None
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8") as f:
        f.write(text)
        tmp = f.name
    try:
        out = subprocess.run(
            [exe, "--builtin", "en-GB_to_en-US", tmp],
            capture_output=True, text=True, timeout=15,
        )
        return [ln.rsplit(":", 1)[-1].strip() for ln in out.stdout.splitlines() if "==>" in ln]
    except Exception:
        return None


def main():
    try:
        sys.stdin.reconfigure(encoding="utf-8")  # Windows defaults to cp1252
        data = json.load(sys.stdin)
    except Exception:
        return 0
    ti = data.get("tool_input", {})
    path = (ti.get("file_path") or "").replace("\\", "/").lower()
    texts = [t for t in (ti.get("new_string"), ti.get("content")) if t]
    if not texts or any(s in path for s in SKIP_PATHS):
        return 0
    text = "\n".join(texts)

    errs = []
    if "—" in text:
        errs.append('em dash (U+2014) in new text: use "--", a comma, or restructure')
    hits = codespell_hits(text)
    if hits:
        errs.append("British spellings (codespell en-GB): " + "; ".join(hits[:6]))
    elif hits is None:
        m = sorted(set(w.lower() for w in BRITISH_RX.findall(text)))
        if m:
            errs.append("British spellings: " + ", ".join(m[:8]) + " (C-11: American English)")
    ratio = comment_ratio(path, text)
    if ratio is not None and ratio > 0.5:
        errs.append(
            f"comment-to-code ratio {ratio:.0%} in new code: comments state "
            "constraints, not stories (C-12). Move narration to the docs and link it."
        )
    if errs:
        print("STYLE GATE: " + " | ".join(errs), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
