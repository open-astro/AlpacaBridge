#!/usr/bin/env python3
"""Select the GitHub Releases a new release supersedes.

After beta vX.Y.Z-beta.N publishes, the releases of vX.Y.Z-beta.1 ..
beta.(N-1) are superseded. After stable vX.Y.Z publishes, every
vX.Y.Z-beta.* release is. Nothing else is selected: not the released tag, not a
stable release, not nightly-build, not the betas of another X.Y.Z, not a name
that is not a release tag. The caller deletes releases only and never the tags
(no --cleanup-tag), so a pruned release can be rebuilt from its tag.

Prints the tags to delete, one per line, oldest beta first.

Usage: python3 scripts/release_prune.py <released-tag> [<existing-release-tag> ...]
Self-test: python3 scripts/release_prune.py --self-test
"""
import os
import re
import sys
import tempfile

TAG_RE = re.compile(r"^v(\d+\.\d+\.\d+)(?:-beta\.([1-9]\d*))?$")


def parse(tag):
    """Return (X.Y.Z, beta number or None), or None for any other name."""
    m = TAG_RE.fullmatch(tag)
    if not m:
        return None
    return m.group(1), int(m.group(2)) if m.group(2) else None


def select(released, existing):
    """Tags in `existing` that the release `released` supersedes."""
    rel = parse(released)
    if rel is None:
        raise ValueError("tag %r is not vX.Y.Z or vX.Y.Z-beta.N" % released)
    base, beta = rel
    picked = {}
    for tag in existing:
        p = parse(tag)
        if p is None or p[0] != base or p[1] is None or tag == released:
            continue
        if beta is None or p[1] < beta:
            picked[tag] = p[1]
    return sorted(picked, key=picked.get)


CASES_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "release_prune_cases.txt")


def read_cases(path=CASES_FILE):
    """(released, existing, expected) from release_prune_cases.txt.

    A '#' starts a comment, blank lines are skipped, every other line is
    '<released> | <existing tags> | <tags to delete>' with '-' for an empty
    list. Anything else raises ValueError naming the line.
    """
    cases = []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            text = line.split("#", 1)[0].strip()
            if not text:
                continue
            parts = [p.split() for p in text.split("|")]
            if len(parts) != 3 or len(parts[0]) != 1:
                raise ValueError("%s:%d: expected '<released> | <existing> | <deleted>', got %r" % (path, n, line.rstrip("\n")))
            existing, expected = ([] if p == ["-"] else p for p in parts[1:])
            cases.append((parts[0][0], existing, expected))
    return cases


def self_test():
    failures = []
    try:
        cases = read_cases()
    except ValueError as e:
        failures.append(str(e))
        cases = []
    if not cases:
        failures.append("release_prune_cases.txt holds no cases")
    for released, existing, expected in cases:
        got = select(released, existing)
        if got != expected:
            failures.append("%s over %s: got %s, want %s" % (released, existing, got, expected))
        if released in got or any(parse(t) is None or parse(t)[1] is None for t in got):
            failures.append("%s selected the released tag or a non-beta" % released)
    with tempfile.TemporaryDirectory() as tmp:
        sample = os.path.join(tmp, "cases.txt")
        with open(sample, "w", encoding="utf-8") as f:
            f.write("# c\n\nv1.0.0 | v1.0.0-beta.1 | v1.0.0-beta.1  # trailing\r\n")
        if read_cases(sample) != [("v1.0.0", ["v1.0.0-beta.1"], ["v1.0.0-beta.1"])]:
            failures.append("trailing comments and CRLF are not read")
        with open(sample, "w", encoding="utf-8") as f:
            f.write("v1.0.0 | v1.0.0-beta.1\n")
        try:
            read_cases(sample)
            failures.append("a two-field line was accepted")
        except ValueError as e:
            if ":1:" not in str(e):
                failures.append("a bad line does not name its line")
    for bad in ("", "5.0.0", "v5.0", "v5.0.0-beta.0", "nightly-build", "v5.0.0-rc.1", "v5.0.0\n"):
        try:
            select(bad, [])
            failures.append("accepted malformed released tag %r" % bad)
        except ValueError:
            pass
    for f in failures:
        print("SELF-TEST FAIL: %s" % f, file=sys.stderr)
    print("release_prune self-test %s." % ("FAILED" if failures else "OK"))
    return 1 if failures else 0


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    if not argv:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        tags = select(argv[0], argv[1:])
    except ValueError as e:
        print("ERROR: %s" % e, file=sys.stderr)
        return 1
    for t in tags:
        print(t)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
