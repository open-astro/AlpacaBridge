#!/usr/bin/env python3
"""Turn a ctest JUnit report into COST properties, so the next run starts the slowest tests first.

`ctest -j` starts tests in descending COST order. Its own memory of earlier
timings, Testing/Temporary/CTestCostData.txt, is useless here: ctest splits each
line of that file at the first space, so a test whose name has a space (every
Catch2 case) never matches its entry, and the file gains a junk line per such
test on every run (CMake 3.28 and 3.31). It also lives in the build tree, which
run_all_tests.sh deletes on entry.

So run_all_tests.sh, given CTEST_COST_DATA=<file>, runs ctest with
--output-junit and calls this script on exit to write <file> as one
`set_tests_properties(<name> PROPERTIES COST <seconds>)` line per test that ran.
AlpacaCore/cmake/CTestCost.cmake includes <file> on the next run, after the
Catch2 cases are discovered. Each name is a CMake bracket argument, so spaces,
quotes, semicolons and brackets in it need no escaping. A test that was not run
or is disabled has no meaningful time and is left out; a test with no line
(a new one) keeps COST 0 and starts last.

Usage: python3 scripts/ctest_cost.py <junit.xml> <out.cmake>
Self-test: python3 scripts/ctest_cost.py --self-test
"""
import os
import sys
import tempfile
import xml.etree.ElementTree as ET

TIMED_STATUSES = ("run", "fail")


def bracket(name):
    """`name` as a CMake bracket argument whose closing bracket it cannot contain."""
    eq = ""
    while "]%s]" % eq in name:
        eq += "="
    return "[%s[%s]%s]" % (eq, name, eq)


def costs(junit_text):
    """(name, seconds) for every testcase that ran, slowest first."""
    out = []
    for case in ET.fromstring(junit_text).iter("testcase"):
        if case.get("status") not in TIMED_STATUSES:
            continue
        out.append((case.get("name"), float(case.get("time", "0"))))
    return sorted(out, key=lambda c: (-c[1], c[0]))


def render(pairs):
    lines = ["# Written by scripts/ctest_cost.py from the last ctest run; read by AlpacaCore/cmake/CTestCost.cmake."]
    lines += ["set_tests_properties(%s PROPERTIES COST %.3f)" % (bracket(n), t) for n, t in pairs]
    return "\n".join(lines) + "\n"


def write(junit_path, out_path):
    with open(junit_path, encoding="utf-8") as f:
        text = render(costs(f.read()))
    out_dir = os.path.dirname(os.path.abspath(out_path))
    os.makedirs(out_dir, exist_ok=True)
    # Written beside the target and renamed, so a failure never leaves half a file.
    fd, tmp = tempfile.mkstemp(dir=out_dir, prefix=".ctest_cost.")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(text)
        os.replace(tmp, out_path)
    except BaseException:
        os.unlink(tmp)
        raise


def self_test():
    failures = []

    def check(name, ok):
        if not ok:
            failures.append(name)

    junit = """<?xml version="1.0" encoding="UTF-8"?>
<testsuite name="(empty)" tests="6">
  <testcase name="quick" classname="quick" time="0.5" status="run"><properties/></testcase>
  <testcase name="OnStep telescope - Park and FindHome race" classname="x" time="266.3" status="run"/>
  <testcase name="quotes &quot;a&quot;; [tag] &amp; &lt;x&gt; ]] ]=]" classname="x" time="2" status="run"/>
  <testcase name="fails" classname="fails" time="9.25" status="fail"><failure message="Failed"/></testcase>
  <testcase name="notrun" classname="notrun" time="0" status="notrun"><skipped message="x"/></testcase>
  <testcase name="disabled" classname="disabled" time="0" status="disabled"/>
</testsuite>"""
    got = costs(junit)
    check("slowest first, failed tests kept",
          [n for n, _ in got] == ["OnStep telescope - Park and FindHome race", "fails",
                                  'quotes "a"; [tag] & <x> ]] ]=]', "quick"])
    check("not-run and disabled tests are left out", all(n not in ("notrun", "disabled") for n, _ in got))
    check("times are read", got[0][1] == 266.3)
    check("a plain name", bracket("quick") == "[[quick]]")
    check("a name holding ]] gets a longer bracket", bracket("a ]] b") == "[=[a ]] b]=]")
    check("a name holding ]] and ]=] gets a longer one still", bracket("]] ]=]") == "[==[]] ]=]]==]")
    text = render(got)
    check("one line per timed test plus the header", len(text.splitlines()) == 1 + len(got))
    check("the line's shape",
          "set_tests_properties([[OnStep telescope - Park and FindHome race]] PROPERTIES COST 266.300)" in text)
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "j.xml")
        with open(src, "w", encoding="utf-8") as f:
            f.write(junit)
        out = os.path.join(d, "sub", "costs.cmake")
        write(src, out)
        with open(out, encoding="utf-8") as f:
            check("write() creates the directory and the file", f.read() == text)
        check("write() leaves no temporary file", sorted(os.listdir(os.path.dirname(out))) == ["costs.cmake"])
    try:
        costs("<testsuite><testcase")
        failures.append("accepted a truncated report")
    except ET.ParseError:
        pass
    for f in failures:
        print("SELF-TEST FAIL: %s" % f, file=sys.stderr)
    print("ctest_cost self-test %s." % ("FAILED" if failures else "OK"))
    return 1 if failures else 0


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        write(argv[0], argv[1])
    except (OSError, ET.ParseError, ValueError) as e:
        print("ERROR: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
