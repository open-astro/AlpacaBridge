#!/usr/bin/env python3
"""Require a collaborator's explicit authorization for a change to a decision record.

The decision records in docs/decisions/ are the rules the decision-record gate
holds every pull request to, so changing one changes the rules. A PR that adds,
edits, renames or deletes a file under docs/decisions/ passes this check only
when a repository collaborator with write, maintain or admin permission has
posted, as a PR comment or a review, a line that reads exactly

    ADR authorized <head sha>

where <head sha> is the PR's current head commit (7 to 40 hex digits of it).
Naming the commit binds the authorization to the content: a later push changes
the head, and the PR needs a new authorization. The author may authorize their
own PR; the authorization is still an explicit act on the record. A PR that
touches no decision record passes at once.

The result is posted as the commit status `adr-authorization` on the head, so
the check can be re-evaluated by a comment event as well as by a push. The
workflow (.github/workflows/decision-records.yml) runs this file from the base
branch, never from the PR.

Usage:
    check_adr_authorization.py --repo OWNER/NAME --pr N [--post-status]
    check_adr_authorization.py --self-test
"""

import json
import re
import subprocess
import sys

CONTEXT = "adr-authorization"
DECISION_PREFIX = "docs/decisions/"
AUTHORIZE_RE = re.compile(r"^[ \t]*ADR authorized ([0-9a-fA-F]{7,40})[ \t]*$", re.M)
AUTHORIZING_PERMISSIONS = {"admin", "maintain", "write"}
# GitHub limits a commit status description to 140 characters.
MAX_DESCRIPTION = 140


def decide(changed, head_sha, statements, permission_of):
    """Return (state, description) for one PR.

    changed: every path the PR adds, edits, deletes or renames (old and new
    names). statements: [(login, body)] for every PR comment and review.
    permission_of: login -> the login's repository permission, or None.
    """
    records = sorted({p for p in changed if p.startswith(DECISION_PREFIX)})
    if not records:
        return "success", "No decision record changed"
    head = head_sha.lower()
    for login, body in statements:
        if not login or login.endswith("[bot]"):
            continue
        for sha in AUTHORIZE_RE.findall(body or ""):
            if head.startswith(sha.lower()) and permission_of(login) in AUTHORIZING_PERMISSIONS:
                return "success", ("Decision record change authorized by @%s for %s" % (login, head[:12]))[:MAX_DESCRIPTION]
    return "failure", ("%d decision record file(s) changed: a collaborator must comment 'ADR authorized %s'"
                       % (len(records), head[:12]))[:MAX_DESCRIPTION]


def gh(*args):
    r = subprocess.run(["gh", *args], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("gh %s failed: %s" % (" ".join(args), r.stderr.strip()))
    return r.stdout


def gh_list(path):
    """Every item of a paginated list endpoint, one JSON object per line."""
    out = gh("api", "--paginate", path, "--jq", ".[] | tojson")
    return [json.loads(line) for line in out.splitlines() if line.strip()]


def permission_lookup(repo):
    cache = {}

    def permission_of(login):
        if login not in cache:
            r = subprocess.run(["gh", "api", "repos/%s/collaborators/%s/permission" % (repo, login),
                                "--jq", ".permission"], capture_output=True, text=True)
            # 404 for a user with no access: not a collaborator.
            cache[login] = r.stdout.strip() if r.returncode == 0 else None
        return cache[login]

    return permission_of


def run(repo, pr, post_status):
    head_sha = gh("api", "repos/%s/pulls/%s" % (repo, pr), "--jq", ".head.sha").strip()
    changed = []
    for f in gh_list("repos/%s/pulls/%s/files?per_page=100" % (repo, pr)):
        changed.append(f.get("filename", ""))
        if f.get("previous_filename"):
            changed.append(f["previous_filename"])
    statements = [(c.get("user", {}).get("login", ""), c.get("body", ""))
                  for c in gh_list("repos/%s/issues/%s/comments?per_page=100" % (repo, pr))]
    statements += [(r.get("user", {}).get("login", ""), r.get("body", ""))
                   for r in gh_list("repos/%s/pulls/%s/reviews?per_page=100" % (repo, pr))]
    state, description = decide(changed, head_sha, statements, permission_lookup(repo))
    print("%s: %s on %s: %s" % (CONTEXT, state, head_sha[:12], description))
    if post_status:
        gh("api", "-X", "POST", "repos/%s/statuses/%s" % (repo, head_sha),
           "-f", "state=%s" % state, "-f", "context=%s" % CONTEXT, "-f", "description=%s" % description)
    return 0


def self_test():
    failures = []

    def expect(cond, msg):
        if not cond:
            failures.append(msg)

    head = "0123456789abcdef0123456789abcdef01234567"
    perms = {"maint": "maintain", "writer": "write", "admin1": "admin", "reader": "read", "triager": "triage"}
    permission_of = perms.get
    rec = ["docs/decisions/0005-task-clock.md", "AlpacaCore/src/x.cpp"]

    state, _ = decide(["AlpacaCore/src/x.cpp"], head, [], permission_of)
    expect(state == "success", "a PR with no decision record must pass")
    state, desc = decide(rec, head, [], permission_of)
    expect(state == "failure" and "ADR authorized 0123456789ab" in desc, "an unauthorized record change must fail: %r" % desc)
    for login in ("writer", "maint", "admin1"):
        state, _ = decide(rec, head, [(login, "ADR authorized 0123456")], permission_of)
        expect(state == "success", "%s may authorize" % login)
    for login in ("reader", "triager", "stranger"):
        state, _ = decide(rec, head, [(login, "ADR authorized %s" % head)], permission_of)
        expect(state == "failure", "%s must not be able to authorize" % login)
    state, _ = decide(rec, head, [("writer", "ADR authorized fedcba9")], permission_of)
    expect(state == "failure", "an authorization for another commit must not count")
    state, _ = decide(rec, head, [("writer", "ADR authorized 012345")], permission_of)
    expect(state == "failure", "a sha shorter than 7 digits must not count")
    state, _ = decide(rec, head, [("writer", "I think ADR authorized 0123456 is fine")], permission_of)
    expect(state == "failure", "the phrase must be a line of its own")
    state, _ = decide(rec, head, [("writer", "Looks good.\n  ADR authorized 0123456789ABCDEF  \nThanks")], permission_of)
    expect(state == "success", "the phrase may sit on its own line inside a longer comment, any case")
    state, _ = decide(rec, head, [("github-actions[bot]", "ADR authorized 0123456")],
                      lambda _login: "admin")
    expect(state == "failure", "a bot must not be able to authorize")
    state, _ = decide(["AlpacaCore/src/y.cpp", "docs/decisions/0009-x.md"], head, [], permission_of)
    expect(state == "failure", "a rename away from docs/decisions/ (old name listed) must still need authorization")
    state, desc = decide(rec, head, [("w" * 200, "ADR authorized 0123456")], lambda _login: "write")
    expect(state == "success" and len(desc) <= MAX_DESCRIPTION, "the description must fit a commit status")

    for f in failures:
        print("FAIL: " + f)
    if failures:
        print("%d check_adr_authorization self-test(s) failed." % len(failures))
        return 1
    print("check_adr_authorization self-test passed.")
    return 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    repo = pr = None
    args = iter(argv)
    for arg in args:
        if arg == "--repo":
            repo = next(args, None)
        elif arg == "--pr":
            pr = next(args, None)
    if not repo or not pr or not re.fullmatch(r"[\w.-]+/[\w.-]+", repo) or not re.fullmatch(r"\d+", pr):
        print("usage: check_adr_authorization.py --repo OWNER/NAME --pr N [--post-status] | --self-test",
              file=sys.stderr)
        return 2
    return run(repo, pr, "--post-status" in argv)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
