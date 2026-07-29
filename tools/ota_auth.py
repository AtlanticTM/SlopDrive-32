#!/usr/bin/env python3
# ota_auth.py — PlatformIO pre-script for the sd32-ota environment.
#
# ArduinoOTA/espota needs the OTA password on the command line (--auth=<secret>).
# We refuse to hardcode that in platformio.ini (it's committed to git). Instead
# this hook parses include/secrets.h at build time, pulls SECRET_OTA_PASSWORD,
# and appends it to the espota UPLOADERFLAGS. The secret rides the local build
# only — it never lands in a tracked file. :3
#
# Wired via `extra_scripts = pre:tools/ota_auth.py` in [env:sd32-ota].
#
# If secrets.h is missing or the macro is absent/empty we DON'T inject --auth and
# print a loud warning; espota will then fail the auth handshake against a
# password-protected device, which is the correct fail-safe (better a clean
# "auth failed" than a silent unauthenticated flash attempt).

import os
import re

Import("env")  # noqa: F821  (injected by PlatformIO's SCons runner)

SECRETS_PATH = os.path.join(env.subst("$PROJECT_DIR"), "include", "secrets.h")  # noqa: F821


def _read_ota_password(path):
    """Return the string value of #define SECRET_OTA_PASSWORD "..." or None."""
    if not os.path.isfile(path):
        return None
    # Match:  #define SECRET_OTA_PASSWORD   "the secret"
    pat = re.compile(
        r'^\s*#\s*define\s+SECRET_OTA_PASSWORD\s+"((?:[^"\\]|\\.)*)"',
        re.MULTILINE,
    )
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return None
    m = pat.search(text)
    if not m:
        return None
    # Un-escape the two sequences a C string literal can carry that matter here.
    return m.group(1).replace('\\"', '"').replace("\\\\", "\\")


password = _read_ota_password(SECRETS_PATH)

if password:
    # espota.py accepts --auth=<pass>. Feed it through UPLOADERFLAGS so it's
    # appended to the upload command for BOTH -t upload and -t uploadfs.
    env.Append(UPLOADERFLAGS=["--auth", password])  # noqa: F821
    print("[ota_auth] SECRET_OTA_PASSWORD loaded from include/secrets.h — espota --auth wired :3")
else:
    print("[ota_auth] WARNING: SECRET_OTA_PASSWORD not found in include/secrets.h — "
          "espota will run WITHOUT --auth and will FAIL against a password-protected "
          "device. Copy the OTA line from secrets.example.h into secrets.h.")