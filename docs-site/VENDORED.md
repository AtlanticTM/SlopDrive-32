# Vendored third-party assets

This directory ships one third-party file. It is recorded here, in the same
spirit as `lib/ruckig/VENDORED.md`: provenance, license, and the exact
procedure for updating it.

## `docs/assets/javascripts/mermaid.min.js`

| | |
|---|---|
| Package | `mermaid` |
| Version | 11.16.0 |
| Source | `https://unpkg.com/mermaid@11/dist/mermaid.min.js` (resolved to `mermaid@11.16.0`) |
| Retrieved | 2026-07-26 |
| Size | 3,565,102 bytes |
| SHA-256 | `74d7c46dabca328c2294733910a8aa1ed0c37451776e8d5295da38a2b758fb9b` |
| License | MIT |

The file is **byte-identical to upstream**. Never patch it locally. If mermaid
needs different behavior, configure it, or wrap it — do not edit the vendored
bytes, because the next update silently reverts the edit.

### Why it is vendored

Material for MkDocs fetches `mermaid.min.js` from `unpkg.com` at page load.
That makes a diagram page — and only a diagram page — depend on a CDN, while
every other part of this build is hermetic and offline-capable.

SlopSync is a LAN-first, offline-first protocol. A page that explains that
property must not phone a CDN to draw its own diagram. The failure modes are
real and boring: a blocked network, an air-gapped bench, a CDN outage, a
reader behind a corporate proxy. Any of them turns every figure on this site
into raw text.

Material skips its own fetch whenever the global `mermaid` is already defined,
so vendoring is one asset plus one `extra_javascript` line. The cost is ~3.4
MiB in the repository. That was accepted deliberately, in exchange for a site
that renders identically with the network unplugged.

### Updating it

```bash
cd docs-site
curl -sSL -o docs/assets/javascripts/mermaid.min.js \
     https://unpkg.com/mermaid@11/dist/mermaid.min.js
python -c "import hashlib,pathlib; \
  b=pathlib.Path('docs/assets/javascripts/mermaid.min.js').read_bytes(); \
  print(len(b), hashlib.sha256(b).hexdigest())"
```

Update the table above with the new version, size and hash. Then **prove the
diagrams still render**: a fence that fails to parse becomes silent raw text,
and `mkdocs build --strict` reports nothing about it. Build the site, serve
`site/`, and check that every `.mermaid` host contains an `<svg>` — in both
color schemes. Material renders into a **closed** shadow root, so page
JavaScript cannot see the SVG; the check needs a browser driven over the
DevTools Protocol with `DOM.getDocument { pierce: true }`.

Stay on the major version Material asks for (`mermaid@11` today). Material
pins the major in its own fetch URL, so a vendored copy from a different major
would behave differently for anyone whose build lacks this file.
