#!/usr/bin/env python3
"""Validate changelog fragments and assemble them into CHANGELOG.md at release.

A PR adds one file, ``changelog.d/<branch-slug>.md``, instead of editing the
shared UNRELEASED section of CHANGELOG.md, so parallel PRs never conflict on it.
Only the release step writes CHANGELOG.md. The format is in
``changelog.d/README.md``.

  --check                    validate every fragment (CI and pre-flight)
  --preview [--version V]    print the section --release would write (no date)
  --bump                     print the proposed next version
  --release V --date D       write the dated section, collapse the previous
                             release, merge a legacy UNRELEASED section, and
                             delete the fragments
"""

from __future__ import annotations

import argparse
import contextlib
import io
import datetime
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HEADING_RE = re.compile(r"^## \[([^\]]+)\](?: - (\d{4}-\d{2}-\d{2}|UNRELEASED))?\s*$")
SUMMARY_HEADING_RE = re.compile(
    r"^<summary><strong>\[([^\]]+)\](?: - (\d{4}-\d{2}-\d{2}|UNRELEASED))?\s*</strong></summary>\s*$"
)
NAME_RE = re.compile(r"^[a-z0-9][a-z0-9._-]*\.md$")
# The one spelling of a VERSION (docs/beta-channel.md). A VERSION file may carry
# the Debian pre-release suffix of a beta (5.0.0~beta2); betas count from 1, as
# release_tag.py's TAG_RE. The patterns are plain strings with no capturing
# group, so other scripts can embed them (check_docs_drift.py's badge regexes)
# without shifting their own group numbers; shell scripts ask --is-beta.
BARE_PATTERN = r"\d+\.\d+\.\d+"
BETA_SUFFIX_PATTERN = r"~beta[1-9]\d*"
VERSION_PATTERN = BARE_PATTERN + "(?:" + BETA_SUFFIX_PATTERN + ")?"
BARE_RE = re.compile("^" + BARE_PATTERN + "$")  # a dated CHANGELOG heading is always a bare X.Y.Z
BETA_RE = re.compile("^" + BARE_PATTERN + BETA_SUFFIX_PATTERN + "$")
VERSION_RE = re.compile("^" + VERSION_PATTERN + "$")
DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
CATEGORY_RE = re.compile(r"^### (.+?)\s*$")
BASE_CATEGORIES = [
    "Breaking changes",
    "Added",
    "Changed",
    "Deprecated",
    "Removed",
    "Fixed",
    "Security",
]
QUALIFIED_RE = re.compile(r"^(%s)(?: \(([^()]+)\))?$" % "|".join(BASE_CATEGORIES))
FENCE_RE = re.compile(r"^(`{3,}|~{3,})(.*)$")
BAD_HEADING_RE = re.compile(r"^#{1,2}\s")

def fence_match(line: str, content_col: int = 2) -> re.Match | None:
    """Match a CommonMark fence line, or None for inline code or an indented code block.

    A backtick fence's info string cannot hold a backtick (CommonMark 4.5), so a line that
    starts with inline code is no fence; a line indented 4+ columns past ``content_col`` (the
    content column of the nearest bullet, 2 for "- text") is an indented code block.
    """
    if len(line) - len(line.lstrip()) >= content_col + 4:
        return None
    m = FENCE_RE.match(line.strip())
    if m and m.group(1)[0] == "`" and "`" in m.group(2):
        return None
    return m


Entries = dict  # category -> list of bullets; a bullet is a list of lines


def category_key(category: str) -> tuple:
    """Sort key: base order, the unqualified form first, then qualifiers by name."""
    m = QUALIFIED_RE.match(category)
    if not m:
        return (len(BASE_CATEGORIES), 0, category)
    return (BASE_CATEGORIES.index(m.group(1)), 1 if m.group(2) else 0, m.group(2) or "")


def parse_body(lines: list[str]) -> tuple[Entries, list[str]]:
    """Split ``### Category`` subsections into bullets; return (entries, problems)."""
    entries: Entries = {}
    problems: list[str] = []
    category: str | None = None
    fence = ""  # the opening fence run (``` or ~~~, any length) while inside a fenced block
    in_fence = False
    pending: list[str] = []  # blank lines seen since the last kept line
    content_col = 2  # content column of the latest bullet line, "- text" at indent n gives n + 2
    for raw in lines:
        line = raw.rstrip()
        if not in_fence:
            m = CATEGORY_RE.match(line)
            if m:
                category = m.group(1)
                entries.setdefault(category, [])
                pending = []
                continue
        if not line.strip():
            pending.append("")
            continue
        if category is None:
            problems.append("text before the first '### <Category>' subsection: %r" % line[:60])
            pending = []
            continue
        if not in_fence and line.startswith("- "):
            entries[category].append([line])
        elif entries[category]:
            # a bullet's own text stays on one line; a nested bullet, a fence or a paragraph after a blank line may follow
            if not in_fence and not pending and not line.lstrip().startswith("- ") and not fence_match(line, content_col):
                problems.append("a bullet is wrapped onto a second line, put it on one line: %r" % line[:60])
            # a blank line stays only inside a bullet: before an indented line or within a fence
            if pending and (in_fence or line[0] in " \t"):
                entries[category][-1].extend(pending)
            entries[category][-1].append(line)  # continuation or nested bullet
        else:
            problems.append("'### %s' has text before its first '- ' bullet: %r" % (category, line[:60]))
        pending = []
        if category is not None:
            if not in_fence and line.lstrip().startswith("- "):
                content_col = len(line) - len(line.lstrip()) + 2
            m = fence_match(line, content_col)
            if m and not in_fence:
                fence, in_fence = m.group(1), True
            elif m and in_fence and m.group(1)[0] == fence[0] and len(m.group(1)) >= len(fence) and not m.group(2):
                fence, in_fence = "", False
    if in_fence:
        problems.append("unclosed code fence %r: close it with a matching fence line" % fence)
    return entries, problems


def validate(name: str, text: str) -> list[str]:
    """Return the rule violations of one fragment (file name and body)."""
    problems: list[str] = []
    if not NAME_RE.match(name):
        hint = ""
        if name != name.lower() and NAME_RE.match(name.lower()):
            hint = ": lowercase the branch slug, expected '%s'" % name.lower()
        problems.append(
            "file name must match [a-z0-9][a-z0-9._-]*.md (the branch name after its last '/', lowercased)" + hint
        )
    lines = text.splitlines()
    for i, line in enumerate(lines, 1):
        if BAD_HEADING_RE.match(line):
            problems.append("line %d: a fragment has no '#' or '##' heading (no version, no date)" % i)
    entries, body_problems = parse_body(lines)
    problems.extend(body_problems)
    if not entries:
        problems.append("no '### <Category>' subsection with a '- ' bullet")
    for category, bullets in entries.items():
        if not QUALIFIED_RE.match(category):
            problems.append(
                "unknown category '### %s' (allowed: %s, optionally followed by '(qualifier)')"
                % (category, ", ".join(BASE_CATEGORIES))
            )
        if not bullets:
            problems.append("'### %s' has no '- ' bullet" % category)
    return problems


def fragment_files(directory: Path) -> list[Path]:
    if not directory.is_dir():
        return []
    return sorted(p for p in directory.iterdir() if p.is_file() and p.name != "README.md")


def load_fragments(directory: Path) -> list[tuple[str, Entries]]:
    """Return [(file name, entries)] sorted by file name. Does not validate."""
    return [(p.name, parse_body(p.read_text(encoding="utf-8").splitlines())[0]) for p in fragment_files(directory)]


def check(directory: Path) -> int:
    failures = 0
    for p in fragment_files(directory):
        for problem in validate(p.name, p.read_text(encoding="utf-8")):
            print("%s: %s" % (p, problem), file=sys.stderr)
            failures += 1
    if failures:
        print("%d changelog fragment problem(s); see changelog.d/README.md" % failures, file=sys.stderr)
        return 1
    print("changelog fragments OK (%d file(s))." % len(fragment_files(directory)))
    return 0


# ---------------------------------------------------------------- CHANGELOG.md


def block_starts(lines: list[str]) -> list[int]:
    """Indexes where a version section starts (an expanded heading or its <details>)."""
    starts = []
    for i, line in enumerate(lines):
        if HEADING_RE.match(line):
            starts.append(i)
        elif line.strip().startswith("<details"):
            j = i + 1
            while j < len(lines) and not lines[j].strip():
                j += 1
            if j < len(lines) and SUMMARY_HEADING_RE.match(lines[j]):
                starts.append(i)
    return starts


def base_version(v: str) -> str:
    """5.0.0~beta2 -> 5.0.0; a bare version is returned unchanged."""
    return v.split("~", 1)[0]


def is_beta(v: str) -> bool:
    """True only for the published pre-release form X.Y.Z~betaN (not ~rc1, ~beta0, ~beta)."""
    return BETA_RE.match(v) is not None


def version_tuple(v: str) -> tuple:
    return tuple(int(x) for x in base_version(v).split("."))


def section_headers(lines: list[str]) -> list[tuple[str, str | None]]:
    out = []
    for line in lines:
        m = HEADING_RE.match(line) or SUMMARY_HEADING_RE.match(line)
        if m:
            out.append((m.group(1), m.group(2)))
    return out


def latest_released(lines: list[str]) -> str:
    dated = [v for v, d in section_headers(lines) if d and d != "UNRELEASED" and BARE_RE.match(v)]
    if not dated:
        raise SystemExit("ERROR: CHANGELOG.md has no dated '## [X.Y.Z] - YYYY-MM-DD' release")
    return max(dated, key=version_tuple)


def legacy_unreleased(lines: list[str]) -> tuple[int, int, str] | None:
    """Return (start, end, label) of the expanded UNRELEASED section, if any."""
    starts = block_starts(lines)
    for n, s in enumerate(starts):
        m = HEADING_RE.match(lines[s])
        if m and m.group(2) == "UNRELEASED":
            end = starts[n + 1] if n + 1 < len(starts) else len(lines)
            return s, end, m.group(1)
    return None


def merge_entries(groups: list[Entries]) -> Entries:
    merged: Entries = {}
    for g in groups:
        for category, bullets in g.items():
            merged.setdefault(category, []).extend(bullets)
    return merged


def render_section(heading: str, entries: Entries) -> list[str]:
    out = [heading, ""]
    for category in sorted(entries, key=category_key):
        if not entries[category]:
            continue
        out.append("### %s" % category)
        for bullet in entries[category]:
            out.extend(bullet)
        out.append("")
    return out


def collapse(block: list[str]) -> list[str]:
    """Turn an expanded ``## [X] - date`` section into the <details> form."""
    m = HEADING_RE.match(block[0])
    body = block[1:]
    while body and not body[0].strip():
        body.pop(0)
    while body and not body[-1].strip():
        body.pop()
    label = "[%s] - %s" % (m.group(1), m.group(2)) if m.group(2) else "[%s]" % m.group(1)
    return ["<details>", "<summary><strong>%s</strong></summary>" % label, ""] + body + ["", "</details>", ""]


def collect(lines: list[str], directory: Path) -> tuple[Entries, tuple[int, int, str] | None, int]:
    """Return (merged entries, legacy section span, fragment count)."""
    groups: list[Entries] = []
    legacy = legacy_unreleased(lines)
    if legacy:
        groups.append(parse_body(lines[legacy[0] + 1 : legacy[1]])[0])
    frags = load_fragments(directory)
    groups.extend(entries for _, entries in frags)
    return merge_entries(groups), legacy, len(frags)


def beta_bump_warning(current: str | None, branch: str | None) -> str | None:
    """Why a --bump with a beta VERSION on a branch other than stable/X.Y is suspect.

    Only stable/X.Y (and the release/ PR branch cut from it) carries a beta
    VERSION: a merge down keeps the receiving branch's VERSION and badge
    (docs/beta-channel.md), so a beta VERSION anywhere else means one was not
    kept, and a bump there proposes the version the stable branch owns. None
    when the VERSION is not a beta, the branch is a stable branch, or it is unknown.
    """
    if (not current or not is_beta(current) or branch in (None, "HEAD")
            or branch.startswith(("stable/", "release/"))):
        return None  # "HEAD": a detached checkout (CI); release/: the PR branch /bump-release cuts
    return ("WARNING: VERSION %s is a beta but this is branch %r, not stable/X.Y: the proposed version belongs "
            "to the stable branch, run /bump-release there (docs/beta-channel.md)" % (current, branch))


def current_branch() -> str | None:
    try:
        r = subprocess.run(["git", "rev-parse", "--abbrev-ref", "HEAD"], capture_output=True, text=True, check=False)
    except OSError:
        return None
    return r.stdout.strip() or None if r.returncode == 0 else None


def default_version_floor(changelog: Path) -> str | None:
    """The VERSION file beside the changelog, the floor --bump and --preview use without --version."""
    version_file = changelog.resolve().parent / "VERSION"
    if not version_file.is_file():
        return None
    text = version_file.read_text(encoding="utf-8").strip()
    if not VERSION_RE.match(text):
        # Say so: a silently dropped floor proposes a lower version than the one VERSION names.
        print("WARNING: %s holds %r, which is not X.Y.Z or X.Y.Z~betaN; it is not used as the version floor"
              % (version_file, text), file=sys.stderr)
        return None
    return text


def propose_bump(lines: list[str], directory: Path, current: str | None = None) -> str:
    base = latest_released(lines)
    fragment_entries = merge_entries([e for _, e in load_fragments(directory)])
    major, minor, patch = version_tuple(base)
    if any(c == "Breaking changes" or c.startswith("Breaking changes (") for c in fragment_entries):
        proposed = (major + 1, 0, 0)
    elif "Added" in fragment_entries:
        proposed = (major, minor + 1, 0)
    else:
        proposed = (major, minor, patch + 1)
    legacy = legacy_unreleased(lines)
    if legacy and BARE_RE.match(legacy[2]):
        proposed = max(proposed, version_tuple(legacy[2]))
    if current:
        # A beta VERSION (5.0.0~beta2) is a floor: its stable release is its base version.
        proposed = max(proposed, version_tuple(current))
    return ".".join(str(x) for x in proposed)


def assemble(text: str, directory: Path, version: str, date: str) -> str:
    """Return the new CHANGELOG.md text (pure; the caller deletes the fragments)."""
    # A beta never consumes the fragments (docs/beta-channel.md): /bump-release
    # beta mode skips this step, and stable mode writes VERSION X.Y.0 first.
    if not BARE_RE.match(version):
        raise SystemExit("ERROR: %r is not a bare X.Y.Z version (a beta is not released this way)" % version)
    if not DATE_RE.match(date):
        raise SystemExit("ERROR: %r is not a YYYY-MM-DD date" % date)
    try:
        datetime.date.fromisoformat(date)
    except ValueError as e:
        raise SystemExit("ERROR: %r is not a real date (%s)" % (date, e))
    lines = text.split("\n")
    if version_tuple(version) <= version_tuple(latest_released(lines)):
        raise SystemExit(
            "ERROR: %s is not greater than the latest dated release %s" % (version, latest_released(lines))
        )
    proposed = propose_bump(lines, directory, None)
    if version_tuple(version) < version_tuple(proposed):
        raise SystemExit("ERROR: %s is below the proposed bump %s for these fragments" % (version, proposed))
    entries, legacy, _ = collect(lines, directory)
    if not any(entries.values()):
        raise SystemExit("ERROR: nothing to release: no fragments and no UNRELEASED entries")
    if legacy:
        del lines[legacy[0] : legacy[1]]
    starts = block_starts(lines)
    first = starts[0] if starts else len(lines)
    if starts and HEADING_RE.match(lines[first]):
        end = starts[1] if len(starts) > 1 else len(lines)
        lines[first:end] = collapse(lines[first:end])
    section = render_section("## [%s] - %s" % (version, date), entries)
    lines[first:first] = section
    return "\n".join(lines)


# ------------------------------------------------------------------- self-test

FIXTURE = """# Changelog

Intro paragraph.

## [1.2.1] - UNRELEASED

### Fixed
- **Legacy fix** (core, issue #1): text
  continued here.

### Added (tests)
- **Legacy tests** (issue #1)

## [1.2.0] - 2026-01-02

### Added
- **Older** (issue #0)

<details>
<summary><strong>[1.1.0] - 2026-01-01</strong></summary>

### Added
- **Oldest** (issue #0)

</details>
"""


def _write(d: Path, name: str, body: str) -> None:
    d.mkdir(exist_ok=True)
    (d / name).write_text(body, encoding="utf-8")


def self_test() -> int:
    failures: list[str] = []

    def expect(cond: bool, msg: str) -> None:
        if not cond:
            failures.append(msg)

    # a line starting with inline code is no fence (CommonMark 4.5)
    inline = "### Fixed\n- x\n\n  ```foo``` and more\n"
    expect(
        validate("a.md", inline) == [],
        "inline code at the start of a line read as an opening fence",
    )
    # a fence indented 4+ past the bullet content is an indented code block
    indented = "### Fixed\n- x\n\n          ```\n"
    expect(
        validate("a.md", indented) == [],
        "indented code block line read as an opening fence",
    )
    # a fence under a nested bullet measures from that bullet's content column
    nested = "### Fixed\n- x\n  - y\n\n      ```\n      code\n      ```\n"
    expect(validate("a.md", nested) == [], "fence under a nested bullet read as an indented code block")
    # validation
    good = "### Fixed\n- **x** (issue #1)\n\n### Added (tests)\n- **y**\n  - more\n"
    expect(validate("a-b.c-d.md", good) == [], "valid fragment rejected")
    expect(any("file name" in p for p in validate("Bad Name.md", good)), "bad name accepted")
    expect(any("file name" in p for p in validate("-x.md", good)), "leading hyphen accepted")
    expect(any("expected 'fix-issue-12.md'" in p for p in validate("fix-Issue-12.md", good)), "uppercase name lacks lowercase hint")
    expect(validate("fix-issue-12.md", good) == [], "lowercase name rejected")
    expect(any("no '- ' bullet" in p for p in validate("a.md", "### Fixed\n\n")), "empty category accepted")
    expect(any("unknown category" in p for p in validate("a.md", "### Stuff\n- x\n")), "unknown category accepted")
    expect(any("unknown category" in p for p in validate("a.md", "### Added (x\n- x\n")), "open paren accepted")
    expect(any("wrapped" in p for p in validate("a.md", "### Fixed\n- **x**: one\n  two\n")), "wrapped bullet accepted")
    expect(any("wrapped" in p for p in validate("a.md", "### Fixed\n- x\n  - y\n    z\n")), "wrapped nested bullet accepted")
    one_line = "### Fixed\n- x\n  - y\n\n  second paragraph\n  ```\n  code\n  ```\n"
    expect(validate("a.md", one_line) == [], "nested bullet, paragraph or fence rejected as wrapped")
    expect(any("heading" in p for p in validate("a.md", "## [1.0.0] - x\n### Fixed\n- x\n")), "## heading accepted")
    expect(any("heading" in p for p in validate("a.md", "# T\n### Fixed\n- x\n")), "# heading accepted")
    expect(any("before the first" in p for p in validate("a.md", "text\n### Fixed\n- x\n")), "stray text accepted")
    expect(any("before its first" in p for p in validate("a.md", "### Fixed\nstray\n- x\n")), "text before first bullet accepted")
    expect(validate("a.md", "") != [], "empty fragment accepted")
    expect(
        any("unclosed" in p for p in validate("a.md", "### Fixed\n- x\n  ```\n  a\n\n### Added\n- y\n")),
        "unclosed fence accepted",
    )
    ok_fence = "### Fixed\n- x\n  ````\n  ```\n  ````\n- y\n  ~~~\n  a\n  ~~~\n"
    expect(validate("a.md", ok_fence) == [], "longer or tilde fence mis-tracked")
    expect(
        any("unclosed" in p for p in validate("a.md", "### Fixed\n- x\n  ````\n  ```\n- y\n")),
        "shorter fence closed a four-backtick fence",
    )
    fence_cases = (
        "### Fixed\n- y\n  ~~~\n### Added\n- z\n  ~~~\n- w\n  ```\n  ~~~\n  ```\n- v\n  ```\n  ```python\n  ```\n"
    )
    e, p = parse_body(fence_cases.splitlines())
    expect(
        p == [] and list(e) == ["Fixed"] and len(e["Fixed"]) == 3,
        "tilde, mixed-character or info-string fence mis-tracked",
    )

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        d = root / "changelog.d"
        d.mkdir()
        (d / "README.md").write_text("# readme\n", encoding="utf-8")
        with contextlib.redirect_stdout(io.StringIO()):
            expect(check(d) == 0, "check failed on an empty changelog.d")
            _write(d, "bad.md", "### Stuff\n- x\n")
            with contextlib.redirect_stderr(io.StringIO()):
                expect(check(d) == 1, "check passed on a bad fragment")
        (d / "bad.md").unlink()

        # bump rules
        lines = FIXTURE.split("\n")
        expect(propose_bump(lines, d) == "1.2.1", "legacy floor / patch from 1.2.0 expected 1.2.1")
        _write(d, "t.md", "### Added (tests)\n- x\n")
        expect(propose_bump(lines, d) == "1.2.1", "Added (tests) must count as patch")
        _write(d, "t.md", "### Added\n- x\n")
        expect(propose_bump(lines, d) == "1.3.0", "Added must count as minor")
        _write(d, "t.md", "### Breaking changes\n- x\n")
        expect(propose_bump(lines, d) == "2.0.0", "Breaking changes must count as major")
        _write(d, "t.md", "### Fixed\n- x\n")
        expect(propose_bump(lines, d) == "1.2.1", "Fixed must count as patch")
        floor = FIXTURE.replace("[1.2.1] - UNRELEASED", "[1.4.0] - UNRELEASED").split("\n")
        expect(propose_bump(floor, d) == "1.4.0", "legacy label must be a floor")
        (d / "t.md").unlink()

        # release on a fixture: legacy UNRELEASED + two fragments
        _write(d, "b-second.md", "### Fixed\n- **Second fix** (issue #3)\n\n### Security\n- **Sec** (issue #3)\n")
        _write(d, "a-first.md", "### Added\n- **First add** (issue #2)\n\n### Fixed\n- **First fix** (issue #2)\n")
        _write(
            d,
            "c-blank.md",
            "### Fixed\n- **Paragraphs** (issue #4)\n\n  Second paragraph.\n\n- **Fence** (issue #4)\n  ```\n  a\n\nb\n  ```\n\n\n",
        )
        new = assemble(FIXTURE, d, "1.3.0", "2026-02-03")
        out = new.split("\n")
        expect(
            "- **Paragraphs** (issue #4)\n\n  Second paragraph.\n- **Fence** (issue #4)\n  ```\n  a\n\nb\n  ```\n\n"
            in new,
            "blank lines inside a bullet or fence lost, or between-bullet blanks not collapsed",
        )
        expect("## [1.3.0] - 2026-02-03" in out, "new dated heading missing")
        expect(not any("UNRELEASED" in x for x in out), "UNRELEASED heading survived")
        expect(out.index("Intro paragraph.") + 2 == out.index("## [1.3.0] - 2026-02-03"), "section not under intro")
        expect(
            "<summary><strong>[1.2.0] - 2026-01-02</strong></summary>" in out
            and "## [1.2.0] - 2026-01-02" not in out,
            "previous release not collapsed",
        )
        i = out.index("<summary><strong>[1.2.0] - 2026-01-02</strong></summary>")
        expect(out[i - 1] == "<details>" and out[i + 1] == "", "collapse form wrong (blank line after summary)")
        expect(out[out.index("- **Older** (issue #0)") + 2] == "</details>", "collapse form wrong (blank before </details>)")
        sect = "\n".join(out[out.index("## [1.3.0] - 2026-02-03") : i - 1])
        order = [sect.index(x) for x in ("### Added\n", "### Added (tests)", "### Fixed", "### Security")]
        expect(order == sorted(order), "categories out of order")
        fixed = sect[sect.index("### Fixed") :]
        expect(
            fixed.index("Legacy fix") < fixed.index("First fix") < fixed.index("Second fix"),
            "entries not ordered legacy, then fragments by name",
        )
        expect("  continued here." in sect, "multi-line legacy bullet lost its continuation")
        expect("\n<details>\n<summary><strong>[1.1.0]" in new, "older collapsed section disturbed")
        expect(new.endswith("</details>\n"), "trailing newline changed")

        # refusals
        for ver, date, why in (
            ("1.2.0", "2026-02-03", "version not greater"),
            ("1.3", "2026-02-03", "bad version"),
            ("1.3.0", "2026-2-3", "bad date"),
        ):
            try:
                assemble(FIXTURE, d, ver, date)
                failures.append("release accepted: " + why)
            except SystemExit:
                pass
        try:
            assemble(FIXTURE, d, "1.2.5", "2026-02-03")
            failures.append("release accepted a version below the proposed bump")
        except SystemExit as e:
            expect("1.3.0" in str(e), "below-bump refusal does not name the proposed version: %s" % e)
        expect("## [1.4.0]" in assemble(FIXTURE, d, "1.4.0", "2026-02-03"), "a version above the proposal was refused")
        empty = root / "empty.d"
        empty.mkdir()
        try:
            assemble(FIXTURE.replace("### Fixed\n- **Legacy fix** (core, issue #1): text\n  continued here.\n", "")
                     .replace("### Added (tests)\n- **Legacy tests** (issue #1)\n", ""), empty, "1.3.0", "2026-02-03")
            failures.append("release accepted with nothing to release")
        except SystemExit:
            pass

        # beta VERSION spelling (~betaN)
        expect(VERSION_RE.match("5.0.0~beta2") and not VERSION_RE.match("5.0.0~rc1")
               and not VERSION_RE.match("5.0.0~beta") and not VERSION_RE.match("5.0~beta1")
               and not VERSION_RE.match("5.0.0~beta0") and not VERSION_RE.match("5.0.0~beta01"),
               "VERSION_RE does not accept exactly X.Y.Z[~betaN]")
        expect(base_version("5.0.0~beta2") == "5.0.0" and base_version("4.2.0") == "4.2.0", "base_version wrong")
        expect(is_beta("5.0.0~beta2") and not is_beta("5.0.0") and not is_beta("5.0.0~rc1")
               and not is_beta("5.0.0~beta0") and not is_beta("5.0.0~beta"), "is_beta wrong")
        # Embeddable: no capturing group, so a caller's group numbers cannot shift.
        expect(re.compile(VERSION_PATTERN).groups == 0 and re.compile(BETA_SUFFIX_PATTERN).groups == 0,
               "VERSION_PATTERN gained a capturing group")
        expect(propose_bump(FIXTURE.split("\n"), d, "1.5.0~beta2") == "1.5.0", "--bump on a beta VERSION is not its base")
        expect(propose_bump(FIXTURE.split("\n"), d, "1.0.0~beta1") == "1.3.0", "a lower beta base lowered the bump")
        expect(beta_bump_warning("5.0.0~beta2", "main") is not None
               and beta_bump_warning("5.0.0~beta2", "feature/x") is not None,
               "a beta VERSION off a stable branch did not warn")
        expect(beta_bump_warning("5.0.0~beta2", "stable/5.0") is None
               and beta_bump_warning("5.0.0", "main") is None
               and beta_bump_warning("5.0.0~beta2", None) is None
               and beta_bump_warning("5.0.0~beta2", "HEAD") is None
               and beta_bump_warning("5.0.0~beta2", "release/5.0.0-beta.2") is None
               and beta_bump_warning(None, "main") is None,
               "beta_bump_warning warned where it should not")
        try:
            assemble(FIXTURE, d, "1.4.0~beta2", "2026-02-03")
            expect(False, "--release on a beta VERSION was accepted (it would consume the fragments mid-beta)")
        except SystemExit as e:
            expect("not a bare X.Y.Z" in str(e), "--release on a beta VERSION: wrong error: %s" % e)
        expect([p.name for p in d.iterdir()] != ["README.md"], "fragments were deleted by the rejected --release")
        try:
            assemble(FIXTURE, d, "1.4.0", "2026-02-30")
            expect(False, "--release with an impossible date was accepted")
        except SystemExit as e:
            expect("not a real date" in str(e), "--release impossible date: wrong error: %s" % e)
        # --preview and --bump read the VERSION file beside the changelog as the floor
        # when --version is not given, so both propose the same version on a stable branch.
        (root / "VERSION").write_text("1.5.0~beta2\n", encoding="utf-8")
        expect(default_version_floor(root / "CHANGELOG.md") == "1.5.0~beta2", "VERSION beside the changelog not read")
        (root / "VERSION").unlink()
        expect(default_version_floor(root / "CHANGELOG.md") is None, "a missing VERSION file is not None")
        (root / "VERSION").write_text("1.5.0~rc1\n", encoding="utf-8")
        warned = io.StringIO()
        with contextlib.redirect_stderr(warned):
            floor_rc = default_version_floor(root / "CHANGELOG.md")
        expect(floor_rc is None and "not used as the version floor" in warned.getvalue(),
               "an unparseable VERSION was dropped as the floor without a warning")
        (root / "VERSION").unlink()

        # end to end through the CLI, then changelog_section.py reads the result
        cl = root / "CHANGELOG.md"
        cl.write_text(FIXTURE, encoding="utf-8")
        here = Path(__file__).resolve().parent
        # --version is validated on the CLI for --bump and --preview
        for mode, bad in (("--bump", "5.0.0~rc1"), ("--preview", "5.0~beta1"), ("--bump", "5.0.0~beta0")):
            rej = subprocess.run(
                [sys.executable, str(here / "changelog_fragments.py"), "--changelog", str(cl),
                 "--fragments", str(d), mode, "--version", bad],
                capture_output=True, text=True,
            )
            expect(rej.returncode != 0 and "is not X.Y.Z or X.Y.Z~betaN" in rej.stderr,
                   "%s --version %s was not rejected" % (mode, bad))
        for v, rc in (("5.0.0~beta2", 0), ("5.0.0", 1), ("5.0.0~beta0", 2), ("5.0.0~rc1", 2)):
            got = subprocess.run([sys.executable, str(here / "changelog_fragments.py"), "--is-beta", v],
                                 capture_output=True, text=True).returncode
            expect(got == rc, "--is-beta %s exited %d, not %d" % (v, got, rc))
        ok = subprocess.run(
            [sys.executable, str(here / "changelog_fragments.py"), "--changelog", str(cl),
             "--fragments", str(d), "--bump", "--version", "1.5.0~beta2"],
            capture_output=True, text=True,
        )
        expect(ok.returncode == 0 and ok.stdout.strip() == "1.5.0", "--bump --version 1.5.0~beta2 on the CLI: " + ok.stderr.strip())
        (root / "VERSION").write_text("1.5.0~beta2\n", encoding="utf-8")
        for mode, want in (("--bump", "1.5.0"), ("--preview", "## [1.5.0]")):
            floor = subprocess.run(
                [sys.executable, str(here / "changelog_fragments.py"), "--changelog", str(cl), "--fragments", str(d), mode],
                capture_output=True, text=True,
            )
            expect(floor.returncode == 0 and floor.stdout.splitlines()[0].strip() == want,
                   "%s without --version ignored the VERSION file beside the changelog: %r" % (mode, floor.stdout[:40]))
        (root / "VERSION").unlink()
        r = subprocess.run(
            [sys.executable, str(here / "changelog_fragments.py"), "--changelog", str(cl),
             "--fragments", str(d), "--release", "1.3.0", "--date", "2026-02-03"],
            capture_output=True, text=True,
        )
        expect(r.returncode == 0, "--release failed: " + r.stderr.strip())
        expect([p.name for p in d.iterdir()] == ["README.md"], "fragments not deleted (or README.md was)")
        s = subprocess.run(
            [sys.executable, str(here / "changelog_section.py"), "1.3.0", "--changelog", str(cl)],
            capture_output=True, text=True,
        )
        expect(s.returncode == 0 and "First add" in s.stdout and "Legacy fix" in s.stdout,
               "changelog_section.py did not extract the new section: " + s.stderr.strip())
        again = subprocess.run(
            [sys.executable, str(here / "changelog_fragments.py"), "--changelog", str(cl),
             "--fragments", str(d), "--release", "1.4.0", "--date", "2026-02-04"],
            capture_output=True, text=True,
        )
        expect(again.returncode != 0, "a second release with nothing to release succeeded")

    for f in failures:
        print("SELF-TEST FAIL: %s" % f, file=sys.stderr)
    print("changelog_fragments self-test %s." % ("FAILED" if failures else "OK"))
    return 1 if failures else 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        epilog="Fragment format: changelog.d/README.md.",
        formatter_class=lambda prog: argparse.HelpFormatter(prog, max_help_position=28),
        add_help=False,
    )
    # Help sections print in group-creation order: modes first, then options.
    modes = ap.add_argument_group("mode (exactly one)")
    opts = ap.add_argument_group("options")
    opts.add_argument("-h", "--help", action="help", help="show this help message and exit")
    mode = modes.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true", help="validate every fragment (CI and pre-flight)")
    mode.add_argument(
        "--preview", action="store_true", help="print the section --release would write, without a date"
    )
    mode.add_argument(
        "--bump",
        action="store_true",
        help="print the proposed next version: major for Breaking changes, minor for an unqualified Added, "
        "else patch",
    )
    mode.add_argument(
        "--release",
        metavar="X.Y.Z",
        help="write the dated X.Y.Z section to the changelog and delete the fragments (needs --date)",
    )
    mode.add_argument(
        "--is-beta",
        metavar="VERSION",
        help="exit 0 if VERSION is X.Y.Z~betaN, 1 if it is a bare X.Y.Z, 2 otherwise (for shell scripts)",
    )
    mode.add_argument("--self-test", action="store_true", help="run the script's built-in tests")
    opts.add_argument(
        "--changelog", default="CHANGELOG.md", type=Path, metavar="PATH", help="changelog file (default: %(default)s)"
    )
    opts.add_argument(
        "--fragments", default="changelog.d", type=Path, metavar="DIR", help="fragment directory (default: %(default)s)"
    )
    opts.add_argument(
        "--version", metavar="X.Y.Z", help="with --preview: version for the heading (default: the --bump result); with --bump: the current "
        "VERSION, a beta counts as its base version (default: the VERSION file beside the changelog)"
    )
    opts.add_argument("--date", metavar="YYYY-MM-DD", help="with --release: release date (required)")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if args.is_beta is not None:
        if is_beta(args.is_beta):
            return 0
        if BARE_RE.match(args.is_beta):
            return 1
        print("ERROR: %r is not X.Y.Z or X.Y.Z~betaN" % args.is_beta, file=sys.stderr)
        return 2
    if args.check:
        return check(args.fragments)

    text = args.changelog.read_text(encoding="utf-8")
    lines = text.split("\n")
    problems = [
        "%s: %s" % (p, pr) for p in fragment_files(args.fragments) for pr in validate(p.name, p.read_text(encoding="utf-8"))
    ]
    if problems:
        print("\n".join(problems), file=sys.stderr)
        print("ERROR: fix the fragments first (--check)", file=sys.stderr)
        return 1
    entries, _, _ = collect(lines, args.fragments)
    if not any(entries.values()):
        print("ERROR: nothing to release: no fragments and no UNRELEASED entries", file=sys.stderr)
        return 1

    if args.bump:
        if args.version and not VERSION_RE.match(args.version):
            ap.error("--version %r is not X.Y.Z or X.Y.Z~betaN" % args.version)
        current = args.version or default_version_floor(args.changelog)
        warning = beta_bump_warning(current, current_branch())
        if warning:
            print(warning, file=sys.stderr)
        print(propose_bump(lines, args.fragments, current))
        return 0
    if args.preview:
        if args.version and not VERSION_RE.match(args.version):
            ap.error("--version %r is not X.Y.Z or X.Y.Z~betaN" % args.version)
        # Same floor as --bump, so the preview shows the section Step 2 writes.
        version = args.version or propose_bump(lines, args.fragments, default_version_floor(args.changelog))
        print("\n".join(render_section("## [%s]" % version, entries)).rstrip())
        return 0

    if not args.date:
        ap.error("--release requires --date YYYY-MM-DD")
    new = assemble(text, args.fragments, args.release, args.date)
    args.changelog.write_text(new, encoding="utf-8")
    for p in fragment_files(args.fragments):
        p.unlink()
    print("Wrote [%s] - %s to %s" % (args.release, args.date, args.changelog))
    return 0


if __name__ == "__main__":
    sys.exit(main())
