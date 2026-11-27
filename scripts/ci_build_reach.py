#!/usr/bin/env python3
"""Decide whether a pull request's changed files can reach the CI build and test jobs.

ci.yml's `build-reach` job runs this on a pull request and skips the build,
all-vendors and ASan/UBSan jobs when no changed file can change what they
build or run (issue #2780). Every other event (the merge queue, a push to
main or stable/**, a dispatch) builds everything, so a wrong "no" here can
only delay a failure to the merge queue, never let it land.

A file is build-unreachable only when it matches UNREACHABLE below; anything
else, a file not listed included, reaches the build. An empty list reaches
the build too, so a failed diff never skips anything.

Usage:
    ci_build_reach.py CHANGED_FILES_LIST   prints build=true or build=false
    ci_build_reach.py --self-test
"""

import re
import sys

# Files no build or test job reads. The static-checks job, which always runs,
# checks every one of them (docs drift, link structure, unicode, the scripts'
# self-tests, zizmor for workflows).
UNREACHABLE = [re.compile(p) for p in (
    r"docs/.*",
    r"changelog\.d/.*",
    r".*\.md",                           # no CMake file, test or run_all_tests.sh reads Markdown
    r"\.github/instructions/.*",
    r"\.github/ISSUE_TEMPLATE/.*",
    r"\.github/scripts/(check-unicode\.py|merge-gate\.sh)",  # run by static-checks and merge-gate.yml;
                                         # coverage-compilers.sh wraps the shards' compilers, so it builds
    r"\.github/workflows/(?!ci\.yml$)[^/]+\.yml",  # ci.yml defines the build jobs, so it reaches
    r"\.claude/.*",
    r"scripts/check_[a-z0-9_]+\.py",     # gate scripts the static-checks job runs
    r"scripts/ci_build_reach\.py",
    r"AlpacaCore/conformu/.*",           # committed ConformU reports
    r"\.lycheeignore",
)]


def reaches_build(changed):
    """True when any changed path can change what the build and test jobs build or run."""
    paths = [p.strip() for p in changed if p.strip()]
    return not paths or any(not any(r.fullmatch(p) for r in UNREACHABLE) for p in paths)


def self_test():
    cases = [
        ([], True, "an empty list builds"),
        (["docs/adoption/0005-task-clock.md", "changelog.d/x.md"], False, "docs and a fragment skip"),
        (["README.md", "AlpacaCore/conformu/ZWO/Fake/report.txt"], False, "Markdown and reports skip"),
        ([".github/workflows/link-check.yml", "scripts/check_docs_drift.py"], False, "a gate script and another workflow skip"),
        ([".github/workflows/ci.yml"], True, "ci.yml builds"),
        (["docs/x.md", "AlpacaCore/src/core/a.cpp"], True, "one source file builds"),
        (["scripts/run_fake_conformu.sh"], True, "a script that is not a gate builds"),
        (["scripts/build_bundled_gphoto.sh"], True, "the gphoto build script builds"),
        (["VERSION"], True, "the version file builds"),
        (["AlpacaCore/tests/CMakeLists.txt"], True, "CMake builds"),
        ([".github/scripts/coverage-compilers.sh"], True, "the coverage compiler wrapper the shards run builds"),
        ([".github/scripts/check-unicode.py", ".github/scripts/merge-gate.sh"], False, "the unicode scan and merge gate skip"),
        ([".claude/commands/pr-checker.md", ".claude/rules/x.md"], False, "Claude config skips"),
        (["AlpacaHTTP/web/notes.md", "debian/README.md"], False, "Markdown skips anywhere: no CMake file, test or package step on a PR reads it"),
    ]
    failures = [name for files, want, name in cases if reaches_build(files) is not want]
    for name in failures:
        print("FAIL: %s" % name)
    print("ci_build_reach self-test %s (%d cases)" % ("FAILED" if failures else "OK", len(cases)))
    return 1 if failures else 0


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        sys.exit(self_test())
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    with open(sys.argv[1], encoding="utf-8") as f:
        print("build=%s" % ("true" if reaches_build(f) else "false"))
