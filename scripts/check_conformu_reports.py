#!/usr/bin/env python3
"""Fail a PR that adds or touches a failing ConformU report.

`/submit-pr` already refuses to open a PR whose ConformU report is unclean
(see `.claude/commands/submit-pr.md`, "ConformU report validation"), but that
is a prompt-only gate: a PR opened by hand, or from a fork not using the
skill, bypasses it entirely. This script ports the same jq/grep logic into a
plain CI gate so a merged PR can never misadvertise a driver as validated.

Run from the repo root, against a specific base ref to diff from:
    python3 scripts/check_conformu_reports.py <base-ref>
Regression guard for this script's own rules (no repo state needed):
    python3 scripts/check_conformu_reports.py --self-test

Files under BOTH AlpacaCore/conformu/ and AlpacaHTTP/conformu/ that changed
relative to <base-ref> are checked -- an unrelated PR that never touches a
report is a no-op. `/submit-pr`'s own scope is only `AlpacaCore/conformu/**`,
but AlpacaHTTP/conformu/README.md states "All HTTP endpoints must pass
ConformU protocol verification before being considered compliant" and that
directory holds a real report -- a review of this script correctly pointed
out that calling it dead and excluding it would leave exactly the kind of
unguarded gap this script exists to close, so both paths are in scope.

The two directories' reports come from different ConformU check types (a
per-device driver check vs. a protocol-level check across all endpoints) and
use different success wording -- SUCCESS_PATTERNS below covers both observed
phrasings; if ConformU ever changes its wording again, add the new phrase
here rather than looping this check.

Pass criteria (identical to `/submit-pr`):
  - JSON reports (*.json): ErrorCount, IssueCount and TimingIssuesCount must
    all be zero.
  - Text logs (*.txt): must NOT contain a line matching
    "OUTSIDE (FAST|STANDARD|EXTENDED) RESPONSE TIME TARGET" or
    "took longer than its target response time", AND MUST contain one of the
    SUCCESS_PATTERNS strings.
"""

import json
import re
import subprocess
import sys

CONFORMU_PREFIXES = ("AlpacaCore/conformu/", "AlpacaHTTP/conformu/")

TIMING_OUTSIDE_RE = re.compile(r"OUTSIDE (FAST|STANDARD|EXTENDED) RESPONSE TIME TARGET")
TIMING_LONGER_RE = re.compile(r"took longer than its target response time")
# Both are real, observed ConformU pass phrasings (see the module docstring).
SUCCESS_PATTERNS = (
    "Congratulations, no errors, warnings or issues found",
    "Congratulations there were no errors, issues or information alerts",
)


def changed_conformu_files(base_ref):
    merge_base = subprocess.run(
        ["git", "merge-base", base_ref, "HEAD"],
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    out = subprocess.run(
        # --name-status with explicit rename detection: a report renamed
        # without content changes shows as a single R100 entry, which is
        # skipped (nothing new to validate). A rename with edits (R0xx) or a
        # plain add/modify yields the destination path and is checked.
        # --name-only would print the destination in both cases and could
        # not tell them apart (PR #281 review).
        # core.quotePath=false: git would otherwise quote a path with
        # non-ASCII bytes, and the prefix filter below would skip it.
        ["git", "-c", "core.quotePath=false", "diff", "--name-status",
         "--find-renames", "--diff-filter=d", merge_base, "HEAD"],
        check=True, capture_output=True, text=True,
    ).stdout
    paths = []
    for line in out.splitlines():
        fields = line.split("\t")
        if len(fields) < 2:
            continue
        status = fields[0]
        # A pure rename is only "already validated" when the SOURCE was
        # under a validated prefix; a report moved in from anywhere else
        # is new to the gate and must be checked (PR #281 review).
        if status == "R100" and fields[1].startswith(CONFORMU_PREFIXES):
            continue
        paths.append(fields[-1])
    return [p for p in paths if p.startswith(CONFORMU_PREFIXES)
            and p.endswith((".json", ".txt"))]


def check_json(path):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, json.JSONDecodeError) as e:
        return ["%s: could not parse as JSON (%s)" % (path, e)]

    if not isinstance(data, dict):
        return ["%s: top-level JSON is a %s, not an object -- cannot check "
                "ErrorCount/IssueCount/TimingIssuesCount" % (path, type(data).__name__)]

    failures = []
    for field in ("ErrorCount", "IssueCount", "TimingIssuesCount"):
        # No default: a report missing one of these fields entirely (e.g.
        # truncated/malformed) is a hard failure, not a silent pass -- the
        # whole point of this script is to never let an unclear report
        # through.
        if field not in data:
            failures.append("%s: missing required field %r" % (path, field))
            continue
        value = data[field]
        # Explicit numeric comparison, not truthiness: a real ConformU count
        # is always an int, but `if value:` would wrongly pass a JSON `false`
        # or `null` and wrongly fail a numeric-looking string like "0".
        if not isinstance(value, (int, float)) or isinstance(value, bool):
            failures.append("%s: %s=%r is not a number (must be the integer 0)" % (path, field, value))
        elif value != 0:
            failures.append("%s: %s=%s (must be 0)" % (path, field, value))
    return failures


def check_text(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError as e:
        return ["%s: could not read file (%s)" % (path, e)]

    failures = []
    for lineno, line in enumerate(text.splitlines(), start=1):
        if TIMING_OUTSIDE_RE.search(line):
            failures.append("%s:%d: %s" % (path, lineno, line.strip()))
        elif TIMING_LONGER_RE.search(line):
            failures.append("%s:%d: %s" % (path, lineno, line.strip()))
    if not any(p in text for p in SUCCESS_PATTERNS):
        failures.append(
            "%s: missing a required success line (looked for any of %r) -- "
            "report does not confirm a clean ASCOM validation pass" % (path, SUCCESS_PATTERNS)
        )
    return failures


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        print("usage: check_conformu_reports.py <base-ref> | --self-test", file=sys.stderr)
        return 2
    base_ref = argv[0]

    files = changed_conformu_files(base_ref)
    if not files:
        print("No ConformU report files changed relative to %s -- nothing to check." % base_ref)
        return 0

    failures = []
    for path in files:
        if path.endswith(".json"):
            failures.extend(check_json(path))
        else:
            failures.extend(check_text(path))

    if failures:
        print("ConformU report validation failed:\n")
        for f in failures:
            print("  " + f)
        print(
            "\n%d finding(s). The driver is not validated. Fix the driver, "
            "re-run ConformU until clean, and replace the report on this "
            "branch. A failing ConformU report must never merge -- see "
            "AGENTS.md and /driver-build Step 10." % len(failures)
        )
        return 1

    print("ConformU report validation OK -- %d file(s) checked: %s"
          % (len(files), ", ".join(files)))
    return 0


def self_test():
    """Regression guard for this script's own rules, run with --self-test.

    The gate's failure mode is silence: main() returns 0 with "nothing to
    check" whenever changed_conformu_files() comes back empty, which is the
    right answer for a PR that touches no report and indistinguishable from
    CONFORMU_PREFIXES, the rename handling or the base-ref resolution having
    broken (issue #449). So this drives three layers:

      * check_json() / check_text() over synthetic reports, one case per
        rule, each asserted to fire on a bad input and stay quiet on a good
        one. Deleting a rule fails here rather than quietly widening the gate.
      * changed_conformu_files() against a REAL temporary git repository,
        because the prefix filter, the R100 rule and the merge-base are all
        decided by what git prints, and a fake would only test the fake.
      * main() end to end over that repository, so the wiring between the
        two is pinned as well (a report that fails check_json() must fail
        main()).
    """
    import hashlib
    import os
    import subprocess as sp
    import tempfile

    checks = []

    def check(name, condition):
        checks.append((name, condition))

    with tempfile.TemporaryDirectory() as tmp:
        # --- check_json(): one case per rule ---------------------------------
        def json_file(name, content):
            path = os.path.join(tmp, name)
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(content)
            return path

        clean = json_file("clean.json",
                          '{"ErrorCount": 0, "IssueCount": 0, "TimingIssuesCount": 0}')
        check("check_json passes a clean report", check_json(clean) == [])
        # Spelled as a list, not the checker's own tuple literal, so a
        # search-and-replace that trims the checker's field loop cannot
        # trim this one to match.
        for field in ["ErrorCount", "IssueCount", "TimingIssuesCount"]:
            bad = json_file("bad.json",
                            '{"ErrorCount": 0, "IssueCount": 0, "TimingIssuesCount": 0, "%s": 2}'
                            % field)
            found = check_json(bad)
            check("check_json FAILS on %s=2" % field,
                  len(found) == 1 and "%s=2 (must be 0)" % field in found[0])
            missing = json_file("missing.json",
                                '{"ErrorCount": 0, "IssueCount": 0, "TimingIssuesCount": 0}'
                                .replace('"%s": 0' % field, '"Other": 0'))
            found = check_json(missing)
            check("check_json FAILS when %s is missing" % field,
                  len(found) == 1 and "missing required field %r" % field in found[0])
        # Truthiness would pass `false`/`null` and fail "0"; all three must
        # be rejected as non-numbers.
        for literal in ("false", "null", '"0"'):
            bad = json_file("type.json",
                            '{"ErrorCount": %s, "IssueCount": 0, "TimingIssuesCount": 0}' % literal)
            found = check_json(bad)
            check("check_json FAILS on ErrorCount=%s (not a number)" % literal,
                  len(found) == 1 and "is not a number" in found[0])
        found = check_json(json_file("list.json", "[]"))
        check("check_json FAILS on a top-level array",
              len(found) == 1 and "not an object" in found[0])
        found = check_json(json_file("broken.json", "{not json"))
        check("check_json FAILS on unparsable JSON",
              len(found) == 1 and "could not parse as JSON" in found[0])
        found = check_json(os.path.join(tmp, "absent.json"))
        check("check_json FAILS on a missing file",
              len(found) == 1 and "could not parse as JSON" in found[0])

        # --- check_text(): one case per rule ---------------------------------
        def text_file(name, content, mode="w", encoding="utf-8"):
            path = os.path.join(tmp, name)
            with open(path, mode, encoding=encoding) as fh:
                fh.write(content)
            return path

        for pattern in SUCCESS_PATTERNS:
            path = text_file("ok.txt", "Conform started\n%s\nConform finished\n" % pattern)
            check("check_text passes with success line %r" % pattern[:24],
                  check_text(path) == [])
        found = check_text(text_file("nosuccess.txt", "Conform started\nConform finished\n"))
        check("check_text FAILS without any success line",
              len(found) == 1 and "missing a required success line" in found[0])
        for tier in ("FAST", "STANDARD", "EXTENDED"):
            line = "Connected  OUTSIDE %s RESPONSE TIME TARGET 1.2s" % tier
            path = text_file("timing.txt",
                             "%s\n%s\n" % (SUCCESS_PATTERNS[0], line))
            found = check_text(path)
            check("check_text FAILS on an OUTSIDE %s RESPONSE TIME TARGET line" % tier,
                  found == ["%s:2: %s" % (path, line)])
        line = "The Connected property took longer than its target response time"
        path = text_file("longer.txt", "%s\n%s\n" % (line, SUCCESS_PATTERNS[1]))
        check("check_text FAILS on a 'took longer than its target' line",
              check_text(path) == ["%s:1: %s" % (path, line)])
        # A near-miss must NOT fire: the tier word is part of the pattern.
        path = text_file("near.txt",
                         "%s\nOUTSIDE SLOW RESPONSE TIME TARGET\n" % SUCCESS_PATTERNS[0])
        check("check_text ignores an OUTSIDE line with an unknown tier",
              check_text(path) == [])
        # Reports are opened with errors="replace": a stray non-UTF-8 byte
        # must not turn a timing failure into an unreadable-file failure.
        path = text_file("latin1.txt",
                         b"caf\xe9\nOUTSIDE FAST RESPONSE TIME TARGET\n"
                         + SUCCESS_PATTERNS[0].encode("utf-8") + b"\n",
                         mode="wb", encoding=None)
        found = check_text(path)
        check("check_text still applies its rules to a non-UTF-8 report",
              len(found) == 1 and found[0].startswith("%s:2: OUTSIDE FAST" % path))
        found = check_text(os.path.join(tmp, "absent.txt"))
        check("check_text FAILS on a missing file",
              len(found) == 1 and "could not read file" in found[0])

        # --- changed_conformu_files() + main() over a real git repository ----
        repo = os.path.join(tmp, "repo")
        os.mkdir(repo)
        env = dict(os.environ,
                   GIT_AUTHOR_NAME="self-test", GIT_AUTHOR_EMAIL="self-test@example.invalid",
                   GIT_COMMITTER_NAME="self-test", GIT_COMMITTER_EMAIL="self-test@example.invalid",
                   GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_SYSTEM="/dev/null")

        def git(*args):
            if args[0] == "mv":
                # git mv does not create the destination directory.
                os.makedirs(os.path.dirname(os.path.join(repo, args[-1])), exist_ok=True)
            return sp.run(["git", "-C", repo] + list(args), check=True,
                          capture_output=True, text=True, env=env).stdout

        def write(rel, content):
            path = os.path.join(repo, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(content)

        clean_json = '{"ErrorCount": 0, "IssueCount": 0, "TimingIssuesCount": 0}\n'

        def clean_txt_for(rel):
            # Each report gets a distinct BODY, not just a distinct path line.
            # With identical files, git's rename detection pairs the deleted
            # report with whichever added one it likes, and the R100 rule
            # below then skips the wrong path; with bodies that differ only in
            # one line the pairing still depends on git's similarity scoring
            # (Removed vs Cafe scored ~93%). Eight lines keyed on the path's
            # hash keep every pair well under the 50% rename threshold.
            digest = hashlib.sha1(rel.encode("utf-8")).hexdigest()
            body = "".join("%s %d %s\n" % (digest, i, rel) for i in range(8))
            return "%s\nreport for %s\n%s" % (SUCCESS_PATTERNS[0], rel, body)

        def write_report(rel, suffix=""):
            write(rel, clean_txt_for(rel) + suffix)
        git("init", "-q", "-b", "base")
        write_report("AlpacaCore/conformu/Vendor/Model/Linux-arm64.txt")
        write_report("AlpacaCore/conformu/Vendor/Renamed/Linux-arm64.txt")
        write_report("AlpacaCore/conformu/Vendor/Edited/Linux-arm64.txt")
        write_report("AlpacaCore/conformu/Vendor/Removed/Linux-arm64.txt")
        write_report("docs/moved-in.txt")
        write("README.md", "base\n")
        git("add", "-A")
        git("commit", "-q", "-m", "base")
        git("checkout", "-q", "-b", "topic")
        # The base branch moves on AFTER the topic branched: a report added
        # there must NOT be attributed to the topic, which is what the
        # merge-base (rather than a plain diff against the base tip) buys.
        git("checkout", "-q", "base")
        write_report("AlpacaCore/conformu/Vendor/OnBase/Linux-arm64.txt")
        git("add", "-A")
        git("commit", "-q", "-m", "base moves on")
        git("checkout", "-q", "topic")

        real_cwd = os.getcwd()
        os.chdir(repo)
        # Git detects renames by default (diff.renames=true since 2.9), which
        # would keep the R100 cases green even with the explicit
        # --find-renames dropped from the diff command. Turn the default off
        # for the child git so the flag is what the rename cases exercise.
        saved_params = os.environ.get("GIT_CONFIG_PARAMETERS")
        os.environ["GIT_CONFIG_PARAMETERS"] = "'diff.renames=false'"
        try:
            check("changed_conformu_files is empty when nothing changed",
                  changed_conformu_files("base") == [])
            check("main() passes with nothing to check", main(["base"]) == 0)

            write_report("AlpacaCore/conformu/Vendor/Model/Linux-arm64.txt", "modified\n")
            write("AlpacaCore/conformu/Vendor/Added/Linux-arm64.json", clean_json)
            write("AlpacaHTTP/conformu/protocol.txt",
                  "%s\n" % SUCCESS_PATTERNS[1])
            write("AlpacaCore/conformu/Vendor/README.md", "not a report\n")
            write("docs/conformu-notes.txt", "outside the prefixes\n")
            write_report("AlpacaCore/conformu/Vendor/Café/Linux-arm64.txt")
            git("mv", "AlpacaCore/conformu/Vendor/Renamed/Linux-arm64.txt",
                "AlpacaCore/conformu/Vendor/RenamedTo/Linux-arm64.txt")
            git("mv", "AlpacaCore/conformu/Vendor/Edited/Linux-arm64.txt",
                "AlpacaCore/conformu/Vendor/EditedTo/Linux-arm64.txt")
            write("AlpacaCore/conformu/Vendor/EditedTo/Linux-arm64.txt",
                  clean_txt_for("AlpacaCore/conformu/Vendor/Edited/Linux-arm64.txt") + "edited\n")
            git("mv", "docs/moved-in.txt", "AlpacaCore/conformu/Vendor/MovedIn/Linux-arm64.txt")
            git("rm", "-q", "AlpacaCore/conformu/Vendor/Removed/Linux-arm64.txt")
            git("add", "-A")
            git("commit", "-q", "-m", "topic")

            files = changed_conformu_files("base")
            expected = [
                "AlpacaCore/conformu/Vendor/Added/Linux-arm64.json",
                "AlpacaCore/conformu/Vendor/Café/Linux-arm64.txt",
                "AlpacaCore/conformu/Vendor/EditedTo/Linux-arm64.txt",
                "AlpacaCore/conformu/Vendor/Model/Linux-arm64.txt",
                "AlpacaCore/conformu/Vendor/MovedIn/Linux-arm64.txt",
                "AlpacaHTTP/conformu/protocol.txt",
            ]
            check("changed_conformu_files reports added, modified, edited-rename, "
                  "moved-in and non-ASCII reports under both prefixes",
                  sorted(files) == expected)
            check("changed_conformu_files skips a pure rename inside the prefixes",
                  not any("RenamedTo" in f for f in files))
            check("changed_conformu_files skips a deleted report",
                  not any("Removed" in f for f in files))
            check("changed_conformu_files skips a non-report extension under the prefix",
                  not any(f.endswith("README.md") for f in files))
            check("changed_conformu_files skips a .txt outside the prefixes",
                  not any(f.startswith("docs/") for f in files))
            check("changed_conformu_files diffs from the merge-base, not the base tip",
                  not any("OnBase" in f for f in files))

            # The wiring: every report above is clean, so main() passes; one
            # bad report of each kind then fails it.
            check("main() passes when every changed report is clean",
                  main(["base"]) == 0)
            write("AlpacaCore/conformu/Vendor/Added/Linux-arm64.json",
                  '{"ErrorCount": 1, "IssueCount": 0, "TimingIssuesCount": 0}\n')
            git("commit", "-q", "-am", "bad json")
            check("main() FAILS when a changed JSON report has ErrorCount=1",
                  main(["base"]) == 1)
            write("AlpacaCore/conformu/Vendor/Added/Linux-arm64.json", clean_json)
            write("AlpacaHTTP/conformu/protocol.txt", "no success line here\n")
            git("commit", "-q", "-am", "bad text")
            check("main() FAILS when a changed text report lacks the success line",
                  main(["base"]) == 1)
            import contextlib
            import io
            with contextlib.redirect_stderr(io.StringIO()):
                check("main() rejects a missing base-ref argument", main([]) == 2)
        finally:
            os.chdir(real_cwd)
            if saved_params is None:
                del os.environ["GIT_CONFIG_PARAMETERS"]
            else:
                os.environ["GIT_CONFIG_PARAMETERS"] = saved_params

    failed = [name for name, ok in checks if not ok]
    for name, ok in checks:
        print("[%s] %s" % ("PASS" if ok else "FAIL", name))
    if failed:
        print("\n%d/%d self-test(s) failed." % (len(failed), len(checks)))
        return 1
    print("\nAll %d self-test(s) passed." % len(checks))
    return 0


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(self_test())
    sys.exit(main())
