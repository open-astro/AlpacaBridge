#!/usr/bin/env python3
"""The <next> a nightly from main is stamped with: <next>~nightly<stamp>.g<sha7>.

changelog_fragments.py --bump alone is not enough. Once /bump-release cuts
stable/X.Y, every X.Y.Z (its betas, the release and its patches) is built from
that branch, and main's code will first ship in X.(Y+1).0 or later. A merge-down
keeps main's VERSION, so --bump on main can still print X.Y.Z: the nightly was
then named after a release it is not part of, and dpkg sorted it below X.Y.Z,
so apt "upgraded" a tester from main back to the stable line.

The base is therefore the higher of the --bump result and X.(Y+1).0, where X.Y
is the newest line named by a stable/X.Y branch or a vX.Y.Z[-beta.N] tag. Ref
names (refs/heads/stable/5.0, refs/tags/v5.0.0-beta.4, ...) are read from stdin,
one per line; anything else is ignored.

Usage: <refs> | python3 scripts/nightly_version.py --bump <X.Y.Z>
Self-test: python3 scripts/nightly_version.py --self-test
"""
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from changelog_fragments import BARE_RE, version_tuple  # noqa: E402

LINE_REF_RE = re.compile(r"^refs/(?:heads/stable/(\d+)\.(\d+)|tags/v(\d+)\.(\d+)\.\d+(?:-beta\.[1-9]\d*)?)$")


def newest_line(refs):
    """(X, Y) of the newest stable branch or release tag, or None."""
    lines = []
    for ref in refs:
        m = LINE_REF_RE.match(ref.strip())
        if m:
            x, y = (m.group(1), m.group(2)) if m.group(1) is not None else (m.group(3), m.group(4))
            lines.append((int(x), int(y)))
    return max(lines) if lines else None


def nightly_base(bump, refs):
    """The higher of bump and the minor release after the newest line."""
    if not BARE_RE.match(bump):
        raise ValueError("--bump %r is not X.Y.Z" % bump)
    proposed = version_tuple(bump)
    line = newest_line(refs)
    if line:
        proposed = max(proposed, (line[0], line[1] + 1, 0))
    return ".".join(str(x) for x in proposed)


def self_test():
    failures = []

    def check(name, ok):
        if not ok:
            failures.append(name)

    cut = ["refs/heads/main", "refs/heads/stable/5.0", "refs/tags/v4.2.0", "refs/tags/v5.0.0-beta.4"]
    check("main after the 5.0 cut is past 5.0.0", nightly_base("5.0.0", cut) == "5.1.0")
    check("a breaking change on main still wins", nightly_base("6.0.0", cut) == "6.0.0")
    check("a beta tag alone marks the line", nightly_base("5.0.0", ["refs/tags/v5.0.0-beta.1"]) == "5.1.0")
    check("a stable branch alone marks the line", nightly_base("5.0.0", ["refs/heads/stable/5.0"]) == "5.1.0")
    check("after the merge-down a fix-only main is still past 5.0",
          nightly_base("5.0.1", cut + ["refs/tags/v5.0.0"]) == "5.1.0")
    check("no line keeps the bump", nightly_base("5.0.0", ["refs/heads/main"]) == "5.0.0")
    check("lines compare numerically", newest_line(["refs/heads/stable/5.9", "refs/heads/stable/5.10"]) == (5, 10))
    for ref in ("refs/heads/stable/5", "refs/heads/stable/5.0-x", "refs/tags/v5.0", "refs/tags/v5.0.0-rc.1",
                "refs/tags/v5.0.0-beta.0", "refs/tags/nightly-build", "refs/heads/feature/stable/9.9"):
        check("ignores %s" % ref, newest_line([ref]) is None)
    for bad in ("5.0", "5.0.0~beta1", ""):
        try:
            nightly_base(bad, cut)
            failures.append("accepted --bump %r" % bad)
        except ValueError:
            pass
    # The ordering the nightly promises a tester on main.
    order = ["5.0.0~beta4", "5.0.0", "5.0.1", "5.1.0~beta1", "5.1.0~nightly202610170317.gabc1234", "5.1.0"]
    try:
        for lo, hi in zip(order, order[1:]):
            if subprocess.run(["dpkg", "--compare-versions", lo, "lt", hi]).returncode != 0:
                failures.append("dpkg order: %s is not below %s" % (lo, hi))
    except FileNotFoundError:
        print("dpkg not installed: version order not checked", file=sys.stderr)
    for f in failures:
        print("SELF-TEST FAIL: %s" % f, file=sys.stderr)
    print("nightly_version self-test %s." % ("FAILED" if failures else "OK"))
    return 1 if failures else 0


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    if len(argv) != 2 or argv[0] != "--bump":
        print(__doc__, file=sys.stderr)
        return 2
    try:
        print(nightly_base(argv[1], sys.stdin.read().splitlines()))
    except ValueError as e:
        print("ERROR: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
