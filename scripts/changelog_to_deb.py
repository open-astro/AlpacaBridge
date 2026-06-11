#!/usr/bin/env python3
"""Generate debian/changelog from the root CHANGELOG.md.

CHANGELOG.md (Keep a Changelog format) is the single source of truth for
release history; debian/changelog is a build artifact derived from it by
scripts/build_deb.sh and is not tracked in git. Ported from the OpenAstro
Guider's generator, adapted to this repo's conventions: the version comes
from the VERSION file, and the in-progress section is written as
"## [X.Y.Z] - UNRELEASED" rather than "## [Unreleased]".

Mapping:
  - Every "## [X.Y.Z] - YYYY-MM-DD" section becomes one Debian changelog
    stanza for X.Y.Z, dated noon UTC on the release date (UTC so the
    generated output is byte-identical across build environments).
  - Bullets keep their "### Added/Changed/..." category as a prefix and are
    re-wrapped to Debian's continuation-line layout. Markdown emphasis,
    inline code, and links are stripped.
  - If --version names a release that has no dated section yet, the top
    stanza is synthesized from the "## [X.Y.Z] - UNRELEASED" section with
    distribution UNRELEASED and the current time. The script warns when the
    UNRELEASED label disagrees with --version, and when unreleased work
    would be missing from a released version's changelog.
"""

import argparse
import datetime
import email.utils
import re
import sys

HEADING_RE = re.compile(r"^## \[([^\]]+)\](?: - (\d{4}-\d{2}-\d{2}|UNRELEASED))?\s*$")
CATEGORY_RE = re.compile(r"^### (.+?)\s*$")
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+[A-Za-z0-9.~+-]*$")

# Order matters: links first (their text may contain emphasis), then
# emphasis/code markers, innermost first.
MARKDOWN_SUBS = [
    (re.compile(r"!?\[([^\]]*)\]\([^)]*\)"), r"\1"),  # [text](url) -> text
    (re.compile(r"\*\*([^*]+)\*\*"), r"\1"),  # **bold**
    (re.compile(r"\*([^*\s][^*]*)\*"), r"\1"),  # *emphasis*
    (re.compile(r"`([^`]*)`"), r"\1"),  # `code`
]


def strip_markdown(text):
    for pattern, repl in MARKDOWN_SUBS:
        text = pattern.sub(repl, text)
    return re.sub(r"\s+", " ", text).strip()


def parse_changelog(path):
    """Return a list of sections: {label, date, bullets: [(category, text)]}."""
    sections = []
    current = None
    category = None
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            m = HEADING_RE.match(line)
            if m:
                current = {"label": m.group(1), "date": m.group(2), "bullets": []}
                sections.append(current)
                category = None
                continue
            if current is None:
                continue
            m = CATEGORY_RE.match(line)
            if m:
                category = m.group(1)
                continue
            if line.startswith("- "):
                current["bullets"].append([category, line[2:].strip()])
            elif line.strip() and current["bullets"]:
                # Continuation line or nested bullet: fold into the open bullet.
                current["bullets"][-1][1] += " " + line.strip().lstrip("- ")
    return sections


def wrap_bullet(category, text, width=78):
    text = strip_markdown(text)
    if category:
        text = "%s: %s" % (category, text)
    words = text.split()
    lines = []
    line = "  *"
    for word in words:
        if len(line) + 1 + len(word) > width and line not in ("  *", "   "):
            lines.append(line)
            line = "   "
        line += " " + word
    lines.append(line)
    return lines


def emit_stanza(out, package, version, distribution, bullets, date, maintainer):
    out.write("%s (%s) %s; urgency=low\n\n" % (package, version, distribution))
    if bullets:
        for category, text in bullets:
            out.write("\n".join(wrap_bullet(category, text)) + "\n")
    else:
        out.write("  * See CHANGELOG.md for release notes.\n")
    out.write("\n -- %s  %s\n" % (maintainer, email.utils.format_datetime(date)))


def section_date(section):
    # Noon UTC, not local noon: keeps generated stanza dates byte-identical
    # across build environments and DST transitions.
    d = datetime.date.fromisoformat(section["date"])
    return datetime.datetime(d.year, d.month, d.day, 12, 0, 0, tzinfo=datetime.timezone.utc)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--changelog", required=True, help="path to CHANGELOG.md")
    ap.add_argument("--out", required=True, help="path to debian/changelog to write")
    ap.add_argument("--package", required=True, help="Debian source package name")
    ap.add_argument("--version", required=True, help="version being built (from VERSION)")
    ap.add_argument("--maintainer", required=True, help='"Name <email>"')
    args = ap.parse_args()

    if not VERSION_RE.match(args.version):
        sys.exit("error: --version %r is not a valid Debian version" % args.version)

    sections = parse_changelog(args.changelog)
    released = [
        s
        for s in sections
        if VERSION_RE.match(s["label"]) and s["date"] and s["date"] != "UNRELEASED"
    ]
    unreleased = next(
        (s for s in sections if s["date"] == "UNRELEASED" or s["label"].lower() == "unreleased"),
        None,
    )

    stanzas = []
    if any(s["label"] == args.version for s in released):
        if unreleased and unreleased["bullets"]:
            print(
                "warning: version %s is already released in CHANGELOG.md but "
                "the %s section is not empty; the unreleased work will be in "
                "the binary but not in its changelog. Cut a new version, or "
                "ignore if intended." % (args.version, unreleased["label"]),
                file=sys.stderr,
            )
    else:
        bullets = unreleased["bullets"] if unreleased else []
        if unreleased and VERSION_RE.match(unreleased["label"]) and unreleased["label"] != args.version:
            print(
                "warning: VERSION is %s but CHANGELOG.md's unreleased section "
                "is labeled [%s]; using %s for the top stanza. Align the two "
                "before release." % (args.version, unreleased["label"], args.version),
                file=sys.stderr,
            )
        stanzas.append((args.version, "UNRELEASED", bullets, datetime.datetime.now().astimezone()))

    for s in released:
        stanzas.append((s["label"], "stable", s["bullets"], section_date(s)))

    if not stanzas:
        sys.exit("error: no versioned sections found in %s" % args.changelog)

    with open(args.out, "w", encoding="utf-8") as out:
        for i, (version, dist, bullets, date) in enumerate(stanzas):
            if i:
                out.write("\n")
            emit_stanza(out, args.package, version, dist, bullets, date, args.maintainer)


if __name__ == "__main__":
    main()
