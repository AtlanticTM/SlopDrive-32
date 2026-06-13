"""
build_webui.py — PlatformIO pre-build script that bridges C++ build flags
to Vite environment variables, then runs the Vite build pipeline.

Flow:
  1. Parse C++ build_flags from the active PlatformIO environment
  2. Map known C++ defines to VITE_* env vars
  3. Write webui/.env.local
  4. npm install (skipped if node_modules/ exists)
  5. npm run build → outputs to webui/dist/index.html
  6. Gzip webui/dist/index.html → data/index.html.gz
  7. Cleans up stale data/index.html if present

On failure (npm install or build errors), the entire PlatformIO build
aborts — you can't accidentally flash stale or broken web assets.

This is the compile-time bridge between the firmware's feature flags
and the web UI's tree-shaken production bundle. :3
"""

import gzip
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

Import("env")  # noqa: F821 — PlatformIO SCons injection

# ---- 1. Parse build_flags from the active environment ----
# PlatformIO may pass them as a single space-joined string in a single-element
# list. Split into individual tokens so -DFOO matches work correctly. :3
raw_flags = env.get("BUILD_FLAGS", [])
if isinstance(raw_flags, str):
    raw_flags = [raw_flags]

build_flags = []
for item in raw_flags:
    for token in item.split():
        build_flags.append(token)

print(f"[build_webui] Parsed build_flags: {build_flags}")

# ---- 2. Map C++ defines → Vite env vars ----
FLAG_MAP = {
    "DRIVER_TMC2160": "VITE_DRIVER_TMC2160",
    "BLE_ENABLED":    "VITE_BLE_ENABLED",
    # Extensible: add new flags here as the project grows.
}

# Default values (feature off unless explicitly enabled)
env_vars = {v: "false" for v in FLAG_MAP.values()}

for flag in build_flags:
    # Match -DFOO or -DFOO=BAR
    m = re.match(r"-D(\w+)(?:=(.+))?", flag)
    if not m:
        continue
    key = m.group(1)
    val = m.group(2) if m.group(2) else "true"
    if key in FLAG_MAP:
        env_vars[FLAG_MAP[key]] = val
        print(f"[build_webui]   {FLAG_MAP[key]} = {val}")

# ---- 3. Write webui/.env.local ----
# SCons exec's this script, so __file__ is not available — use cwd which
# PlatformIO always sets to the project root. :3
script_dir = Path.cwd()
webui_dir = script_dir / "webui"
webui_dir.mkdir(parents=True, exist_ok=True)

env_local_path = webui_dir / ".env.local"
with open(env_local_path, "w") as f:
    for k, v in env_vars.items():
        f.write(f"{k}={v}\n")
print(f"[build_webui] Wrote {env_local_path}: {dict(env_vars)}")

# ---- 4. npm install (skip if node_modules/ exists) ----
node_modules = webui_dir / "node_modules"
if not node_modules.exists():
    print("[build_webui] npm install (first run or clean)...")
    result = subprocess.run(
        ["npm", "install"],
        cwd=str(webui_dir),
        shell=True,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print("[build_webui] ERROR: npm install failed")
        print(result.stdout)
        print(result.stderr, file=sys.stderr)
        sys.exit(1)
    print("[build_webui] npm install OK")
else:
    print("[build_webui] node_modules/ exists — skipping npm install")

# ---- 5. npm run build ----
print("[build_webui] npm run build...")
result = subprocess.run(
    ["npm", "run", "build"],
    cwd=str(webui_dir),
    shell=True,
    capture_output=True,
    text=True,
)
print(result.stdout)
if result.returncode != 0:
    print("[build_webui] ERROR: npm run build failed", file=sys.stderr)
    print(result.stderr, file=sys.stderr)
    sys.exit(1)

# ---- 6. Verify output and gzip ----
dist_index = webui_dir / "dist" / "index.html"
data_dir = script_dir / "data"
data_dir.mkdir(parents=True, exist_ok=True)

if not dist_index.exists():
    print("[build_webui] ERROR: webui/dist/index.html not found after build", file=sys.stderr)
    sys.exit(1)

print(f"[build_webui] Built {dist_index} ({dist_index.stat().st_size} bytes)")

# Gzip the built index.html for LittleFS (the C++ serve code sends
# Content-Encoding: gzip so the browser auto-decompresses). Smaller flash
# footprint, faster over-the-wire — every byte counts on an ESP32. :3
gz_path = data_dir / "index.html.gz"
with open(dist_index, "rb") as src, gzip.open(gz_path, "wb", compresslevel=9) as dst:
    shutil.copyfileobj(src, dst)

compressed_size = gz_path.stat().st_size
orig_size = dist_index.stat().st_size
print(f"[build_webui] Gzipped {gz_path} ({compressed_size} bytes, {compressed_size*100//orig_size}% of original)")

# ---- 7. Copy uncompressed index.html as fallback ----
# The C++ serve logic tries .gz first, but we keep the uncompressed copy
# in data/ as a manual fallback (and so the PlatformIO IDE sees files to upload). :3
fallback_html = data_dir / "index.html"
shutil.copy2(dist_index, fallback_html)
print(f"[build_webui] Copied fallback {fallback_html}")

print("[build_webui] Done — web assets ready for LittleFS upload :3")