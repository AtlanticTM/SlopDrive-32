---
title: Contributing to the documentation
description: >-
  How to build the SlopSync documentation site, which writing register applies
  to which page, and why no wire number is ever typed by hand.
register: STE
---

# Contributing to the documentation

This page covers the documentation site. For protocol changes, see
[the RFC process](rfc-process.md).

## Build the site

```bash
cd docs-site
python -m venv .venv
.venv/bin/pip install -r requirements.txt      # Windows: .venv\Scripts\pip
python tools/gen_docs_tables.py                # refresh generated pages
mkdocs build --strict                          # or: mkdocs serve
```

Run `mkdocs` from `docs-site/`. The glossary include resolves against the
working directory, and the build fails loudly if you run it from elsewhere.

`--strict` is not optional in review. It turns a dead cross-reference into a
build failure. A dead link in a protocol specification is a defect, because
implementers follow those links.

## The two writing registers

Every page declares its register in front matter. The register tells you which
rules bind the page you are editing. The theme renders it as a badge at the
top of the page, so you can see it while you read.

=== "STE — guides and reference"

    ```yaml
    ---
    register: STE
    ---
    ```

    Influenced by ASD-STE100.

    - One idea per sentence.
    - Active voice. Present tense.
    - An instruction is 20 words or fewer.
    - No noun stack longer than three words.
    - One term, one meaning, used consistently.

=== "IEEE — the specification"

    ```yaml
    ---
    register: IEEE
    ---
    ```

    Numbered clauses. RFC 2119 keywords: MUST, MUST NOT, SHALL, SHOULD, MAY.
    Exact cross-references. No prose ambiguity.

    This register is permitted to be dense. Implementers need exactness more
    than they need approachability.

!!! danger "Simplify sentences. Never simplify terminology."

    This is the constraint people get wrong.

    *Quintic*, *deadman*, *token bucket*, *etag*, *jerk*, *arbiter*,
    *conflation* and *catalog* are precise vocabulary. **They stay.** Replacing
    one with a vague plain-language substitute makes the page shorter and the
    reader wronger.

    STE's real rule is "one term, one meaning, used consistently", which
    *protects* domain words rather than banning them. Define the word in
    [the Dictionary](../reference/dictionary.md), then use it everywhere
    without apology.

## Never type a wire number

Every frame type, CBOR key, NACK code, channel id, field role, setting
category, safety cause, stream kind, packed field type and limit on this site
is generated from `registry.yaml`.

This is not a preference. This project has shipped the same
hand-copied-constant drift bug four separate times: a stale JavaScript
intent-schema table, a hand-rolled safety-cause enum, a hardcoded interface
layout table, and a duplicated build caveat. **A wrong NACK code in documentation is worse than a wrong NACK
code in source, because implementers trust documentation and nothing compiles
it.**

So:

1. Change the number in `registry.yaml`. That is the only place a number is
   decided.
2. Run `python tools/gen_docs_tables.py`.
3. Commit the registry and the generated output together.

CI runs `python tools/gen_docs_tables.py --check` and fails the build when a
generated file has drifted. Editing a generated page by hand achieves nothing:
the next run overwrites it, and CI catches it first.

A generated page carries a `generated: true` badge and a DO-NOT-EDIT banner.

**Adding a new registry section?** The generator refuses to run until you say
which page documents it. That is deliberate — an undocumented wire number is
exactly the failure this gate exists to stop.

## The Specification is generated too

Every page under **Specification** except [Errata](../spec/errata.md) is
produced from `docs/slopsync/SPEC.md` by `tools/gen_spec_pages.py`. That file
is the normative source, and it is the only place to change a clause.

There is deliberately no second copy of the normative text here. An implementer
trusts a specification absolutely, so a specification that can drift from its
source would be the worst instance of the drift bug above — worse than a wrong
NACK code, because a whole clause can go stale at once.

The generator copies normative text verbatim. It adds the split, the
clause-derived anchors (`§6.4` → `#s6-4`), and the links that make `§n.m`
cross-references work across page boundaries. It refuses to run on a clause no
page claims and on a cross-reference that resolves to nothing.

## The Dictionary is generated too

Do not edit `docs/reference/dictionary.md`. Edit `dictionary.yaml` and
regenerate.

One source produces both the Dictionary page and the hover definitions
appended to every other page, so a term can never acquire a second definition.
The generator refuses two different definitions for the same written form, and
refuses a `see` cross-reference to a term that does not exist.

## Stubs are allowed. Fake content is not

A page whose content is deferred says so. A stub:

- declares `status: stub` in front matter, which renders a badge;
- states what belongs on the page;
- links to the source material an author should read first.

A stub never invents plausible-sounding prose to fill space. A reader must be
able to tell, in one second, that a page is unwritten.

Content follows the v1.0 tag on purpose. The normative section must describe
what shipped, not what was planned.

## Adding a page

1. Create the file under `docs/`.
2. Add `title`, `description` and `register` to its front matter. The
   `description` becomes the page's meta description; write a real one,
   because search is how anyone finds this site.
3. Add it to `nav:` in `mkdocs.yml`. A page outside the nav fails the strict
   build.
4. Build with `--strict`.

## See also

- [Governance](governance.md)
- [The RFC process](rfc-process.md)
- `docs-site/README.md` — layout, the one outside path, and how to extract
  this directory into its own repository.
