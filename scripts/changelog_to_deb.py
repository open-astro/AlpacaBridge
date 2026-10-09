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
    distribution UNRELEASED and the current time. A beta --version
    (X.Y.Z~betaN, docs/beta-channel.md) is a published build instead: its
    top stanza has distribution "beta" and is dated noon UTC on --date,
    which is then required (build_deb.sh passes the README badge date, the
    day the beta was cut), so two builds of one beta tag are byte-identical.
    The script warns when the UNRELEASED label disagrees with --version,
    and when unreleased work would be missing from a released version's
    changelog.
  - Unreleased work also lives in changelog.d/ fragments (see
    changelog.d/README.md): their bullets are appended to the synthesized
    stanza, and count as unreleased work for the warning above.
"""

import argparse
import datetime
import email.utils
import os
import pathlib
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import changelog_fragments  # noqa: E402  (shared fragment parser)

HEADING_RE = re.compile(r"^## \[([^\]]+)\](?: - (\d{4}-\d{2}-\d{2}|UNRELEASED))?\s*$")
# Released versions are collapsed into <details> blocks in CHANGELOG.md (only the
# current/unreleased version stays an uncollapsed "## [..]" heading), so the version
# heading also appears as a <summary> line. Match that form too.
SUMMARY_HEADING_RE = re.compile(
    r"^<summary><strong>\[([^\]]+)\](?: - (\d{4}-\d{2}-\d{2}|UNRELEASED))?\s*</strong></summary>\s*$"
)
CATEGORY_RE = re.compile(r"^### (.+?)\s*$")
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+[A-Za-z0-9.~+-]*$")
# The one published pre-release form (docs/beta-channel.md) is owned by
# changelog_fragments.is_beta(), which build_deb.sh also asks (--is-beta). Any
# other ~suffix (an ~rc1 or ~dev1 test build) is unreleased work and gets the
# UNRELEASED stanza as before.

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
            m = HEADING_RE.match(line) or SUMMARY_HEADING_RE.match(line)
            if m:
                current = {"label": m.group(1), "date": m.group(2), "bullets": []}
                sections.append(current)
                category = None
                continue
            if current is None:
                continue
            # Skip the <details>/</details> wrappers around collapsed versions so they
            # aren't folded into the previous bullet as continuation text. Use startswith
            # (not an exact match) so a future "<details open>" or any attribute is caught.
            stripped = line.strip()
            if stripped.startswith("<details") or stripped.startswith("</details"):
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


def fragment_bullets(directory):
    """Bullets of every changelog.d fragment, in parse_changelog()'s shape."""
    bullets = []
    for _name, entries in changelog_fragments.load_fragments(directory):
        for category in sorted(entries, key=changelog_fragments.category_key):
            for lines in entries[category]:
                text = lines[0][2:].strip()
                for extra in lines[1:]:
                    text += " " + extra.strip().lstrip("- ")
                bullets.append([category, text])
    return bullets


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


def self_test():
    """Drive main() over fixture trees: the fragment bullets and the warning."""
    import contextlib
    import io
    import shutil
    import subprocess
    import tempfile

    released = "## [1.0.0] - 2026-01-01\n\n### Added\n- **old**\n"
    fragment = "### Fixed\n- **a**\n  more\n"

    def run(version, changelog, fragments, date=None, expect_rc=None):
        """(stdout text of the written changelog, stderr). expect_rc: main() must exit with it."""
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "CHANGELOG.md").write_text(changelog, encoding="utf-8")
            if fragments:
                (root / "changelog.d").mkdir()
                for name, text in fragments.items():
                    (root / "changelog.d" / name).write_text(text, encoding="utf-8")
            out = root / "changelog"
            argv, err = sys.argv, io.StringIO()
            sys.argv = [
                "changelog_to_deb.py", "--changelog", str(root / "CHANGELOG.md"),
                "--out", str(out), "--package", "pkg", "--version", version,
                "--maintainer", "T <t@example.com>",
            ] + (["--date", date] if date else [])
            try:
                with contextlib.redirect_stderr(err):
                    try:
                        main()
                        rc = 0
                    except SystemExit as e:
                        rc = e.code if isinstance(e.code, int) else 2
                        if expect_rc is None:
                            raise
                if expect_rc is not None:
                    check("exit code for %s" % version, rc == expect_rc)
                    return "", err.getvalue()
            finally:
                sys.argv = argv
            return out.read_text(encoding="utf-8"), err.getvalue()

    failures = []

    def check(name, ok):
        if not ok:
            failures.append(name)

    # A fragment alone feeds the synthesized UNRELEASED stanza of a new version.
    text, err = run("1.0.1", released, {"x.md": fragment})
    check("stanza", "pkg (1.0.1) UNRELEASED; urgency=low" in text)
    check("bullet", "  * Fixed: a more\n" in text)
    check("released stanza kept", "pkg (1.0.0) stable; urgency=low" in text)
    check("no warning for a new version", err == "")

    # No fragments: the stanza falls back to the placeholder bullet.
    text, err = run("1.0.1", released, {})
    check("empty stanza", "  * See CHANGELOG.md for release notes.\n" in text.split("pkg (1.0.0)")[0])

    # A fragment against an already released version warns, and adds no stanza.
    text, err = run("1.0.0", released, {"x.md": fragment})
    check("warning", "already released" in err and "UNRELEASED section" in err)
    check("no bullet in released stanza", "Fixed: a more" not in text)

    # A beta VERSION (5.0.0~beta1) heads the changelog with the fragments' notes
    # and sorts below the stable release it leads up to.
    # A beta is a published build: its stanza is a release stanza (distribution
    # "beta", dated noon UTC on the --date it was cut, never UNRELEASED or now()),
    # so two builds of the same beta tag are byte-identical.
    text, err = run("1.1.0~beta1", released, {"x.md": fragment}, date="2026-11-01")
    check("beta stanza", text.startswith("pkg (1.1.0~beta1) beta; urgency=low") and "  * Fixed: a more\n" in text)
    check("beta stanza date", "Sun, 01 Nov 2026 12:00:00 +0000" in text.split("pkg (1.0.0)")[0])
    check("beta no warning", err == "")
    again, _ = run("1.1.0~beta1", released, {"x.md": fragment}, date="2026-11-01")
    check("beta stanza reproducible", again == text)
    _, err = run("1.1.0~beta1", released, {"x.md": fragment}, expect_rc=2)
    check("beta without --date is refused", "--date" in err)
    # A legacy UNRELEASED label names the beta's base version: no mismatch.
    legacy = "## [1.1.0] - UNRELEASED\n\n### Fixed\n- **b**\n\n" + released
    text, err = run("1.1.0~beta1", legacy, {}, date="2026-11-01")
    check("beta under its base label: stanza", text.startswith("pkg (1.1.0~beta1) beta; urgency=low"))
    check("beta under its base label: no warning", err == "")
    text, err = run("1.2.0~beta1", legacy, {}, date="2026-11-01")
    check("beta under another label: warning", "is labeled [1.1.0]" in err)
    # Only ~betaN is a published pre-release; another ~suffix is unreleased work, no --date needed.
    text, err = run("1.1.0~rc1", released, {"x.md": fragment})
    check("~rc1 is not a beta stanza", text.startswith("pkg (1.1.0~rc1) UNRELEASED; urgency=low") and err == "")
    # Only a beta matches its base label; an ~rc1 under [1.1.0] still warns.
    _, err = run("1.1.0~rc1", legacy, {})
    check("~rc1 under its base label: warning", "is labeled [1.1.0]" in err)
    # An impossible date is refused cleanly, not with a traceback.
    _, err = run("1.1.0~beta1", released, {"x.md": fragment}, date="2026-02-30", expect_rc=2)
    check("impossible --date is refused", "not a real date" in err)
    if shutil.which("dpkg"):
        order = ["5.0.0~beta1", "5.0.0~beta2", "5.0.0", "5.0.1", "5.1.0~beta1"]
        for lo, hi in zip(order, order[1:]):
            r = subprocess.run(["dpkg", "--compare-versions", lo, "lt", hi])
            check("dpkg order %s < %s" % (lo, hi), r.returncode == 0)

    if failures:
        print("changelog_to_deb self-test FAILED: %s" % ", ".join(failures), file=sys.stderr)
        return 1
    print("changelog_to_deb self-test passed")
    return 0


def main():
    if "--self-test" in sys.argv[1:] and len(sys.argv) == 2:
        sys.exit(self_test())
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--changelog", required=True, help="path to CHANGELOG.md")
    ap.add_argument("--out", required=True, help="path to debian/changelog to write")
    ap.add_argument("--package", required=True, help="Debian source package name")
    ap.add_argument("--version", required=True, help="version being built (from VERSION)")
    ap.add_argument("--maintainer", required=True, help='"Name <email>"')
    ap.add_argument("--date", metavar="YYYY-MM-DD",
                    help="for a beta --version: the day it was cut (the README badge date); its stanza is dated "
                    "noon UTC that day so the build is reproducible. Required for a beta, ignored otherwise.")
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

    frag_bullets = fragment_bullets(pathlib.Path(args.changelog).parent / "changelog.d")

    stanzas = []
    if any(s["label"] == args.version for s in released):
        if (unreleased and unreleased["bullets"]) or frag_bullets:
            print(
                "warning: version %s is already released in CHANGELOG.md but "
                "the %s section or changelog.d/ is not empty; the unreleased work will be in "
                "the binary but not in its changelog. Cut a new version, or "
                "ignore if intended." % (args.version, unreleased["label"] if unreleased else "UNRELEASED"),
                file=sys.stderr,
            )
    else:
        bullets = (unreleased["bullets"] if unreleased else []) + frag_bullets
        # A beta (5.0.0~beta1) matches the label of its base version ([5.0.0]);
        # any other ~suffix (~rc1) must match the label exactly, as before.
        is_beta = changelog_fragments.is_beta(args.version)
        base = changelog_fragments.base_version(args.version) if is_beta else args.version
        if unreleased and VERSION_RE.match(unreleased["label"]) and unreleased["label"] != base:
            print(
                "warning: VERSION is %s but CHANGELOG.md's unreleased section "
                "is labeled [%s]; using %s for the top stanza. Align the two "
                "before release." % (args.version, unreleased["label"], args.version),
                file=sys.stderr,
            )
        if is_beta:
            # A beta is a published build, not unreleased work: a release stanza
            # in the "beta" distribution, dated like a release (noon UTC on the
            # cut date) so two builds of the same tag are byte-identical.
            if not args.date or not changelog_fragments.DATE_RE.match(args.date):
                ap.error("--version %s is a beta: pass --date YYYY-MM-DD (the README badge date)" % args.version)
            try:
                beta_date = section_date({"date": args.date})
            except ValueError as e:
                ap.error("--date %s is not a real date (%s)" % (args.date, e))
            stanzas.append((args.version, "beta", bullets, beta_date))
        else:
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
