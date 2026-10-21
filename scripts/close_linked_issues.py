#!/usr/bin/env python3
"""Close the issues a merged stable-branch PR names with a closing keyword.

GitHub acts on `Closes #N` only when a PR merges into the default branch, and
fixes for the beta channel merge into stable/X.Y (docs/beta-channel.md). The
workflow .github/workflows/close-linked-issues.yml runs this script for a PR
merged into stable/**.

The PR body is untrusted text: it is read from an environment variable, parsed
as data, and only integers reach `gh`.

Closing keywords (case-insensitive, optional colon): close, closes, closed,
fix, fixes, fixed, resolve, resolves, resolved, followed by `#N`,
`<repo>#N` or the full issue URL of this repository. Other repositories and
`Refs #N` are ignored.

Usage:
  close_linked_issues.py --repo OWNER/NAME --pr N --base BRANCH --sha SHA
                         [--body-env VAR] [--dry-run]
  close_linked_issues.py --self-test
"""
import argparse
import json
import os
import re
import subprocess
import sys

KEYWORD = r"(?:close[sd]?|fix(?:e[sd])?|resolve[sd]?)"
REF = (
    r"(?:https://github\.com/(?P<urepo>[\w.-]+/[\w.-]+)/issues/(?P<unum>\d+)"
    r"|(?P<repo>[\w.-]+/[\w.-]+)?#(?P<num>\d+))"
)
CLOSING_RE = re.compile(r"(?<![\w-])" + KEYWORD + r":?[ \t]+" + REF + r"(?![\w/-])", re.IGNORECASE)


def parse_closing_issues(body, repo):
    """Sorted unique issue numbers of `repo` that `body` closes with a keyword."""
    want = repo.lower()
    found = set()
    for m in CLOSING_RE.finditer(body or ""):
        if m.group("unum"):
            ref_repo, num = m.group("urepo"), m.group("unum")
        else:
            ref_repo, num = m.group("repo") or repo, m.group("num")
        if ref_repo.lower() == want:
            found.add(int(num))
    return sorted(found)


def gh(*args):
    return subprocess.run(["gh", *args], capture_output=True, text=True)


def close_one(repo, number, pr, base, sha, dry_run):
    """(ok, message) for one issue; an already-closed issue or a PR is skipped."""
    view = gh("api", "repos/%s/issues/%d" % (repo, number))
    if view.returncode != 0:
        return False, "lookup failed: " + view.stderr.strip()
    info = json.loads(view.stdout)
    if "pull_request" in info:
        return True, "skipped, it is a pull request"
    if info.get("state") != "open":
        return True, "skipped, already closed"
    if dry_run:
        return True, "would close"
    comment = "Fixed by #%d on %s (merge %s)." % (pr, base, sha[:7])
    res = gh("issue", "close", str(number), "--repo", repo, "--reason", "completed", "--comment", comment)
    if res.returncode != 0:
        return False, "close failed: " + res.stderr.strip()
    return True, "closed"


def run(args):
    body = os.environ.get(args.body_env, "")
    numbers = parse_closing_issues(body, args.repo)
    if not numbers:
        print("No closing keyword names an issue of %s; nothing to close." % args.repo)
        return 0
    failed = 0
    for n in numbers:
        ok, msg = close_one(args.repo, n, args.pr, args.base, args.sha, args.dry_run)
        print("#%d: %s" % (n, msg))
        failed += not ok
    return 1 if failed else 0


def self_test():
    repo = "open-astro/AlpacaBridge"
    cases = [
        ("Closes #12", [12]),
        ("close #1, closed #2, fix #3, fixes #4, fixed #5, resolve #6, resolves #7, resolved #8",
         [1, 2, 3, 4, 5, 6, 7, 8]),
        ("CLOSES #9 and Fixes #10", [9, 10]),
        ("Fixes: #11", [11]),
        ("Closes open-astro/AlpacaBridge#13", [13]),
        ("closes OPEN-ASTRO/alpacabridge#14", [14]),
        ("Fixes https://github.com/open-astro/AlpacaBridge/issues/15", [15]),
        ("Refs #16", []),
        ("See #17, related to #18", []),
        ("Closes other/repo#19", []),
        ("Fixes https://github.com/other/repo/issues/20", []),
        ("Closes https://github.com/open-astro/AlpacaBridge/pull/21", []),
        ("Closes #22\nCloses #22\nfixes open-astro/AlpacaBridge#22", [22]),
        ("Closes #23x", []),
        ("prefixes #24, unclosed #25", []),
        ("Closes #30 #31", [30]),
        ("", []),
        (None, []),
    ]
    bad = 0
    for body, want in cases:
        got = parse_closing_issues(body, repo)
        if got != want:
            bad += 1
            print("FAIL %r: got %r, want %r" % (body, got, want))
    if bad:
        return 1
    print("close_linked_issues self-test: %d cases passed" % len(cases))
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--self-test", action="store_true")
    p.add_argument("--repo")
    p.add_argument("--pr", type=int)
    p.add_argument("--base")
    p.add_argument("--sha", default="")
    p.add_argument("--body-env", default="PR_BODY")
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args()
    if args.self_test:
        return self_test()
    if not (args.repo and args.pr and args.base):
        p.error("--repo, --pr and --base are required")
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
