#!/usr/bin/env bash
# STYLE GATE (PreToolUse, Edit|Write): blocks em dashes and British spellings
# in text being ADDED (never in surrounding context), and comment-heavy code
# (CANON C-11/C-12 in the machine repo; same law here by operator ruling).
# canon_lint / slopsync_lint remain the authoritative full-tree floor; this is
# the fast pre-filter at the point of write.
exec python "$(dirname "$0")/style_check.py"
