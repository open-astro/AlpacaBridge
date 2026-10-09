#!/usr/bin/env python3
"""Decide whether a PR is a merge down (docs/beta-channel.md), for the
falsified-by gate in .github/workflows/pr-body.yml and ci_preflight.sh gate 2b2.

A merge down carries stable/X.Y into main, or a hotfix into a newer
stable/X.Z. Its test cases were already listed on the fix PRs that landed them
on stable/X.Y, and against the receiving branch's merge-base every one of them
reads as new again, so the gate skips it. Only the real shape is exempt:

  - the head is named merge-down/X.Y-to-main or merge-down/X.Y-to-X.Z
    (one head per receiving branch, so merging one PR never deletes the head
    of the other);
  - the base is the branch the name targets (main, or stable/X.Z);
  - the head is a branch of this repository (CI passes --same-repo);
  - every non-merge commit the PR adds is already on stable/X.Y;
  - no merge commit the PR adds changes a test file beyond what git's own
    merge of its parents produces. The head merges the receiving branch in
    to resolve the version files (the receiving branch keeps its own VERSION
    and badge), and a conflict resolution may edit anything else too, but a
    test case written into a merge would otherwise skip this gate unseen.
    Each merge is re-merged with `git merge-tree --write-tree` (git 2.38+) and
    compared with the commit; a merge down whose test-file conflicts had to
    be resolved by hand goes through the gate like any other PR.

Usage:
  merge_down.py --exempt --head-ref H --base-ref B --base-rev REV
                [--head-rev REV] [--stable-remote R] [--same-repo true|false]
      exit 0 and say why when exempt, 1 (and say why not) otherwise
  merge_down.py --self-test
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

HEAD_RE = re.compile(r"^merge-down/(\d+\.\d+)-to-(main|\d+\.\d+)$")
# Where the gate looks for new test cases (scripts/check_falsified_by.py).
TEST_DIRS = ("AlpacaCore/tests/", "AlpacaHTTP/tests/")


def parse_head(head_ref):
    """('X.Y', 'main' or 'stable/X.Z') for a merge-down head name, else None."""
    m = HEAD_RE.match(head_ref or "")
    if not m:
        return None
    source, target = m.groups()
    if target != "main":
        if tuple(map(int, target.split("."))) <= tuple(map(int, source.split("."))):
            return None  # a hotfix only travels to a newer stable branch
        target = "stable/" + target
    return source, target


def git(*args, cwd=None):
    return subprocess.run(["git", *args], capture_output=True, text=True, cwd=cwd)


def strip_remote(ref, cwd=None):
    """origin/stable/5.1 -> stable/5.1 when 'origin' is a remote; a bare name is unchanged."""
    first, _, rest = ref.partition("/")
    if rest and first in git("remote", cwd=cwd).stdout.split():
        return rest
    return ref


def exempt(head_ref, base_ref, base_rev, head_rev="HEAD", stable_remote="origin", same_repo=True, cwd=None):
    """(True, reason) or (False, reason)."""
    if not same_repo:
        return False, "the head is not a branch of this repository"
    parsed = parse_head(head_ref)
    if not parsed:
        return False, "head %r is not merge-down/X.Y-to-main or merge-down/X.Y-to-X.Z" % head_ref
    source, target = parsed
    base = strip_remote(base_ref, cwd)
    if base != target:
        return False, "head %s targets %s but the base is %s" % (head_ref, target, base)
    stable = "%s/stable/%s" % (stable_remote, source)
    if git("rev-parse", "--verify", "--quiet", stable, cwd=cwd).returncode != 0:
        return False, "%s is not available to compare against (fetch it)" % stable
    r = git("rev-list", "--no-merges", "%s..%s" % (base_rev, head_rev), "--not", stable, cwd=cwd)
    if r.returncode != 0:
        return False, "git rev-list failed: %s" % r.stderr.strip()
    extra = r.stdout.split()
    if extra:
        return False, "%d commit(s) are not on %s, e.g. %s" % (len(extra), stable, extra[0][:12])
    merges = git("rev-list", "--merges", "%s..%s" % (base_rev, head_rev), cwd=cwd)
    if merges.returncode != 0:
        return False, "git rev-list failed: %s" % merges.stderr.strip()
    for merge in merges.stdout.split():
        edited = merge_edits(merge, cwd)
        if edited is None:
            return False, "could not re-merge %s to inspect it" % merge[:12]
        tests = [p for p in edited if p.startswith(TEST_DIRS)]
        if tests:
            return False, "merge %s changes test files beyond the automatic merge: %s" % (merge[:12], ", ".join(tests))
    return True, "merge down of stable/%s into %s: every commit it adds is already on %s" % (source, target, stable)


def merge_edits(merge, cwd=None):
    """Paths where a two-parent merge commit differs from git's own merge of its parents.

    Empty for a merge git made unaided; a resolved conflict or an edit made in
    the merge shows up here. None when the merge cannot be re-run (an octopus
    merge, or a git without merge-tree --write-tree).
    """
    parents = git("rev-list", "--parents", "-n", "1", merge, cwd=cwd).stdout.split()[1:]
    if len(parents) != 2:
        return None
    # Exit 1 means conflicts; the first line is still the tree, markers included.
    r = git("merge-tree", "--write-tree", "--no-messages", parents[0], parents[1], cwd=cwd)
    if r.returncode not in (0, 1) or not r.stdout:
        return None
    tree = r.stdout.split()[0]
    d = git("diff", "--name-only", tree, merge, cwd=cwd)
    if d.returncode != 0:
        return None
    return d.stdout.split()


def self_test():
    failures = []

    def check(name, ok):
        if not ok:
            failures.append(name)

    check("to main", parse_head("merge-down/5.0-to-main") == ("5.0", "main"))
    check("to newer stable", parse_head("merge-down/5.0-to-5.1") == ("5.0", "stable/5.1"))
    for bad in ("merge-down/5.0", "merge-down/5.0-to-5.0", "merge-down/5.1-to-5.0", "merge-down/foo",
                "merge-down/5.0-to-main/x", "feature/merge-down/5.0-to-main", ""):
        check("rejects %r" % bad, parse_head(bad) is None)

    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t",
                   GIT_COMMITTER_EMAIL="t@t", GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull)

        def g(*args):
            r = subprocess.run(["git", *args], cwd=tmp, capture_output=True, text=True, env=env)
            if r.returncode != 0:
                raise RuntimeError("git %s: %s" % (" ".join(args), r.stderr))
            return r.stdout.strip()

        def commit(name):
            with open(os.path.join(tmp, name), "w", encoding="utf-8") as f:
                f.write(name)
            g("add", name)
            g("commit", "-qm", name)

        g("init", "-q", "-b", "main")
        commit("a")
        g("branch", "stable/5.0")
        commit("main-only")
        g("checkout", "-q", "stable/5.0")
        commit("fix")
        # A fake remote layout: refs/remotes/origin/<branch>.
        g("update-ref", "refs/remotes/origin/stable/5.0", "stable/5.0")
        g("update-ref", "refs/remotes/origin/main", "main")
        g("remote", "add", "origin", tmp)
        g("checkout", "-q", "-b", "merge-down/5.0-to-main")
        g("merge", "-q", "--no-edit", "main")
        base = g("rev-parse", "main")

        def ex(head="merge-down/5.0-to-main", base_ref="main", **kw):
            return exempt(head, base_ref, base, cwd=tmp, **kw)[0]

        check("a clean merge down is exempt", ex())
        check("a remote-qualified base is read", ex(base_ref="origin/main"))
        check("a fork head is not exempt", not ex(same_repo=False))
        check("a wrong base is not exempt", not ex(base_ref="stable/5.1"))
        check("a bad head name is not exempt", not ex(head="merge-down/foo"))
        # A version file resolved in a follow-up merge is fine; a test case
        # written into a merge is not.
        g("checkout", "-q", "main")
        commit("main-later")
        base = g("rev-parse", "main")
        g("checkout", "-q", "merge-down/5.0-to-main")
        g("merge", "-q", "--no-commit", "main")
        with open(os.path.join(tmp, "VERSION"), "w", encoding="utf-8") as f:
            f.write("4.2.0\n")
        g("add", "VERSION")
        g("commit", "-qm", "merge main, keep its VERSION")
        check("a merge that edits only the version files is exempt", ex())
        g("checkout", "-q", "-b", "evil", "merge-down/5.0-to-main~1")
        g("merge", "-q", "--no-commit", "main")
        os.makedirs(os.path.join(tmp, "AlpacaCore", "tests"), exist_ok=True)
        with open(os.path.join(tmp, "AlpacaCore", "tests", "test_x.cpp"), "w", encoding="utf-8") as f:
            f.write('TEST_CASE("smuggled") {}\n')
        g("add", "AlpacaCore/tests/test_x.cpp")
        g("commit", "-qm", "merge main")
        check("a test case written into a merge is not exempt", not ex(head_rev="evil"))
        check("merge_edits names the test file",
              merge_edits(g("rev-parse", "evil"), cwd=tmp) == ["AlpacaCore/tests/test_x.cpp"])
        g("checkout", "-q", "merge-down/5.0-to-main")
        commit("smuggled")
        check("a commit not on stable/5.0 is not exempt", not ex())
    for f in failures:
        print("SELF-TEST FAIL: %s" % f, file=sys.stderr)
    print("merge_down self-test %s." % ("FAILED" if failures else "OK"))
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--exempt", action="store_true")
    ap.add_argument("--head-ref")
    ap.add_argument("--base-ref")
    ap.add_argument("--base-rev", help="the base commit to diff against (CI: the PR's base SHA)")
    ap.add_argument("--head-rev", default="HEAD")
    ap.add_argument("--stable-remote", default="origin")
    ap.add_argument("--same-repo", default="true", choices=("true", "false"))
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not (args.exempt and args.head_ref is not None and args.base_ref and args.base_rev):
        ap.error("--exempt needs --head-ref, --base-ref and --base-rev")
    ok, reason = exempt(args.head_ref, args.base_ref, args.base_rev, args.head_rev, args.stable_remote,
                        args.same_repo == "true")
    print(("exempt: " if ok else "not a merge down: ") + reason)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
