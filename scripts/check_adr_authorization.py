#!/usr/bin/env python3
"""Require a collaborator's explicit authorization for a change to a decision record.

The decision records in docs/decisions/ are the rules the decision-record gate
holds every pull request to, so changing one changes the rules. A PR that adds,
edits, renames or deletes a file under docs/decisions/ passes this check only
when a repository collaborator with write, maintain or admin permission has
posted, as a PR comment or a review, a line that reads exactly

    ADR authorized <head sha>

where <head sha> names a commit (7 to 40 hex digits of it), normally the PR
head at the time; any commit GitHub resolves counts, since only its record
content is compared. Naming the commit binds the authorization to the content of
the records: it holds for a later head as long as every decision-record path the
PR changes has the same content (git blob) at that head as at the named commit,
so a push that only changes code or tests needs no new authorization, and a push
that changes the content of one of those records (an edit, a new record, or the
deletion of a record the PR still changes) does. The comparison is by
content, not by a diff between the two commits, so a merge of the base branch
or a rebase that leaves the records alone keeps the authorization. A named
commit GitHub cannot resolve does not count. The author may authorize their own
PR; the authorization is still an explicit act on the record. A PR that touches
no decision record passes at once.

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


def decide(changed, head_sha, statements, permission_of, records_at=lambda _sha: None):
    """Return (state, description) for one PR.

    changed: every path the PR adds, edits, deletes or renames (old and new
    names). statements: [(login, body)] for every PR comment and review.
    permission_of: login -> the login's repository permission, or None.
    records_at: sha -> {path: blob sha} for the decision records at that
    commit, or None when the commit cannot be resolved.
    """
    records = sorted({p for p in changed if p.startswith(DECISION_PREFIX)})
    if not records:
        return "success", "No decision record changed"
    head = head_sha.lower()
    at_head = None
    for login, body in statements:
        if not login or login.endswith("[bot]"):
            continue
        for sha in AUTHORIZE_RE.findall(body or ""):
            if permission_of(login) not in AUTHORIZING_PERMISSIONS:
                continue
            if head.startswith(sha.lower()):
                return "success", ("Decision record change authorized by @%s for %s" % (login, head[:12]))[:MAX_DESCRIPTION]
            if at_head is None:
                at_head = records_at(head)
            at_sha = records_at(sha.lower())
            if at_head is not None and at_sha is not None and all(at_sha.get(r) == at_head.get(r) for r in records):
                return "success", ("Authorized by @%s at %s; the changed records are the same at %s"
                                   % (login, sha.lower()[:12], head[:12]))[:MAX_DESCRIPTION]
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


def records_lookup(repo, run=subprocess.run):
    """sha -> {path: object sha} for every entry under docs/decisions/ at that commit, or None.

    The whole subtree, not only its direct files: a record added in a
    subdirectory, a symlink or a submodule entry must differ from its absence.
    None (the authorization does not count) when the commit cannot be resolved,
    a call fails, or the tree listing is truncated.
    """
    cache = {}
    parent, name = DECISION_PREFIX.rstrip("/").rsplit("/", 1)

    def api(path):
        r = run(["gh", "api", "repos/%s/%s" % (repo, path)], capture_output=True, text=True)
        if r.returncode != 0:
            return "404" if "(HTTP 404)" in r.stderr else None
        try:
            return json.loads(r.stdout)
        except ValueError:
            return None

    def snapshot(sha):
        commit = api("commits/%s" % sha)
        full = commit.get("sha", "") if isinstance(commit, dict) else ""
        if not re.fullmatch(r"[0-9a-f]{40}", full):
            return None
        listing = api("contents/%s?ref=%s" % (parent, full))
        if listing == "404":
            return {}  # no docs/ directory at that commit, so no record at all
        if not isinstance(listing, list):
            return None
        directory = next((e for e in listing if isinstance(e, dict) and e.get("name") == name), None)
        if directory is None:
            return {}
        if directory.get("type") != "dir":
            return None
        tree = api("git/trees/%s?recursive=1" % directory.get("sha", ""))
        if not isinstance(tree, dict) or tree.get("truncated") or not isinstance(tree.get("tree"), list):
            return None
        return {DECISION_PREFIX + e["path"]: e.get("sha") for e in tree["tree"]
                if isinstance(e, dict) and e.get("type") != "tree" and e.get("path")}

    def records_at(sha):
        if sha not in cache:
            cache[sha] = snapshot(sha)
        return cache[sha]

    return records_at


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
    state, description = decide(changed, head_sha, statements, permission_lookup(repo), records_lookup(repo))
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

    # An authorization for an earlier commit holds while the changed records keep their content.
    old = "fedcba9876543210fedcba9876543210fedcba98"
    snapshots = {head: {"docs/decisions/0005-task-clock.md": "b1", "docs/decisions/README.md": "i1"},
                 old: {"docs/decisions/0005-task-clock.md": "b1", "docs/decisions/README.md": "i0"}}

    def records_at(sha):
        return next((v for k, v in snapshots.items() if k.startswith(sha)), None)

    state, desc = decide(rec, head, [("writer", "ADR authorized fedcba9")], permission_of, records_at)
    expect(state == "success" and "same at" in desc, "an older authorization holds when the changed records are the same: %r" % desc)
    state, _ = decide(rec + ["docs/decisions/README.md"], head, [("writer", "ADR authorized fedcba9")], permission_of, records_at)
    expect(state == "failure", "a record the PR changed after the authorization needs a new one")
    state, _ = decide(rec + ["docs/decisions/0020-new.md"], head, [("writer", "ADR authorized fedcba9")], permission_of,
                      lambda sha: dict(records_at(sha), **({"docs/decisions/0020-new.md": "n1"} if head.startswith(sha) else {})))
    expect(state == "failure", "a record added after the authorization needs a new one")
    state, _ = decide(rec, head, [("writer", "ADR authorized abcdef1")], permission_of, records_at)
    expect(state == "failure", "an authorization for a commit that cannot be resolved does not count")
    state, _ = decide(rec, head, [("reader", "ADR authorized fedcba9")], permission_of, records_at)
    expect(state == "failure", "an older authorization by a reader still does not count")
    state, _ = decide(rec, head, [("writer", "ADR authorized fedcba9")], permission_of, lambda sha: None)
    expect(state == "failure", "when the head's records cannot be read, only the head sha authorizes")

    # records_lookup over a fake GitHub API: a record added in a subdirectory after the
    # authorization must not read as unchanged (#1517 review).
    old_full, head_full = "a" * 40, "b" * 40
    trees = {old_full: {"0005-task-clock.md": "b1"},
             head_full: {"0005-task-clock.md": "b1", "sub/0020-new.md": "n1"}}

    def fake_gh(args, **_kw):
        class R:
            returncode, stdout, stderr = 0, "", ""
        r, url = R(), args[2]
        ref = url.split("ref=")[-1] if "ref=" in url else None
        if "/commits/" in url:
            full = next((f for f in trees if f.startswith(url.rsplit("/", 1)[-1])), None)
            body = {"sha": full} if full else None
        elif "/contents/docs?" in url:
            body = [{"name": "decisions", "path": "docs/decisions", "sha": "tree-" + ref, "type": "dir"}]
        elif "/git/trees/tree-" in url:
            sha = url.split("/git/trees/tree-")[1].split("?")[0]
            body = {"truncated": sha == truncated_at, "tree": [{"path": n, "sha": v, "type": "blob"} for n, v in trees[sha].items()]
                    + [{"path": d, "sha": "d", "type": "tree"} for d in {n.split("/")[0] for n in trees[sha] if "/" in n}]}
        else:
            body = None
        if body is None:
            r.returncode, r.stderr = 1, "gh: Not Found (HTTP 404)"
            return r
        r.stdout = json.dumps(body)
        return r

    truncated_at = None
    lookup = records_lookup("o/r", run=fake_gh)
    nested = rec + ["docs/decisions/sub/0020-new.md"]
    state, _ = decide(nested, head_full, [("writer", "ADR authorized aaaaaaa")], permission_of, lookup)
    expect(state == "failure", "a record added in a subdirectory after the authorization needs a new one")
    state, _ = decide(rec, head_full, [("writer", "ADR authorized aaaaaaa")], permission_of, lookup)
    expect(state == "success", "the API lookup keeps an authorization whose records are unchanged")
    expect(records_lookup("o/r", run=fake_gh)("c" * 7) is None, "a commit the API cannot find has no snapshot")
    truncated_at = old_full
    expect(records_lookup("o/r", run=fake_gh)(old_full) is None, "a truncated tree listing has no snapshot")
    truncated_at = None

    def flaky_gh(args, **kw):
        r = fake_gh(args, **kw)
        if "/contents/docs?" in args[2]:
            r.returncode, r.stdout, r.stderr = 1, "", "gh: error connecting to api.github.com: 404 bytes read"
        return r
    expect(records_lookup("o/r", run=flaky_gh)(old_full) is None,
           "an API error that only mentions 404 is not a missing docs/ directory")

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
