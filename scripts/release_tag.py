#!/usr/bin/env python3
"""Map a release tag to its VERSION file spelling and release kind.

A stable tag vX.Y.Z is VERSION X.Y.Z. A beta tag vX.Y.Z-beta.N is VERSION
X.Y.Z~betaN (the Debian pre-release spelling, so 5.0.0~beta1 < 5.0.0~beta2 <
5.0.0). Prints key=value lines for .github/workflows/release.yml:

  version=<VERSION file text>
  prerelease=true|false
  notes_name=<docs/releases/ file stem; the tag without its leading v>

Usage: python3 scripts/release_tag.py <tag>
Self-test: python3 scripts/release_tag.py --self-test
"""
import os
import re
import subprocess
import sys

TAG_RE = re.compile(r"^v(\d+\.\d+\.\d+)(?:-beta\.([1-9]\d*))?$")


def map_tag(tag):
    """Return (version, prerelease, notes_name) or raise ValueError."""
    m = TAG_RE.fullmatch(tag)
    if not m:
        raise ValueError("tag %r is not vX.Y.Z or vX.Y.Z-beta.N" % tag)
    base, beta = m.groups()
    if beta:
        return "%s~beta%s" % (base, beta), True, tag[1:]
    return base, False, tag[1:]


CASES_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "release_tag_cases.txt")


def read_cases(path=CASES_FILE):
    """(tag, VERSION) pairs from release_tag_cases.txt.

    The same rule as the C++ reader in AlpacaHTTP/tests/test_software_update.cpp:
    a '#' starts a comment (whole line or trailing), blank lines are skipped, and
    every other line holds exactly two fields. Anything else raises ValueError
    naming the line, never a bare unpack traceback.
    """
    pairs = []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            fields = line.split("#", 1)[0].split()
            if not fields:
                continue
            if len(fields) != 2:
                raise ValueError("%s:%d: expected '<tag> <VERSION>', got %r" % (path, n, line.rstrip("\n")))
            pairs.append((fields[0], fields[1]))
    return pairs


def self_test():
    failures = []

    def check(name, ok):
        if not ok:
            failures.append(name)

    check("stable", map_tag("v4.2.0") == ("4.2.0", False, "4.2.0"))
    check("beta", map_tag("v5.0.0-beta.2") == ("5.0.0~beta2", True, "5.0.0-beta.2"))
    check("beta 10", map_tag("v5.0.0-beta.10")[0] == "5.0.0~beta10")
    # The shared pairs also drive AlpacaHTTP/tests/test_software_update.cpp
    # (the update card maps a VERSION back to its tag spelling).
    try:
        pairs = read_cases()
    except ValueError as e:
        failures.append(str(e))
        pairs = []
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        sample = os.path.join(tmp, "cases.txt")
        with open(sample, "w", encoding="utf-8") as f:
            f.write("# c\n\nv5.0.0 5.0.0  # trailing comment\n  v5.0.0-beta.1 5.0.0~beta1\r\n")
        check("trailing comments and CRLF are read", read_cases(sample) == [("v5.0.0", "5.0.0"), ("v5.0.0-beta.1", "5.0.0~beta1")])
        with open(sample, "w", encoding="utf-8") as f:
            f.write("v5.0.0 5.0.0 extra\n")
        try:
            read_cases(sample)
            failures.append("a three-field line was accepted")
        except ValueError as e:
            check("a three-field line names its line", ":1:" in str(e))
    check("release_tag_cases.txt has a beta and a stable pair",
          any("~" in v for _, v in pairs) and any("~" not in v for _, v in pairs))
    # The tag side is spelled here, the VERSION side in changelog_fragments.py:
    # every mapped version must be what that module calls a VERSION and a beta.
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import changelog_fragments
    for tag, version in pairs:
        mapped, pre, notes = map_tag(tag)
        check("case %s is a VERSION to changelog_fragments" % tag, changelog_fragments.VERSION_RE.match(mapped) is not None)
        check("case %s beta agrees with changelog_fragments.is_beta" % tag, changelog_fragments.is_beta(mapped) == pre)
        check("case %s -> %s" % (tag, version), mapped == version)
        check("case %s notes name" % tag, notes == tag[1:])
        check("case %s prerelease flag" % tag, pre == ("~" in version))
    for bad in ("5.0.0", "v5.0", "v5.0.0-beta", "v5.0.0-beta.0", "v5.0.0-beta.01", "v5.0.0-rc.1",
                "v5.0.0-beta.1-x", "v5.0.0~beta1", "v5.0.0-beta.1\n", ""):
        try:
            map_tag(bad)
            failures.append("accepted malformed tag %r" % bad)
        except ValueError:
            pass
    order = ["5.0.0~beta1", "5.0.0~beta2", "5.0.0", "5.0.1", "5.1.0~beta1"]
    try:
        for lo, hi in zip(order, order[1:]):
            if subprocess.run(["dpkg", "--compare-versions", lo, "lt", hi]).returncode != 0:
                failures.append("dpkg order: %s is not below %s" % (lo, hi))
    except FileNotFoundError:
        print("dpkg not installed: version order not checked", file=sys.stderr)
    for f in failures:
        print("SELF-TEST FAIL: %s" % f, file=sys.stderr)
    print("release_tag self-test %s." % ("FAILED" if failures else "OK"))
    return 1 if failures else 0


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    if len(argv) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        version, pre, notes = map_tag(argv[0])
    except ValueError as e:
        print("ERROR: %s" % e, file=sys.stderr)
        return 1
    print("version=%s\nprerelease=%s\nnotes_name=%s" % (version, str(pre).lower(), notes))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
