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

    The diff only says which lines were added; the marker text is read from
    `head_text` (the file at HEAD), because git may slide an inserted block so
    that the added `    {` is the next existing block's opener (issue #993). So a
    block is judged by its body, not its opener: for each `    {` after `main(` in
    `head_text`, a first body line that is an added `// case: <name>` is collected;
    a block whose body lines were ALL added and which lacks that marker is an error.
    Any other block (an untouched body line, no added marker) existed before and is skipped.
    Scoped blocks in helper functions above `main()` are not cases.
    """
    m = MAIN_RE.search(head_text)
    main_line = head_text.count("\n", 0, m.start()) + 1 if m else None
    if main_line is None:
        return {}, []
    head = head_text.splitlines()
    added, n = [], 0
    for ln in diff_text.splitlines():
        h = HUNK_RE.match(ln)
        if h:
            n = int(h.group(1))
        elif ln.startswith("+") and not ln.startswith("+++"):
            added.append(n)
            n += 1

    def line(no):
        return head[no - 1] if 1 <= no <= len(head) else ""

    added_set = set(added)
    names, errors = {}, []
    for no in range(main_line + 1, len(head) + 1):
        if line(no).rstrip() != "    {":
            continue
        body = []
        for k in range(no + 1, len(head) + 1):
            if line(k).rstrip() == "    }":
                break
            if line(k).strip():
                body.append(k)
        if not body:
            continue
        first = body[0]
        mk = CASE_MARKER_RE.match(line(first))
        if mk and first in added_set:
            # An added marker names a case even when a later body line is unchanged (a split block).
            names[mk.group("name")] = set()
        elif not mk and all(k in added_set for k in body):
            # No marker is an error only for a wholly added block: a shared opener (slider
            # placement) or an edit to the top of an existing block leaves a body line untouched.
            errors.append("added or rewritten top-level block in an AlpacaHTTP test has no "
                          "`// case: <name>` marker on its first line (block at line %d)" % no)
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


MERGE_PARENT = "merge-parent"


def resolve_base(explicit):
    """Merge-base of HEAD and the first match among `explicit` and the defaults.

    `explicit == "merge-parent"` (the CI job) means HEAD^1 when HEAD is a merge
    commit: on a pull_request run HEAD is refs/pull/N/merge, built against the
    CURRENT tip of the base branch, so its first parent is the exact base and a
    base that moved after the PR opened adds none of the cases other PRs landed
    (issue #992; pull_request.base.sha is the tip when the PR opened). Anywhere
    else HEAD has no second parent and the usual defaults apply.
    """
    if explicit == MERGE_PARENT:
        explicit = None
        if subprocess.run(["git", "-C", ROOT, "rev-parse", "--verify", "-q", "HEAD^2"],
                          capture_output=True).returncode == 0:
            r = subprocess.run(["git", "-C", ROOT, "rev-parse", "HEAD^1"], capture_output=True, text=True)
            if r.returncode == 0:
                return r.stdout.strip()
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

def moved_base_self_test():
    """Scratch repo: the base branch gains a case after the PR branched (issue #992)."""
    global ROOT
    import tempfile
    failures = []
    saved = ROOT
    # Isolated git: no user config, and no GIT_DIR from an enclosing hook
    # pointing the scratch repo's git at the real repository.
    git_env = {"GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1"}
    stripped = ("GIT_DIR", "GIT_INDEX_FILE", "GIT_WORK_TREE", "GIT_COMMON_DIR")
    saved_env = {k: os.environ.get(k) for k in list(git_env) + list(stripped)}
    os.environ.update(git_env)
    for name in stripped:
        os.environ.pop(name, None)
    with tempfile.TemporaryDirectory() as tmp:
        ROOT = tmp

        def commit(msg, name, *cases):
            path = os.path.join(tmp, "AlpacaCore/tests", name)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as fh:
                fh.write("".join('TEST_CASE("%s", "[x]") {}\n' % c for c in cases))
            git("add", "-A")
            git("-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false", "commit", "-q", "-m", msg)

        def sha():
            return git("rev-parse", "HEAD").strip()

        try:
            git("init", "-q", "-b", "trunk")
            commit("base", "test_a.cpp", "Old")
            opened = sha()
            git("checkout", "-q", "-b", "pr")
            commit("pr", "test_a.cpp", "Old", "Mine")
            commit("pr again", "test_c.cpp", "Mine2")
            git("checkout", "-q", "trunk")
            commit("other pr", "test_b.cpp", "Other")
            # refs/pull/N/merge: first parent the moved base tip, second the PR head.
            git("checkout", "-q", "-B", "merge", "trunk")
            subprocess.run(["git", "-C", tmp, "-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false",
                            "merge", "-q", "--no-ff", "-m", "merge", "pr"], capture_output=True)
            old = gather_new_cases(resolve_base(opened))[0]
            new = gather_new_cases(resolve_base(MERGE_PARENT))[0]
            if set(old) != {"Mine", "Mine2", "Other"}:
                failures.append("moved base: old base.sha choice flags the other PR's case")
            if set(new) != {"Mine", "Mine2"}:
                failures.append("moved base: merge-parent base flags only this PR's cases")
            # Not a merge commit: falls back to the defaults (main here). The PR has two
            # commits, so HEAD^1 would drop Mine and a dropped HEAD^2 guard shows.
            git("branch", "main", opened)
            git("checkout", "-q", "-B", "plain", "pr")
            if set(gather_new_cases(resolve_base(MERGE_PARENT))[0]) != {"Mine", "Mine2"}:
                failures.append("merge-parent falls back when HEAD is not a merge commit")
        except Exception as exc:  # noqa: BLE001
            failures.append("moved base scratch repo: %s" % exc)
        finally:
            ROOT = saved
            for k, v in saved_env.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v
    return failures


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
    hr_diff = lambda: "@@ -0,0 +2,4 @@\n+    {\n+        // case: moved block\n+        EXPECT(1);\n+    }\n"  # noqa: E731
    moved, errs = new_cases_in(hr, hr_base, hr_names, hr_diff)
    check("hand-rolled moved block is not new", not moved and not errs)
    fresh, errs = new_cases_in(hr, hr_base, set(), hr_diff)
    check("hand-rolled new block is new", set(fresh) == {"moved block"} and not errs)
    _n, errs = new_cases_in(hr, "int main() {\n    {\n        EXPECT(1);\n    }\n}\n", set(),
                            lambda: "@@ -0,0 +2,2 @@\n+    {\n+        EXPECT(1);\n")
    check("hand-rolled missing marker error", len(errs) == 1)

    # Hand-rolled blocks: only after the `int main(` line of the HEAD file.
    diff = "+++ b/f.cpp\n@@ -0,0 +6,4 @@\n+    {\n+        // case: marker one\n+        EXPECT(1);\n+    }\n"
    head = "static void helper() {\n    {\n    }\n}\nint main() {\n    {\n        // case: marker one\n        EXPECT(1);\n    }\n}\n"
    names, errs = hand_rolled_cases(diff, head)
    check("hand-rolled marker", "marker one" in names and not errs)
    nomark = "static void helper() {\n    {\n    }\n}\nint main() {\n    {\n        EXPECT(1);\n    }\n}\n"
    names, errs = hand_rolled_cases("@@ -0,0 +6,3 @@\n+    {\n+        EXPECT(1);\n+    }\n", nomark)
    check("hand-rolled missing marker", not names and len(errs) == 1)
    nested = "int main() {\n    {\n        // case: outer\n        {\n            x();\n        }\n    }\n}\n"
    names, errs = hand_rolled_cases("@@ -0,0 +4,3 @@\n+        {\n+            x();\n+        }\n", nested)
    check("nested block ignored", not names and not errs)
    names, errs = hand_rolled_cases("@@ -0,0 +2,3 @@\n+    {\n+        x();\n+    }\n", head)
    check("scoped block in a helper before main is not a case", not names and not errs)
    # Issue #993: a new block inserted directly before an existing one. Git slides it, so the
    # added lines are the new block's body plus the NEXT block's `    {`; the old reading
    # saw that `{` with no added marker after it.
    slid = ("int main() {\n    {\n        // case: new first\n        EXPECT(1);\n    }\n"
            "    {\n        // case: old second\n        EXPECT(2);\n    }\n}\n")
    sdiff = "@@ -2,0 +3,4 @@\n+        // case: new first\n+        EXPECT(1);\n+    }\n+    {\n"
    names, errs = hand_rolled_cases(sdiff, slid)
    check("slid block: real case name collected, no false marker error", set(names) == {"new first"} and not errs)
    unm = ("int main() {\n    {\n        // case: new first\n        EXPECT(1);\n    }\n"
           "    {\n        EXPECT(2);\n    }\n}\n")
    names, errs = hand_rolled_cases(sdiff, unm)
    check("slid block before an unmarked existing block: no false error", set(names) == {"new first"} and not errs)
    two = "int main() {\n    {\n        EXPECT(1);\n    }\n    {\n        EXPECT(2);\n    }\n}\n"
    names, errs = hand_rolled_cases("@@ -0,0 +3,3 @@\n+        EXPECT(1);\n+    }\n+    {\n", two)
    check("unmarked new block before an existing one: one error", not names and len(errs) == 1)
    mid = "int main() {\n    {\n        EXPECT(1);\n        // case: mid\n    }\n        {\n        // case: nest\n        }\n}\n"
    names, errs = hand_rolled_cases("@@ -0,0 +4,1 @@\n+        // case: mid\n@@ -0,0 +7,1 @@\n+        // case: nest\n", mid)
    check("marker not right after a top-level opener is not a case", not names and not errs)
    edit = "int main() {\n    {\n        Router r(cfg);\n        EXPECT(1);\n    }\n}\n"
    names, errs = hand_rolled_cases("@@ -3 +3 @@\n-        Router r;\n+        Router r(cfg);\n", edit)
    check("first line of an existing unmarked block replaced: no names, no error", not names and not errs)
    names, errs = hand_rolled_cases("@@ -2,0 +3 @@\n+        Router r(cfg);\n", edit)
    check("line inserted at top of an existing unmarked block: no names, no error", not names and not errs)
    split = ("int main() {\n    {\n        // case: x\n        EXPECT(1);\n    }\n    {\n"
             "        // case: y\n        EXPECT(2);\n    }\n}\n")
    names, errs = hand_rolled_cases("@@ -4,0 +5,3 @@\n+    }\n+    {\n+        // case: y\n", split)
    check("split block: added marker with a later unchanged body line is collected", set(names) == {"y"} and not errs)
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

    failures.extend(moved_base_self_test())

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
