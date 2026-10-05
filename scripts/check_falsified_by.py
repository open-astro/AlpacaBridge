#!/usr/bin/env python3
"""Require a `Falsified by:` line in the PR body for every new test case.

A test case that has never failed pins nothing. Every case a PR adds or
renames must be named in the PR body, under a `Falsified by:` heading, next to
the one production-code mutation that makes it fail:

    Falsified by:
    - "Gemini focuser - move beyond max_step throws InvalidValue": AlpacaCore/src/vendors/gemini/gemini_focuser_driver.cpp:212 delete the `steps > max_step_` range check

Grammar: `- "<case name>": <path>:<line> <change>`. `<path>` must exist in the
checkout and be production code (`AlpacaCore/src|include`, `AlpacaHTTP/src|web`).
A test-helper path (`AlpacaCore/tests`, `AlpacaHTTP/tests`) is accepted only when
the case's own name or tags say the helper is the subject (`Fake*`, `PtyPair`,
`StressCallGuard`, `[stress-guard]`). `<change>` has at least 3 words.

What counts as a new case (read from `git diff <merge-base>...HEAD`):
  * Catch2: a registration macro (the `CATCH_TEST_MACROS` tuple of
    `check_stress_registration.py`, comments stripped the same way) whose name
    is absent from every test file at the merge-base. A moved case is not new;
    a renamed one is.
  * AlpacaHTTP hand-rolled tests: an added top-level block in `main()` (a line
    that is exactly `    {`) must open with `// case: <name>`; the name is then
    treated like a Catch2 name.
  * `AlpacaHTTP/tests/web/*.test.js`: `test('<name>', ...)` calls whose name is
    absent from the merge-base web tests.
  * Parameterised cases (`GENERATE`, `TEMPLATE_TEST_CASE`) take one line per
    case, not per instance. Contract-sweep cases generated from registry rows
    are exempt: they are not macro registrations.

The script checks shape, not truth; the review chain applies the mutation.

The PR body is read from the PR_BODY environment variable (or --body-file),
never from a command line, because it is untrusted text.

Usage:
    check_falsified_by.py [--base REF] [--body-file FILE]
    check_falsified_by.py --self-test
"""

import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_stress_registration import (  # noqa: E402
    _STRING_LITERAL_RUN,
    STRING_LITERAL_RE,
    TEST_CASE_START_RE,
    strip_comments,
    tags_in_literal_run,
    test_case_segments,
)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CATCH_DIRS = ("AlpacaCore/tests/", "AlpacaHTTP/tests/")
CATCH_EXTS = (".cpp", ".cc", ".h", ".hpp")
HAND_ROLLED_DIR = "AlpacaHTTP/tests/"
WEB_DIR = "AlpacaHTTP/tests/web/"

PRODUCTION_PREFIXES = (
    "AlpacaCore/src/",
    "AlpacaCore/include/",
    "AlpacaHTTP/src/",
    "AlpacaHTTP/web/",
)
HELPER_PREFIXES = ("AlpacaCore/tests/", "AlpacaHTTP/tests/")
HELPER_SUBJECT_RE = re.compile(
    r"\bFake[A-Za-z0-9_]*|\bPtyPair\b|\bStressCallGuard\b|\[stress-guard\]"
)

HEADING_RE = re.compile(r"^\s*(?:#{1,6}\s*)?(?:\*\*|__)?\s*falsified by\s*:?\s*(?:\*\*|__)?\s*:?\s*$",
                        re.IGNORECASE)
LINE_RE = re.compile(r'^\s*[-*]\s+"(?P<name>.+)":\s+(?P<path>\S+?):(?P<line>\d+)\s+(?P<change>\S.*)$')

CASE_MARKER_RE = re.compile(r"^\s*//\s*case:\s*(?P<name>\S.*?)\s*$")
WEB_TEST_RE = re.compile(r"""\btest\(\s*(?P<q>['"`])(?P<name>.+?)(?P=q)\s*,""")
CATCH_HEADER_RE = re.compile(
    r"\(\s*(?:[^\"(),]+,\s*)?(?P<name>" + _STRING_LITERAL_RUN + r")(?:,\s*(?P<tags>" + _STRING_LITERAL_RUN + r"))?")


def literal_value(run):
    """The concatenated body of a run of adjacent string literals."""
    return "".join(STRING_LITERAL_RE.findall(run))


def catch_cases(text):
    """{name: tags} for every Catch2 registration in `text` (comments stripped)."""
    cases = {}
    for _label, segment in test_case_segments(strip_comments(text)):
        m = CATCH_HEADER_RE.match(segment[TEST_CASE_START_RE.match(segment).end() - 1:])
        if not m:
            continue
        tags = tags_in_literal_run(m.group("tags")) if m.group("tags") else set()
        cases[literal_value(m.group("name"))] = tags
    return cases


def web_cases(text):
    """{name: set()} for each `test('<name>', ...)` call in a web test file."""
    return {m.group("name"): set() for m in WEB_TEST_RE.finditer(strip_comments(text))}


HUNK_RE = re.compile(r"^@@ -\S+ \+(\d+)")
MAIN_RE = re.compile(r"^int\s+main\s*\(", re.M)


def hand_rolled_cases(diff_text, head_text):
    """(names, errors) for the added top-level `main()` blocks in a -U0 unified diff.

    An added line that is exactly `    {` and sits after the `int main(` line of
    `head_text` (the file at HEAD) opens a block; the next added line must be
    `// case: <name>`. Scoped blocks in helper functions above `main()` are not cases.
    """
    m = MAIN_RE.search(head_text)
    main_line = head_text.count("\n", 0, m.start()) + 1 if m else None
    added, lineno, n = [], [], 0
    for ln in diff_text.splitlines():
        h = HUNK_RE.match(ln)
        if h:
            n = int(h.group(1))
        elif ln.startswith("+") and not ln.startswith("+++"):
            added.append(ln[1:])
            lineno.append(n)
            n += 1
    names, errors = {}, []
    for i, ln in enumerate(added):
        if ln.rstrip() != "    {" or main_line is None or lineno[i] <= main_line:
            continue
        nxt = next((a for a in added[i + 1:i + 3] if a.strip()), "")
        m = CASE_MARKER_RE.match(nxt)
        if m:
            names[m.group("name")] = set()
        else:
            errors.append("added top-level block in an AlpacaHTTP test has no `// case: <name>` marker "
                          "on its first line (block at line %d)" % lineno[i])
    return names, errors


def parse_body(body):
    """([(name, path, change)], [error]) from the `Falsified by:` section."""
    lines = body.replace("\r\n", "\n").split("\n")
    start = next((i for i, ln in enumerate(lines) if HEADING_RE.match(ln)), None)
    if start is None:
        return [], []
    entries, errors = [], []
    for ln in lines[start + 1:]:
        if not ln.strip():
            continue
        if re.match(r"^\s*#{1,6}\s", ln) or not re.match(r"^\s*[-*]\s", ln):
            break
        m = LINE_RE.match(ln)
        if not m:
            errors.append('malformed line (want `- "<case>": <path>:<line> <change>`): %r' % ln.strip())
            continue
        if len(m.group("change").split()) < 3:
            errors.append("change has fewer than 3 words for %r" % m.group("name"))
            continue
        entries.append((m.group("name"), m.group("path"), m.group("change")))
    return entries, errors


def path_error(name, path, tags, exists):
    if not exists(path):
        return "%r: path does not exist: %s" % (name, path)
    if path.startswith(PRODUCTION_PREFIXES):
        return None
    if path.startswith(HELPER_PREFIXES):
        subject = " ".join([name] + ["[%s]" % t for t in sorted(tags)])
        if HELPER_SUBJECT_RE.search(subject):
            return None
        return "%r: %s is a test helper and the case name/tags do not say the helper is its subject" % (name, path)
    return "%r: %s is not production code (AlpacaCore/src|include, AlpacaHTTP/src|web)" % (name, path)


def evaluate(new_cases, entries, body_errors, exists, head_cases=None):
    """Every problem found, given {name: tags} of new cases and the parsed body.

    A line naming a case that is not new but exists at HEAD is accepted: it is
    the evidence for a fix to an existing case. A line naming neither is stale.
    """
    head_cases = head_cases or {}
    errors = list(body_errors)
    claimed = {}
    for name, path, _change in entries:
        claimed.setdefault(name, path)
    for name in sorted(new_cases):
        if name not in claimed:
            errors.append("new test case has no `Falsified by:` line: %r" % name)
    for name, path, _change in entries:
        if name in new_cases:
            tags = new_cases[name]
        elif name in head_cases:
            tags = head_cases[name]
        else:
            errors.append("`Falsified by:` line names no test case at HEAD (stale or typo): %r" % name)
            continue
        err = path_error(name, path, tags, exists)
        if err:
            errors.append(err)
    return errors


# --- git plumbing ----------------------------------------------------------

def git(*args, check=True):
    r = subprocess.run(["git", "-C", ROOT, *args], capture_output=True, text=True, errors="replace")
    if check and r.returncode != 0:
        raise RuntimeError("git %s failed: %s" % (" ".join(args), r.stderr.strip()))
    return r.stdout


def resolve_base(explicit):
    for ref in ([explicit] if explicit else []) + ["upstream/main", "origin/main", "main"]:
        r = subprocess.run(["git", "-C", ROOT, "merge-base", ref, "HEAD"], capture_output=True, text=True)
        if r.returncode == 0:
            return r.stdout.strip()
    raise RuntimeError("cannot resolve a diff base (tried %s)" % (explicit or "upstream/main, origin/main, main"))


def is_catch_file(path):
    return path.startswith(CATCH_DIRS) and path.endswith(CATCH_EXTS)


def is_web_file(path):
    return path.startswith(WEB_DIR) and path.endswith(".test.js")


def is_hand_rolled_file(path):
    return path.startswith(HAND_ROLLED_DIR) and path.endswith(".cpp")


def case_names_in(path, text):
    """Every case name `text` (the content of `path`) declares, of any kind."""
    names = {}
    if is_web_file(path):
        names.update(web_cases(text))
    elif is_catch_file(path):
        names.update(catch_cases(text))
        if is_hand_rolled_file(path):
            names.update({m.group("name"): set()
                          for m in map(CASE_MARKER_RE.match, text.splitlines()) if m})
    return names


def new_cases_in(path, text, base_names, diff_for_path):
    """({name: tags}, [error]) for the cases `text` adds over `base_names`.

    `diff_for_path()` returns the unified diff of `path`; it is read only for a
    hand-rolled test file with no Catch2 cases.
    """
    errors = []
    if is_web_file(path):
        found = web_cases(text)
    elif is_catch_file(path):
        found = catch_cases(text)
        if is_hand_rolled_file(path) and not found:
            found, errs = hand_rolled_cases(diff_for_path(), text)
            errors.extend("%s: %s" % (path, e) for e in errs)
    else:
        found = {}
    return {n: t for n, t in found.items() if n not in base_names}, errors


def gather_new_cases(base):
    """(new, errors, head_cases) for the diff `base...HEAD`."""
    changed = set(git("diff", "-z", "--name-only", "--diff-filter=AMR", base + "...HEAD").split("\0"))
    base_names = set()
    for path in git("ls-tree", "-r", "-z", "--name-only", base, "--", "AlpacaCore/tests", "AlpacaHTTP/tests").split("\0"):
        if not path:
            continue
        base_names.update(case_names_in(path, git("show", "%s:%s" % (base, path), check=False)))
    new, errors, head_cases = {}, [], {}
    for path in git("ls-files", "-z", "AlpacaCore/tests", "AlpacaHTTP/tests").split("\0"):
        if not path:
            continue
        full = os.path.join(ROOT, path)
        if not os.path.isfile(full):
            continue
        with open(full, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        head_cases.update(case_names_in(path, text))
        if path in changed:
            found, errs = new_cases_in(path, text, base_names,
                                       lambda p=path: git("diff", "-U0", base + "...HEAD", "--", p))
            new.update(found)
            errors.extend(errs)
    return new, errors, head_cases


def main(argv):
    base_arg, body_file = None, None
    args = iter(argv)
    for a in args:
        if a == "--base":
            base_arg = next(args, None)
        elif a == "--body-file":
            body_file = next(args, None)
        else:
            print("unknown argument: %s" % a, file=sys.stderr)
            return 2
    if body_file:
        with open(body_file, encoding="utf-8", errors="replace") as fh:
            body = fh.read()
    else:
        body = os.environ.get("PR_BODY", "")
    try:
        base = resolve_base(base_arg or os.environ.get("FALSIFIED_BASE"))
        new, errors, head_cases = gather_new_cases(base)
    except RuntimeError as exc:
        print("falsified-by: ERROR: %s" % exc, file=sys.stderr)
        return 2
    entries, body_errors = parse_body(body)
    errors = errors + evaluate(new, entries, body_errors, lambda p: os.path.exists(os.path.join(ROOT, p)),
                               head_cases)
    if not new and not entries and not errors:
        print("falsified-by: no new test cases; nothing to check")
        return 0
    if errors:
        for e in errors:
            print("FALSIFIED-BY: " + e)
        print("falsified-by: FAIL (%d problem(s)); see the `Falsified by:` rule in AGENTS.md Testing Requirements" % len(errors))
        return 1
    print("falsified-by: OK (%d new case(s), each with a mutation line)" % len(new))
    return 0


# --- self-test -------------------------------------------------------------

def self_test():
    failures = []

    def check(name, cond):
        if not cond:
            failures.append(name)

    always = lambda p: True  # noqa: E731
    never = lambda p: False  # noqa: E731
    prod = "AlpacaCore/src/x/a.cpp"

    # Catch2 extraction: comments stripped, tags read, concatenated literals joined.
    src = '''
// TEST_CASE("commented out", "[x]")
TEST_CASE("Real one", "[v][stress-guard]") { }
TEST_CASE_METHOD(Fx, "Method case", "[m]") { }
TEMPLATE_TEST_CASE("Templated", "[t]", int, long) { }
TEST_CASE("Con" "cat", "[a]" "[b]") { }
'''
    cases = catch_cases(src)
    check("catch comment stripped", "commented out" not in cases)
    check("catch plain", cases.get("Real one") == {"v", "stress-guard"})
    check("catch method", "Method case" in cases)
    check("catch parameterised is one case", "Templated" in cases and len(cases) == 4)
    check("catch concatenated", cases.get("Concat") == {"a", "b"})

    # Moved vs renamed, through the same functions gather_new_cases calls.
    cpp = "AlpacaCore/tests/t.cpp"
    nodiff = lambda: ""  # noqa: E731
    base_names = set(case_names_in(cpp, 'TEST_CASE("Old name", "[x]") {}'))
    moved, _e = new_cases_in(cpp, '// moved\nTEST_CASE("Old name", "[x]") {}', base_names, nodiff)
    renamed, _e = new_cases_in(cpp, 'TEST_CASE("Old name 2", "[x]") {}', base_names, nodiff)
    check("moved case is not new", not moved)
    check("renamed case is new", set(renamed) == {"Old name 2"})
    wjs = "AlpacaHTTP/tests/web/a.test.js"
    wbase = set(case_names_in(wjs, "test('old web', () => {});"))
    moved, _e = new_cases_in(wjs, "test('old web', () => {});", wbase, nodiff)
    renamed, _e = new_cases_in(wjs, "test('new web', () => {});", wbase, nodiff)
    check("web moved not new", not moved)
    check("web renamed is new", set(renamed) == {"new web"})
    check("web comment stripped", "ghost" not in web_cases("// test('ghost', f);\n/* test('ghost2', f); */"))
    hr = "AlpacaHTTP/tests/test_h.cpp"
    hr_base = "int main() {\n    {\n        // case: moved block\n        EXPECT(1);\n    }\n}\n"
    hr_names = set(case_names_in(hr, hr_base))
    check("hand-rolled base names collected", hr_names == {"moved block"})
    hr_diff = lambda: "@@ -0,0 +3,4 @@\n+    {\n+        // case: moved block\n+        EXPECT(1);\n+    }\n"  # noqa: E731
    moved, errs = new_cases_in(hr, "int main() {\n}\n", hr_names, hr_diff)
    check("hand-rolled moved block is not new", not moved and not errs)
    fresh, errs = new_cases_in(hr, "int main() {\n}\n", set(), hr_diff)
    check("hand-rolled new block is new", set(fresh) == {"moved block"} and not errs)
    _n, errs = new_cases_in(hr, "int main() {\n}\n", set(), lambda: "@@ -0,0 +3,2 @@\n+    {\n+        EXPECT(1);\n")
    check("hand-rolled missing marker error", len(errs) == 1)

    # Hand-rolled blocks: only after the `int main(` line of the HEAD file.
    head = "static void helper() {\n    {\n    }\n}\nint main() {\n    {\n    }\n}\n"
    diff = "+++ b/f.cpp\n@@ -0,0 +6,4 @@\n+    {\n+        // case: marker one\n+        EXPECT(1);\n+    }\n"
    names, errs = hand_rolled_cases(diff, head)
    check("hand-rolled marker", "marker one" in names and not errs)
    names, errs = hand_rolled_cases("@@ -0,0 +6,3 @@\n+    {\n+        EXPECT(1);\n+    }\n", head)
    check("hand-rolled missing marker", not names and len(errs) == 1)
    names, errs = hand_rolled_cases("@@ -0,0 +6,2 @@\n+        {\n+            x();\n", head)
    check("nested block ignored", not names and not errs)
    names, errs = hand_rolled_cases("@@ -0,0 +2,3 @@\n+    {\n+        x();\n+    }\n", head)
    check("scoped block in a helper before main is not a case", not names and not errs)
    names, errs = hand_rolled_cases("@@ -0,0 +6,3 @@\n+    {\n+        x();\n+    }\n", "void f() {}\n")
    check("file without main has no cases", not names and not errs)
    span = "static void helper() {\n    {\n    }\n}\nint main() {\n    {\n        // case: spans main\n    }\n}\n"
    diff = "@@ -0,0 +1,9 @@\n" + "".join("+" + x + "\n" for x in span.splitlines())
    names, errs = hand_rolled_cases(diff, span)
    check("new file with a hunk spanning main counts only blocks after main", set(names) == {"spans main"} and not errs)

    # Web tests.
    web = "test('first web', () => {});\ntest(\"second web\", async () => {});\nit('not a test call', f);"
    check("web test names", set(web_cases(web)) == {"first web", "second web"})

    # Body parsing: accepted shapes.
    ok = 'Intro\n\nFalsified by:\n- "A case": %s:12 delete the range check\n- "B case": AlpacaHTTP/web/app.js:3 drop the guard clause\n\n## Next\n' % prod
    entries, errs = parse_body(ok)
    check("body accepted", len(entries) == 2 and not errs)
    entries, errs = parse_body('Falsified by:\n- "A": %s:1 delete the check now\n\n- "B": %s:2 drop the other guard\n' % (prod, prod))
    check("loose list keeps later entries", len(entries) == 2 and not errs)
    entries, errs = parse_body('**Falsified by:**\n* "A": %s:1 delete the check now\n' % prod)
    check("bold heading and star bullet", len(entries) == 1 and not errs)
    entries, errs = parse_body('## Falsified by\n- "A": %s:1 delete the check now\n' % prod)
    check("markdown heading", len(entries) == 1 and not errs)
    # Rejected shapes.
    for label, line in [
        ("no quotes", '- A case: %s:1 delete the check now' % prod),
        ("no line number", '- "A": %s delete the check now' % prod),
        ("no change", '- "A": %s:1' % prod),
        ("no colon after name", '- "A" %s:1 delete the check now' % prod),
    ]:
        _e, errs = parse_body("Falsified by:\n%s\n" % line)
        check("rejected: " + label, len(errs) == 1)
    _e, errs = parse_body('Falsified by:\n- "A": %s:1 delete check\n' % prod)
    check("rejected: change under 3 words", len(errs) == 1)
    entries, errs = parse_body("no section at all")
    check("no section", not entries and not errs)

    # Evaluation.
    new = {"A": set(), "B": set()}
    check("all covered", not evaluate(new, [("A", prod, "x y z"), ("B", prod, "x y z")], [], always))
    errs = evaluate(new, [("A", prod, "x y z")], [], always)
    check("missing line reported", len(errs) == 1 and "'B'" in errs[0])
    errs = evaluate(new, [("A", prod, "x y z"), ("B", prod, "x y z"), ("Typo", prod, "x y z")], [], always)
    check("stale line reported", len(errs) == 1 and "Typo" in errs[0])
    errs = evaluate({"A": set()}, [("A", prod, "x y z")], [], never)
    check("missing path reported", len(errs) == 1 and "does not exist" in errs[0])
    errs = evaluate({"A": set()}, [("A", "scripts/foo.py", "x y z")], [], always)
    check("non-production path reported", len(errs) == 1 and "not production" in errs[0])
    errs = evaluate({"A": set()}, [("A", "AlpacaCore/tests/t.cpp", "x y z")], [], always)
    check("helper path rejected for plain case", len(errs) == 1 and "test helper" in errs[0])
    for nm, tg in [("FakeSdk throws", set()), ("PtyPair closes", set()), ("StressCallGuard counts", set()),
                   ("Harness self-test", {"stress-guard"})]:
        errs = evaluate({nm: tg}, [(nm, "AlpacaCore/tests/fake.h", "x y z")], [], always)
        check("helper path accepted: " + nm, not errs)
    for nm, tg in [("Driver connects over the fake", set()), ("Camera connect", {"fakesdk"}),
                   ("Camera connect", {"fake"})]:
        errs = evaluate({nm: tg}, [(nm, "AlpacaCore/tests/fake_a.h", "x y z")], [], always)
        check("helper path rejected for lowercase fake: %s %s" % (nm, sorted(tg)),
              len(errs) == 1 and "test helper" in errs[0])
    # A line for an existing (not new) case is accepted; one naming no case is stale.
    check("existing case accepted", not evaluate({}, [("Old", prod, "x y z")], [], always, {"Old": set()}))
    errs = evaluate({}, [("Ghost", prod, "x y z")], [], always, {"Old": set()})
    check("stale line with no new cases", len(errs) == 1 and "Ghost" in errs[0])
    errs = evaluate({"A": set()}, [("A", prod, "x y z")], ["malformed line"], always)
    check("body errors carried", errs == ["malformed line"])

    if failures:
        for f in failures:
            print("SELF-TEST FAIL: " + f)
        return 1
    print("check_falsified_by.py self-test: OK")
    return 0


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        sys.exit(self_test())
    sys.exit(main(sys.argv[1:]))
