#!/usr/bin/env python3
"""Map a pull request's changed files to the decision-record rules they fall under.

Every decision record in docs/decisions/ has a `## Rules` section whose numbered
rules each name a `Scope:` of repository globs (docs/decisions/README.md). A
changed file is under a rule when one of the rule's globs matches it.

Two modes:

  --list   reads changed paths from stdin, one per line, and prints one line
           per record in scope, `<record file>: rules <n>, <n>`. The review
           bot runs the default branch's copy over the base branch's records
           and reads those rules against the diff
           (.github/workflows/claude-review.yml).

  default  the PR-body gate: the `## Decision records` section of the PR body
           (read from PR_BODY or --body-file, never the command line) must name,
           by its four-digit number, every decision record the PR adds, edits,
           renames or deletes. It also prints the records in scope, for the
           author. The diff base is resolved as check_falsified_by.py does it.

  --checklist SCOPE REVIEW
           the review bot's gate before it posts: SCOPE holds the --list
           output ("none" when nothing is in scope) and REVIEW the bot's
           review-comment.md. Every listed rule needs a checklist line
           `- <record file> rule <n>: pass|defect|n/a: <reason>`, a rule may
           not get two verdicts, and a defect line needs the "Issues found"
           sign-off as the last line (bold, italics or a closing period or
           exclamation mark around it still count). Any finding exits 1, so nothing posts.

Usage:
    check_adr_scope.py --list [--root DIR] < paths
    check_adr_scope.py --checklist SCOPE_FILE REVIEW_FILE
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
# A record number, not an issue or PR number such as #1415 (#1416 review).
RECORD_NUMBER_RE = re.compile(r"(?<![#\d])\b(\d{4})\b")
HEADING_RE = re.compile(r"^##\s+(.+?)\s*$")
COMMENT_RE = re.compile(r"<!--.*?-->", re.S)
SCOPE_LINE_RE = re.compile(r"^(\d{4}-[a-z0-9-]+\.md): rules (\d+(?:, \d+)*)$")
# Consumers: the workflow step "Fold the decision-record checklist" in .github/workflows/claude-review.yml
# imports fold_checklist (and through it CHECKLIST_RE, sign_off and the sign-off constants); rename them there too.
CHECKLIST_RE = re.compile(r"^\s*[-*]\s*`?(\d{4}-[a-z0-9-]+\.md)`?\s+rule\s+(\d+)\s*:\s*(pass|defect|n/a)\s*:\s*\S", re.I)
ISSUES_SIGN_OFF = "\u26a0\ufe0f Issues found"
APPROVED_SIGN_OFF = "\u2705 Approved"


def sign_off(line):
    """The line without the Markdown emphasis or closing period a sign-off may carry."""
    return line.strip().strip("*_ ").rstrip(".!").strip("*_ ")


def fold_checklist(text):
    """The review with its checklist folded into <details>, or the text unchanged when it has no checklist lines."""
    lines = text.split("\n")
    start = next((i for i, l in enumerate(lines) if l.strip().lower() == "## " + SECTION), None)
    if start is None:
        return text
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("## ")), len(lines))
    last = max((i for i, l in enumerate(lines) if l.strip()), default=-1)
    if start < last < end and sign_off(lines[last]) in (APPROVED_SIGN_OFF, ISSUES_SIGN_OFF):
        end = last  # a real sign-off stays outside the fold
    verdicts = [m.group(3).lower() for l in lines[start + 1:end] if (m := CHECKLIST_RE.match(l))]
    if not verdicts:
        return text
    counts = ", ".join("%d %s" % (verdicts.count(v), v) for v in ("defect", "pass", "n/a") if v in verdicts)
    summary = "%d rule%s: %s" % (len(verdicts), "" if len(verdicts) == 1 else "s", counts)
    body = "\n".join(lines[start + 1:end]).strip("\n")
    folded = ["## Decision records", "", "<details><summary>%s</summary>" % summary, "", body, "", "</details>", ""]
    return "\n".join(lines[:start] + folded + lines[end:])


def record_rules(records):
    """{record file: [(rule number, [glob])]} for every record that is not superseded or retired."""
    out = {}
    for name, text in sorted(records.items()):
        if not DECISION_FILE_RE.match(name) or re.search(r"^Status: (superseded by|retired)", text, re.M):
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


def scope_pairs(scope):
    """({(record file, rule number)}, [malformed line]) from --list output, where "none..." lists nothing."""
    pairs, bad = set(), []
    for line in scope.splitlines():
        line = line.strip()
        if not line or line.lower().startswith("none"):
            continue
        m = SCOPE_LINE_RE.match(line)
        if not m:
            bad.append(line)
            continue
        pairs.update((m.group(1), int(n)) for n in m.group(2).split(", "))
    return pairs, bad


def checklist_findings(scope, review):
    """What keeps a review from posting: a listed rule with no verdict, two verdicts, or a defect signed off clean."""
    expected, bad = scope_pairs(scope)
    findings = ["the rule scope has a line that is not `<record file>: rules <n>`: %r" % line for line in bad]
    verdicts = {}
    for line in review.splitlines():
        m = CHECKLIST_RE.match(line)
        if m:
            verdicts.setdefault((m.group(1), int(m.group(2))), set()).add(m.group(3).lower())
    findings += ["the review has no checklist line for %s rule %d" % key for key in sorted(expected - set(verdicts))]
    findings += ["the review gives %s rule %d more than one verdict: %s" % (key + (", ".join(sorted(v)),))
                 for key, v in sorted(verdicts.items()) if len(v) > 1]
    lines = [line.strip() for line in review.splitlines() if line.strip()]
    last = lines[-1] if lines else ""
    # Markdown emphasis or a closing period around the sign-off is still the sign-off.
    bare = sign_off(last)
    findings += ["the review marks %s rule %d as a defect but its last line is %r, not the Issues-found sign-off"
                 % (key + (last,)) for key, v in sorted(verdicts.items()) if "defect" in v and bare != ISSUES_SIGN_OFF]
    return findings


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
    if argv[:1] == ["--checklist"]:
        if len(argv) != 3:
            print("usage: check_adr_scope.py --checklist SCOPE_FILE REVIEW_FILE", file=sys.stderr)
            return 2
        scope, review = (Path(a).read_text(encoding="utf-8", errors="replace") for a in argv[1:])
        findings = checklist_findings(scope, review)
        for f in findings:
            print("adr-checklist: " + f)
        return 1 if findings else 0
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
    marked = record.replace("Checked by: review.\n2.", "Checked by: review. <!-- allowed -->\n2.")
    marked_rules = record_rules({"0001-a.md": marked})
    expect(in_scope(["AlpacaCore/src/vendors/zwo/zwo_camera_driver.cpp"], marked_rules) == [("0001-a.md", [1])],
           "a rule line ending in the allowed marker stays in scope")
    retired = record.replace("Status: accepted", "Status: retired (#1)")
    expect(list(record_rules({"0001-a.md": record, "0002-b.md": retired})) == ["0001-a.md"],
           "retired records are skipped")

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
    expect(any("0005" in e for e in body_findings("## Decision records\n\n- from the #0005 review\n", changed)),
           "an issue or PR number (#NNNN) does not name a record")
    expect(body_findings("## Decision records\n\n- 0005, from the #1415 review\n", changed) == [],
           "a record named beside an issue number still counts")
    expect(body_findings("## Decision records\n\n- docs/decisions/README.md\n",
                         ["docs/decisions/README.md"]) == [], "the index is not a record")

    ok = APPROVED_SIGN_OFF
    scope = "0003-docs-drift-gates.md: rules 3\n0005-task-clock.md: rules 1, 2\n"
    full = ("## Defects\nnone\n\n## Decision records\n"
            "- 0003-docs-drift-gates.md rule 3: pass: no gate text changed\n"
            "- `0005-task-clock.md` rule 1: n/a: no driver file changed\n"
            "- 0005-task-clock.md rule 2: n/a: the ZWO telescope row is planned\n\n" + ok + "\n")
    expect(checklist_findings(scope, full) == [], "a checklist naming every listed rule passes")
    expect(checklist_findings("none\n", "## Defects\nnone\n\n" + ok) == [], "an empty scope needs no lines")
    expect(checklist_findings("none (the base branch has no decision records)\n", ok) == [],
           "a 'none (...)' scope needs no lines")
    f = checklist_findings(scope, full.replace("- 0005-task-clock.md rule 2: n/a: the ZWO telescope row is planned\n", ""))
    expect(f == ["the review has no checklist line for 0005-task-clock.md rule 2"], "a missing rule is reported: %r" % f)
    f = checklist_findings(scope, full.replace("rule 2: n/a: the ZWO telescope row is planned", "rule 2: n/a"))
    expect(any("rule 2" in x for x in f), "a verdict with no reason does not count")
    f = checklist_findings(scope, full.replace("rule 2: n/a", "rule 2: defect"))
    expect(any("as a defect" in x for x in f), "a defect under an Approved sign-off is reported")
    issues = full.replace("rule 2: n/a", "rule 2: defect").replace(ok, ISSUES_SIGN_OFF)
    expect(checklist_findings(scope, issues) == [], "a defect under the Issues-found sign-off passes")
    for decorated in ("**%s**", "%s.", "_%s_", "  %s  "):
        f = checklist_findings(scope, issues.replace(ISSUES_SIGN_OFF, decorated % ISSUES_SIGN_OFF))
        expect(f == [], "a decorated Issues-found sign-off %r still counts: %r" % (decorated, f))
    f = checklist_findings(scope, issues.replace(ISSUES_SIGN_OFF, "Issues found, but " + ok))
    expect(any("as a defect" in x for x in f), "a last line that only contains the words is not the sign-off")
    f = checklist_findings(scope, full + "- 0003-docs-drift-gates.md rule 3: defect: x\n" + ok + "\n")
    expect(any("more than one verdict" in x for x in f), "two verdicts for one rule are reported")
    f = checklist_findings(scope + "rm -rf /\n", full)
    expect(any("not `<record file>" in x for x in f), "a malformed scope line is reported")
    expect(checklist_findings(scope, "0005-task-clock.md rule 2 passes, and so do the rest\n" + ok) != [],
           "prose that mentions a rule is not a checklist line")

    repro = ("## Defects\n\nnone\n\n## Decision records\n\n- 0003-docs-drift-gates.md rule 1: pass: a\n"
             "- 0005-task-clock.md rule 2: n/a: b\n")
    out = fold_checklist(repro)
    expect("<summary>2 rules: 1 pass, 1 n/a</summary>" in out and out.rstrip("\n").endswith("</details>"),
           "with no sign-off the last checklist line is folded and counted: %r" % out)
    for decorated in ("%s", "**%s**", "%s.", "_%s_"):
        for mark in (ok, ISSUES_SIGN_OFF):
            out = fold_checklist(repro + "\n" + decorated % mark + "\n")
            expect("<summary>2 rules: 1 pass, 1 n/a</summary>" in out and out.rstrip("\n").endswith(decorated % mark)
                   and out.index("</details>") < out.index(mark), "a sign-off %r stays outside the fold: %r" % (decorated % mark, out))
    expect(fold_checklist("## Decision records\n\nnone\n") == "## Decision records\n\nnone\n",
           "a review with no checklist lines is unchanged")

    for f in failures:
        print("FAIL: " + f)
    if failures:
        print("%d check_adr_scope self-test(s) failed." % len(failures))
        return 1
    print("check_adr_scope self-test passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
