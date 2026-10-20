#!/usr/bin/env python3
"""Map a pull request's changed files to the decision-record rules they fall under.

Every decision record in docs/decisions/ has a `## Rules` section whose numbered
rules each name a `Scope:` of repository globs (docs/decisions/README.md). A
changed file is under a rule when one of the rule's globs matches it.

Two modes:

  --list   reads changed paths from stdin, one per line, and prints one line
           per record in scope, `<record file>: rules <n>, <n>`. The review
           bot runs it from the base branch and reads those rules against the
           diff (.github/workflows/claude-review.yml).

  default  the PR-body gate: the `## Decision records` section of the PR body
           (read from PR_BODY or --body-file, never the command line) must name,
           by its four-digit number, every decision record the PR adds, edits,
           renames or deletes. It also prints the records in scope, for the
           author. The diff base is resolved as check_falsified_by.py does it.

Usage:
    check_adr_scope.py --list [--root DIR] < paths
    check_adr_scope.py [--body-file FILE] [--base REV]
    check_adr_scope.py --self-test
"""

import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_docs_drift import (  # noqa: E402  (the one owner of the Rules grammar)
    DECISION_DIR, DECISION_FILE_RE, DECISION_RULE_RE, CODE_SPAN_ONLY_RE, _decision_sections, _glob_match)

ROOT = Path(__file__).resolve().parent.parent
SECTION = "decision records"
RECORD_NUMBER_RE = re.compile(r"\b(\d{4})\b")
HEADING_RE = re.compile(r"^##\s+(.+?)\s*$")
COMMENT_RE = re.compile(r"<!--.*?-->", re.S)


def record_rules(records):
    """{record file: [(rule number, [glob])]} for every record that is not superseded."""
    out = {}
    for name, text in sorted(records.items()):
        if not DECISION_FILE_RE.match(name) or re.search(r"^Status: superseded by", text, re.M):
            continue
        rules = dict(_decision_sections(text)).get("Rules", "")
        parsed = []
        for line in rules.splitlines():
            m = DECISION_RULE_RE.match(line)
            if m:
                parsed.append((int(m.group("n")), CODE_SPAN_ONLY_RE.findall(m.group("scope"))))
        out[name] = parsed
    return out


def in_scope(changed, rules_by_record):
    """[(record file, [rule number])] for every record with a rule a changed path falls under."""
    result = []
    for record, rules in rules_by_record.items():
        hit = [n for n, globs in rules if any(_glob_match(p, g) for p in changed for g in globs)]
        if hit:
            result.append((record, hit))
    return result


def body_section(body, title=SECTION):
    """The lines of the body's `## <title>` section, or None when it is missing."""
    lines, inside = None, False
    for line in COMMENT_RE.sub("", body.replace("\r\n", "\n")).split("\n"):
        m = HEADING_RE.match(line)
        if m:
            inside = " ".join(m.group(1).lower().split()) == title
            if inside and lines is None:
                lines = []
            continue
        if inside:
            lines.append(line)
    return lines


def body_findings(body, changed):
    section = body_section(body)
    records = sorted({p for p in changed if p.startswith(DECISION_DIR + "/") and DECISION_FILE_RE.match(p.split("/")[-1])})
    if section is None:
        return ["the PR body has no '## Decision records' section"]
    named = set(RECORD_NUMBER_RE.findall("\n".join(section)))
    return ["the '## Decision records' section does not name %s, which this PR changes" % p
            for p in records if p.split("/")[-1][:4] not in named]


def read_records(root):
    directory = Path(root) / DECISION_DIR
    return {p.name: p.read_text(encoding="utf-8", errors="replace") for p in directory.glob("0*.md")}


def changed_since(base):
    out = subprocess.run(["git", "-C", str(ROOT), "diff", "--name-status", "-M", base, "HEAD"],
                         capture_output=True, text=True, check=True).stdout
    paths = []
    for line in out.splitlines():
        parts = line.split("\t")
        paths.extend(parts[1:])
    return paths


def main(argv):
    if "--self-test" in argv:
        return self_test()
    args = iter(argv)
    root, body_file, base = ROOT, None, None
    for arg in args:
        if arg == "--root":
            root = Path(next(args, ""))
        elif arg == "--body-file":
            body_file = next(args, None)
        elif arg == "--base":
            base = next(args, None)
    rules = record_rules(read_records(root))
    if not rules:
        print("check_adr_scope: no decision record found under %s" % (Path(root) / DECISION_DIR), file=sys.stderr)
        return 2
    if "--list" in argv:
        changed = [line.strip() for line in sys.stdin if line.strip()]
        for record, hit in in_scope(changed, rules):
            print("%s: rules %s" % (record, ", ".join(str(n) for n in hit)))
        return 0
    from check_falsified_by import resolve_base
    changed = changed_since(resolve_base(base or os.environ.get("FALSIFIED_BASE")))
    if body_file:
        body = Path(body_file).read_text(encoding="utf-8")
    else:
        body = os.environ.get("PR_BODY", "")
    for record, hit in in_scope(changed, rules):
        print("in scope: %s rules %s" % (record, ", ".join(str(n) for n in hit)))
    errors = body_findings(body, changed)
    for e in errors:
        print("decision-records: " + e)
    if errors:
        return 1
    print("Decision records section names every record this PR changes.")
    return 0


def self_test():
    failures = []

    def expect(cond, msg):
        if not cond:
            failures.append(msg)

    record = ("# R\n\nStatus: accepted\n\n## Context\n\n## Decision\n\n## Rules\n\n"
              "1. A. Scope: `AlpacaCore/src/vendors/*/*_driver.cpp`. Checked by: review.\n"
              "2. B. Scope: `AlpacaHTTP/**`, `docs/x.md`. Checked by: review.\n\n## Links\n")
    superseded = record.replace("Status: accepted", "Status: superseded by 0001")
    rules = record_rules({"0001-a.md": record, "0002-b.md": superseded, "README.md": record})
    expect(list(rules) == ["0001-a.md"], "superseded records and the index are skipped: %r" % list(rules))
    expect(in_scope(["AlpacaCore/src/vendors/zwo/zwo_camera_driver.cpp"], rules) == [("0001-a.md", [1])],
           "a driver file falls under rule 1")
    expect(in_scope(["AlpacaHTTP/src/http/router.cpp", "docs/x.md"], rules) == [("0001-a.md", [2])],
           "an HTTP file falls under rule 2 only")
    expect(in_scope(["AlpacaCore/src/vendors/zwo/sub/x_driver.cpp"], rules) == [],
           "a single star does not cross a directory")
    expect(in_scope(["README.md"], rules) == [], "a file outside every scope is in no record")

    changed = ["docs/decisions/0005-task-clock.md", "scripts/x.py"]
    good = "## Decision records\n\n- 0005 (adds a row)\n"
    expect(body_findings(good, changed) == [], "a body naming the changed record passes")
    expect(body_findings("## What Changed\n- x\n", changed) == ["the PR body has no '## Decision records' section"],
           "a missing section is reported")
    expect(any("0005-task-clock.md" in e for e in body_findings("## Decision records\n\n- None\n", changed)),
           "a section that does not name a changed record is reported")
    expect(body_findings("## Decision records\n\nNone.\n", ["scripts/x.py"]) == [],
           "a PR that changes no record may say None")
    expect(any("0005" in e for e in body_findings("## Decision records\n<!-- 0005 -->\nNone\n", changed)),
           "a record named only inside an HTML comment does not count")
    expect(body_findings("## Decision records\n\n- docs/decisions/README.md\n",
                         ["docs/decisions/README.md"]) == [], "the index is not a record")

    for f in failures:
        print("FAIL: " + f)
    if failures:
        print("%d check_adr_scope self-test(s) failed." % len(failures))
        return 1
    print("check_adr_scope self-test passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
