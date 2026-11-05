#!/usr/bin/env python3
"""Duplicated-line ratchet: the share of authored C++ lines that PMD CPD finds
duplicated may not grow (#1755).

Runs PMD CPD over the authored trees (SCAN_DIRS) and fails when the
duplicated-line share is above MAX_DUPLICATED_LINE_SHARE. A line counts once
however many duplications cover it: the ranges CPD reports are merged per
file before they are counted. A PR that removes duplication lowers the
constant to the new figure so the ratchet holds.

CPD itself is pinned (version and sha256) in .github/workflows/ci.yml's
`duplication` job and in scripts/ci_preflight.sh; scripts/check_docs_drift.py
check 2 keeps the two pins identical.

Usage:
    check_duplication.py --pmd <path to pmd launcher>
    check_duplication.py --self-test
"""
import os
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCAN_DIRS = ("AlpacaCore/src", "AlpacaCore/include", "AlpacaHTTP/src", "AlpacaHTTP/include")
EXTENSIONS = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx")
MINIMUM_TOKENS = 100
# The share measured on the merge base, rounded up to the next 0.1 point; lower it,
# never raise it.
MAX_DUPLICATED_LINE_SHARE = 13.0  # percent


def merge_ranges(ranges):
    """Merge inclusive (first, last) line ranges; an inverted range is an error."""
    for first, last in ranges:
        if last < first:
            raise ValueError("inverted CPD range %d-%d" % (first, last))
    merged = []
    for first, last in sorted(ranges):
        if merged and first <= merged[-1][1] + 1:
            merged[-1][1] = max(merged[-1][1], last)
        else:
            merged.append([first, last])
    return [tuple(r) for r in merged]


def duplicated_lines(cpd_xml):
    """{path: duplicated line count} from CPD's XML report."""
    per_file = {}
    for dup in ET.fromstring(cpd_xml).iter():
        if dup.tag.rsplit("}", 1)[-1] != "duplication":
            continue
        for f in dup:
            if f.tag.rsplit("}", 1)[-1] != "file":
                continue
            first = int(f.get("line"))
            last = int(f.get("endline", f.get("line")))
            per_file.setdefault(f.get("path"), []).append((first, last))
    return {path: sum(last - first + 1 for first, last in merge_ranges(r)) for path, r in per_file.items()}


def total_lines(root=ROOT):
    total = 0
    for d in SCAN_DIRS:
        for dirpath, _, names in os.walk(os.path.join(root, d)):
            for name in names:
                if name.endswith(EXTENSIONS):
                    with open(os.path.join(dirpath, name), "rb") as fh:
                        total += sum(1 for _ in fh)
    return total


def run_cpd(pmd):
    cmd = [pmd, "cpd", "--language", "cpp", "--minimum-tokens", str(MINIMUM_TOKENS), "--format", "xml",
           "--no-fail-on-violation"]
    for d in SCAN_DIRS:
        cmd += ["--dir", os.path.join(ROOT, d)]
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        raise RuntimeError("pmd cpd exited %d" % proc.returncode)
    return proc.stdout


def verdict(duplicated, total, limit=MAX_DUPLICATED_LINE_SHARE):
    """(ok, message)."""
    if total <= 0:
        return False, "scanned no lines: SCAN_DIRS or EXTENSIONS are stale"
    share = 100.0 * duplicated / total
    msg = "duplicated lines: %d of %d (%.2f%%, limit %.2f%%)" % (duplicated, total, share, limit)
    if share > limit:
        return False, msg + ": the share grew; remove the duplication instead of raising the limit"
    return True, msg


def main(argv):
    if len(argv) != 2 or argv[0] != "--pmd":
        sys.stderr.write(__doc__)
        return 2
    try:
        counts = duplicated_lines(run_cpd(argv[1]))
    except (RuntimeError, ValueError, ET.ParseError) as e:
        print("duplication check FAILED: %s" % e)
        return 1
    ok, msg = verdict(sum(counts.values()), total_lines())
    print(msg)
    return 0 if ok else 1


def self_test():
    checks = []

    def check(name, cond):
        checks.append((name, bool(cond)))

    check("overlapping ranges merge", merge_ranges([(1, 10), (5, 12)]) == [(1, 12)])
    check("nested ranges count once", merge_ranges([(1, 20), (4, 8)]) == [(1, 20)])
    check("adjacent ranges merge", merge_ranges([(1, 3), (4, 6)]) == [(1, 6)])
    check("disjoint ranges stay apart", merge_ranges([(10, 12), (1, 3)]) == [(1, 3), (10, 12)])
    try:
        merge_ranges([(9, 3)])
        check("an inverted range fails loudly", False)
    except ValueError as e:
        check("an inverted range fails loudly", "inverted" in str(e))
    xml = """<?xml version="1.0"?>
<pmd-cpd xmlns="https://pmd-code.org/schema/cpd-report">
  <duplication lines="10" tokens="120">
    <file line="1" endline="10" path="/r/a.cpp"/>
    <file line="5" endline="14" path="/r/a.cpp"/>
  </duplication>
  <duplication lines="4" tokens="100">
    <file line="2" endline="5" path="/r/a.cpp"/>
    <file line="20" endline="23" path="/r/b.cpp"/>
  </duplication>
</pmd-cpd>"""
    check("CPD XML: overlap and nesting across duplications counted once",
          duplicated_lines(xml) == {"/r/a.cpp": 14, "/r/b.cpp": 4})
    bad = xml.replace('line="20" endline="23"', 'line="23" endline="20"')
    try:
        duplicated_lines(bad)
        check("CPD XML: an inverted range fails loudly", False)
    except ValueError:
        check("CPD XML: an inverted range fails loudly", True)
    check("verdict passes at the limit", verdict(13, 100, 13.0)[0])
    check("verdict fails above the limit", not verdict(14, 100, 13.0)[0])
    check("verdict fails on an empty scan", not verdict(0, 0)[0])
    check("the authored trees have lines to scan", total_lines() > 10000)

    failed = [name for name, ok in checks if not ok]
    for name, ok in checks:
        print("[%s] %s" % ("PASS" if ok else "FAIL", name))
    if failed:
        print("\n%d/%d self-test(s) failed." % (len(failed), len(checks)))
        return 1
    print("\nAll %d self-test(s) passed." % len(checks))
    return 0


if __name__ == "__main__":
    if "--self-test" in sys.argv[1:]:
        sys.exit(self_test())
    sys.exit(main(sys.argv[1:]))
