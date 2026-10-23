#!/usr/bin/env python3
"""Keep one comment on an open issue while it lacks a type, priority or size label.

The repository labels every issue with a type, a priority and a size, but
nothing enforced it (issue #1361). The workflow .github/workflows/issue-labels.yml
runs this script when an issue is opened, reopened, labeled or unlabeled.

- An open issue missing one or more of the three label groups gets ONE comment
  naming the missing groups; a later run edits that comment in place.
- When all three groups are present the comment is deleted.
- Nothing is labelled or closed, a closed issue is left alone, and an issue
  opened by a bot or labelled `dependencies` or `github_actions` is exempt.

The comment is found by a marker string and only if the workflow's own bot
wrote it, so a copied marker in someone else's comment is never edited. The
labels and state are read from GitHub at run time, not from the event, so
events that overlap cannot act on stale labels. The issue number and author
come from the environment.

Usage:
  check_issue_labels.py [--dry-run]   (ISSUE_NUMBER, AUTHOR_LOGIN, AUTHOR_TYPE; GH_TOKEN, GH_REPO)
  check_issue_labels.py --self-test
"""
import argparse
import contextlib
import io
import json
import os
import subprocess
import sys

MARKER = "<!-- issue-labels -->"
BOT_LOGIN = "github-actions[bot]"
EXEMPT_LABELS = ("dependencies", "github_actions")
GROUPS = (
    ("type", ("bug", "enhancement", "documentation", "question")),
    ("priority", ("P1", "P2", "P3", "P4", "P5")),
    ("size", ("XS", "S", "M", "L", "XL")),
)


def missing_groups(labels):
    """The names of the label groups with no label in `labels`, in group order."""
    have = set(labels)
    return [name for name, options in GROUPS if not have.intersection(options)]


def exempt(labels, author_login, author_type):
    """True for an issue opened by a bot or carrying a bot-issue label."""
    return author_type == "Bot" or author_login.endswith("[bot]") or bool(set(labels).intersection(EXEMPT_LABELS))


def comment_body(missing):
    lines = ["%s\nThis issue has no label for: **%s**.\n" % (MARKER, ", ".join(missing))]
    for name, options in GROUPS:
        if name in missing:
            lines.append("- %s: one of %s" % (name, ", ".join("`%s`" % o for o in options)))
    lines.append("\nA maintainer sets the labels; this comment updates itself and goes away when all three groups are set.")
    return "\n".join(lines) + "\n"


def find_comment(comments):
    """The first marker comment written by the workflow's bot, or None."""
    for c in comments:
        if c.get("user", {}).get("login") == BOT_LOGIN and MARKER in (c.get("body") or ""):
            return c
    return None


def reconcile(number, issue, comments, author_login, author_type, client):
    """Apply the rule to one issue through `client`; return a one-line summary."""
    if issue["state"].lower() != "open":
        return "#%d is closed: nothing to do" % number
    labels = issue["labels"]
    existing = find_comment(comments)
    missing = [] if exempt(labels, author_login, author_type) else missing_groups(labels)
    if not missing:
        if existing:
            client.delete_comment(existing["id"])
            return "deleted the label comment on #%d" % number
        return "#%d needs nothing" % number
    body = comment_body(missing)
    if not existing:
        client.create_comment(number, body)
        return "commented on #%d: missing %s" % (number, ", ".join(missing))
    if existing["body"] == body:
        return "#%d already names %s" % (number, ", ".join(missing))
    client.update_comment(existing["id"], body)
    return "updated the label comment on #%d: missing %s" % (number, ", ".join(missing))


class GhIssues:
    """Issue operations through the gh CLI (GH_TOKEN and GH_REPO from the environment)."""

    def __init__(self, dry_run):
        self.dry_run = dry_run

    def _gh(self, *args, stdin=None, write=False):
        if self.dry_run and write:
            print("DRY RUN: gh " + " ".join(args) + ("\n" + stdin if stdin else ""))
            return ""
        return subprocess.run(["gh", *args], input=stdin, check=True, capture_output=True, text=True).stdout

    def issue(self, number):
        out = self._gh("issue", "view", str(number), "--json", "state,labels")
        d = json.loads(out)
        return {"state": d["state"], "labels": [label["name"] for label in d["labels"]]}

    def comments(self, number):
        out = self._gh("api", "--paginate", "--slurp", "repos/{owner}/{repo}/issues/%d/comments" % number)
        return [c for page in json.loads(out or "[]") for c in page]

    def create_comment(self, number, text):
        self._gh("issue", "comment", str(number), "--body-file", "-", stdin=text, write=True)

    def update_comment(self, comment_id, text):
        self._gh("api", "-X", "PATCH", "repos/{owner}/{repo}/issues/comments/%d" % comment_id, "-F", "body=@-",
                 stdin=text, write=True)

    def delete_comment(self, comment_id):
        self._gh("api", "-X", "DELETE", "repos/{owner}/{repo}/issues/comments/%d" % comment_id, write=True)


def self_test():
    bad = []

    def check(name, got, want):
        if got != want:
            bad.append("FAIL %s: got %r, want %r" % (name, got, want))

    full = ["bug", "P3", "S"]
    for labels, want in [
        ([], ["type", "priority", "size"]),
        (["P2", "M"], ["type"]),
        (["bug", "M"], ["priority"]),
        (["question", "P5"], ["size"]),
        (["documentation"], ["priority", "size"]),
        (["enhancement", "P1", "XL"], []),
        (full, []),
        (["P1", "P2", "bug", "XS"], []),
        (["p3", "s", "Bug"], ["type", "priority", "size"]),
    ]:
        check("missing_groups(%r)" % labels, missing_groups(labels), want)

    for labels, login, kind, want in [
        ([], "alice", "User", False), (["dependencies"], "alice", "User", True),
        (["github_actions"], "alice", "User", True), ([], "dependabot[bot]", "Bot", True),
        ([], "renovate[bot]", "User", True), ([], "app", "Bot", True),
    ]:
        check("exempt(%r, %r, %r)" % (labels, login, kind), exempt(labels, login, kind), want)

    check("body names only the missing groups", [g for g, _ in GROUPS if "- %s:" % g in comment_body(["size"])],
          ["size"])
    check("body starts with the marker", comment_body(["type"]).startswith(MARKER), True)

    own = {"id": 11, "user": {"login": BOT_LOGIN}, "body": comment_body(["type", "priority"])}
    copied = {"id": 12, "user": {"login": "mallory"}, "body": MARKER + "\nfake"}
    check("find own marker comment", (find_comment([copied, own]) or {}).get("id"), 11)
    check("a copied marker is ignored", find_comment([copied]), None)
    check("no comments", find_comment([]), None)

    class Fake:
        def __init__(self):
            self.calls = []

        def create_comment(self, number, text):
            self.calls.append(("create", number, text))

        def update_comment(self, comment_id, text):
            self.calls.append(("update", comment_id, text))

        def delete_comment(self, comment_id):
            self.calls.append(("delete", comment_id))

    def run(labels, comments=(), state="OPEN", login="alice", kind="User"):
        f = Fake()
        reconcile(5, {"state": state, "labels": labels}, list(comments), login, kind, f)
        return f.calls

    for name, labels, missing in [("type missing", ["P3", "S"], ["type"]), ("priority missing", ["bug", "S"], ["priority"]),
                                  ("size missing", ["bug", "P3"], ["size"]),
                                  ("all missing", [], ["type", "priority", "size"])]:
        check("create: " + name, run(labels), [("create", 5, comment_body(missing))])
    check("all present, no comment", run(full), [])
    check("all present deletes the comment", run(full, [own]), [("delete", 11)])
    check("update in place", run(["bug", "S"], [own]), [("update", 11, comment_body(["priority"]))])
    check("unchanged comment is not rewritten", run(["bug", "S"], [dict(own, body=comment_body(["priority"]))]),
          [])
    check("copied marker: new comment, not an edit", run(["bug", "S"], [copied]),
          [("create", 5, comment_body(["priority"]))])
    check("closed issue is left alone", run([], [own], state="CLOSED"), [])
    check("bot author is exempt", run([], login="dependabot[bot]", kind="Bot"), [])
    check("exempt label removes a stale comment", run(["dependencies"], [own]), [("delete", 11)])

    ran, stdins = [], []
    real_run = subprocess.run

    class Done:
        stdout = json.dumps([[{"id": 1}], [{"id": 2}]])

    def record(cmd, **kwargs):
        ran.append(cmd)
        stdins.append(kwargs.get("input"))
        return Done()

    subprocess.run = record
    try:
        dry = GhIssues(dry_run=True)
        with contextlib.redirect_stdout(io.StringIO()):
            dry.create_comment(5, "text")
            dry.update_comment(3, "text")
            dry.delete_comment(3)
        check("dry run makes no write call", ran, [])
        check("dry run still reads", [c["id"] for c in dry.comments(5)], [1, 2])
        check("a read calls gh", [c[:2] for c in ran], [["gh", "api"]])
        live = GhIssues(dry_run=False)
        for name, write, want in [
            ("create", lambda: live.create_comment(5, "text"),
             (["gh", "issue", "comment", "5", "--body-file", "-"], "text")),
            ("update", lambda: live.update_comment(3, "text"),
             (["gh", "api", "-X", "PATCH", "repos/{owner}/{repo}/issues/comments/3", "-F", "body=@-"], "text")),
            ("delete", lambda: live.delete_comment(3),
             (["gh", "api", "-X", "DELETE", "repos/{owner}/{repo}/issues/comments/3"], None)),
        ]:
            del ran[:], stdins[:]
            write()
            check("live %s runs gh" % name, list(zip(ran, stdins)), [want])
    finally:
        subprocess.run = real_run

    for line in bad:
        print(line)
    if bad:
        return 1
    print("check_issue_labels self-test: all cases passed")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--self-test", action="store_true")
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args()
    if args.self_test:
        return self_test()
    number = os.environ.get("ISSUE_NUMBER", "")
    if not number.isdigit():
        p.error("ISSUE_NUMBER must be an issue number")
    number = int(number)
    client = GhIssues(args.dry_run)
    print(reconcile(number, client.issue(number), client.comments(number), os.environ.get("AUTHOR_LOGIN", ""),
                    os.environ.get("AUTHOR_TYPE", ""), client))
    return 0


if __name__ == "__main__":
    sys.exit(main())
