---
paths:
  - "**"
---

# Where files go

The repo root is a closed set: build entry points, license surface, and the
pins. It is not a landing zone. A new loose file at the root is a defect, and
a hook blocks it (`.claude/hooks/tidy_check.py`).

| What | Home | Tracked? |
|---|---|---|
| Per-run output: captures, traces, ladders, sweeps, ELF archives | `artifacts/` | no |
| Vendor manuals, datasheets, schematics | `docs/reference/` | no |
| Prose that outlives the run: findings, designs, baselines | `docs/` | yes |
| Scripts, probes, one-off instruments | `tools/` | see `.gitignore` |
| Doctrine | `.claude/rules/` | yes |
| Flagship board firmware | `flagship_<chip>/`, its OWN PlatformIO project | yes |
| Status, versions, results worth keeping | the dev board (`bd`) | n/a |

## Instruments write to `artifacts/`

Every `tools/` script defaults `--out` under `artifacts/`. A new script does
the same. A bare filename resolves against the CALLER's cwd, so it lands
wherever the harness happened to start, which is how 29 result files ended up
in the root once.

`artifacts/` is gitignored except `.gitkeep`, which exists so a fresh clone
has the directory the tools write into without any of them calling mkdir.

## Board labels: one axis per namespace

`bd` labels carry four unrelated axes. Keep them in separate namespaces or the
set rots into a pile nobody filters by.

| Axis | Form | Examples |
|---|---|---|
| Subsystem | `area:<name>` | `area:motion` `area:comms` `area:webui` `area:system` `area:sim` `area:tooling` `area:docs` |
| Cross-cutting flag | bare | `safety` |
| Process state | bare | `awaiting-stamp` `pending-ruling` `parked` |
| Provenance and tier | bare | `ledger-queue` `tier0-paved-path` |

Every issue gets exactly ONE `area:` label. Set it at creation
(`bd create --labels area:motion`); children inherit it from `--parent` unless
`--no-inherit-labels`. Filter with `bd list --label area:motion`, or
`--label-pattern 'area:*'` to see the whole taxonomy.

`area:` names the subsystem the FIX lands in, not where the symptom showed up.
An NVS clobber triggered by homing is `area:system`, not `area:motion`.

## A file is evidence, not a fact

A measurement that matters gets quoted onto the board, where it is stamped
(C-4) and survives the file being regenerated. Leaving the number only in
`artifacts/foo.json` means the next person has to rediscover both the file and
what it proved.

## A board whose firmware uses a coprocessor gets its OWN project

`flagship_p4/` and `flagship_c6/` are separate PlatformIO projects, each with
its own `platformio.ini`, not environments in the machine repo's. That is a
build-correctness boundary, not tidiness.

The ULP hook resolves exactly one directory, `$PROJECT_DIR/ulp`, with no
per-environment override (platform 55.03.39: `espidf.py:2957`, `ulp.py:64`),
and it wires the result into the ELF unconditionally -- there is no
`ULP_COPROC_ENABLED` check. The exclusion list is `esp32c2/c3/c5/h2`, so
**esp32s3 is not excluded**: a `ulp/` directory at the machine repo's root
would be compiled into the S3 production firmware. It would fire only on builds
that rebuild the Arduino libs (`arduino.py:472`), which is the same event
`build-test-deploy.md` already teaches us to retry rather than diagnose, so it
would wear a known trap's costume.

Patching the platform is not the escape: `arduino.py:455` reinstalls the
framework packages when `custom_sdkconfig` changes, which is the very event
that needs the patch. A separate `PROJECT_DIR` is the only scoping the tool
itself respects.

Build with `-d`: `pio run -d flagship_p4`. Standalone invocation still works
exactly as it does for every other tool here.

## Deleting

Anything under `artifacts/` is regenerable by re-running its instrument, so it
is safe to delete without ceremony. That is the whole point of the boundary:
if deleting a file needs a discussion, it was never an artifact.
