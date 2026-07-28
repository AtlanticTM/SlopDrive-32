"""Remove glossary tooltips from the Specification tier.

WHY THIS EXISTS
    `pymdownx.snippets.auto_append` appends the generated glossary to every
    page, and Material's `content.tooltips` feature turns each `<abbr>` into a
    hover definition. That is exactly right for a guide: a reader who meets
    "etag" for the first time gets one sentence without leaving the page.

    It is wrong for a normative document. In the IEEE register every word on
    the page is either normative or visibly marked as not normative. A
    non-normative hover definition sitting over "hub" inside a MUST clause is
    precisely the ambiguity that register exists to remove: two sources of
    meaning for one word, one of them invisible in print and unversioned
    against the clause it decorates.

    The Dictionary stays one click away — the specification links it. It just
    does not overlay it on normative text.

HOW
    The glossary is applied by a Markdown extension, so it cannot be stripped
    in `on_page_markdown` (the abbreviation definitions have not been consumed
    yet at that point). It is stripped from the rendered HTML instead, where
    the result is unambiguous: the `<abbr>` wrapper goes, its text stays.

    Failing loudly matters here. If a future Material release changes the
    tooltip markup, `--strict` will not notice, so this hook counts what it
    removed and warns when a spec page still carries an `<abbr>`.
"""

from __future__ import annotations

import logging
import re

log = logging.getLogger("mkdocs.hooks.spec_no_glossary")

# Pages whose URI starts with one of these prefixes are the normative tier.
NORMATIVE_PREFIXES: tuple[str, ...] = ("spec/",)

# `<abbr title="...">term</abbr>` — non-greedy, and DOTALL because a long
# definition can be wrapped by the HTML serializer.
_ABBR = re.compile(r"<abbr\b[^>]*>(.*?)</abbr>", re.DOTALL)


def _is_normative(src_uri: str) -> bool:
    return src_uri.startswith(NORMATIVE_PREFIXES)


def on_page_content(html: str, page, config, files) -> str:
    if not _is_normative(page.file.src_uri):
        return html

    stripped, count = _ABBR.subn(r"\1", html)
    if "<abbr" in stripped:
        log.warning(
            "%s: an <abbr> survived the normative-tier glossary strip. "
            "Material's tooltip markup may have changed; check "
            "docs-site/hooks/spec_no_glossary.py.",
            page.file.src_uri,
        )
    if count:
        log.info("%s: removed %d glossary tooltip(s) (normative tier).",
                 page.file.src_uri, count)
    return stripped
