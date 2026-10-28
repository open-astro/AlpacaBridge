#!/usr/bin/env python3
"""Fail when the repo's own docs/config disagree with each other or with reality.

None of these facts are cross-checked anywhere else, so each has drifted
silently in the past (see AGENTS.md's own admissions and the 2026-09
harness-readiness evaluation that prompted this script). Every check below
is read-only and file-local -- no network, no build.

Run from the repo root:  python3 scripts/check_docs_drift.py
Self-test (no repo state): python3 scripts/check_docs_drift.py --self-test
  -- drives check 8's pairing and job-scoping helpers over literal fixtures,
  one per mutation the check exists to catch, so an extractor that stops
  matching fails here instead of degrading the gate to its floor (#455).
Counts (check 15's numbers): python3 scripts/check_docs_drift.py --counts
  -- prints the validated-device count, the brand count and its spelled-out
  word, the brand list and a paste-ready README headline, all computed by
  the same functions check 15 gates with, so the /bump-release recount step
  has one command and no second copy of the row filter (#689).
Badge (check 25's number): python3 scripts/check_docs_drift.py --badge
  -- prints the authored-line count, the rounded figure and the paste-ready
  README badge line, computed by the functions check 25 gates with, so the
  /bump-release badge step has one command (#1465).

Checks:
  1. Every ALPACACORE_ENABLE_* CMake option is documented in the
     docs/development.md build-options table.
  2. The zizmor pinned version + sha256 are identical between ci.yml and
     ci_preflight.sh.
  3. The cppcheck --suppress list is identical between ci.yml and
     ci_preflight.sh (ci-gates instructions, Docs drift bullet: "keep the
     cppcheck --suppress lists identical").
  4. VERSION matches the version in the README's changelog badge line.
  5. The drivers whose `get_connected()` blocks are exactly the ones
     `async_connectable.h` names as blocking. Measured from the code, not
     restated: the router rule ("never call get_connected() while
     get_connecting() is true") depends on knowing which drivers have that
     shape, and the list has drifted repeatedly in both directions
     (issues #315, #355, #381, #407). The same check also fails on a COUNT
     of either list ("the five telescopes", "four wrapper-backed switches")
     stated anywhere the globs below reach: a number is a second source of
     truth for a list that is already gated by name, and it is what went
     stale before. CHANGELOG.md is exempt -- its entries describe what was
     true when they were written.
  6. The QHYSDK seam's three parallel lists agree: every pure virtual on the
     interface has a LockedQHYSDK override, every override actually takes the
     shared mutex through the one-argument locked() -- except two: cancel_exposure(),
     which must take its own cancel_mutex_ and must NOT go through locked() (issue
     #339: production's cancel skips the per-handle mutex so it can interrupt a
     download blocked on the same handle), and get_sdk_version(), which must use
     locked(version_mutex_, ...) and not the shared mutex (issue #1646: the hook
     must not queue behind an in-flight open). A forward written as
     locked(other_mutex_, ...) fails -- and the forward sweep in
     test_qhy_fake_sdk.cpp drives all of them.
  7. Every relative path referenced in AGENTS.md, CONTEXT.md, README.md (issue
     #693: the most-read file in the repo was in no scan), scoped instructions,
     docs/agents/ agent-skills config, .claude/skills/ Claude skills, and
     docs/failures/ and docs/decisions/
     inline code spans (`` `AlpacaCore/...` ``, `` `scripts/...` ``,
     `` `docs/...` ``, etc.) that looks like a real repo path actually exists.
     A `<vendor>`-style placeholder span under AlpacaCore/src/vendors/ or
     AlpacaCore/src/catalog/ must match at least one tracked file.
     First-party code-comment references to a failure or decision record must
     resolve too.
  8. The TSan job's filtered runs are identical between ci.yml and
     ci_preflight.sh: the same ordered `alpacacore_tests "<tag>"` invocations
     out of the same build directory, each one followed by a zero-test
     `grep -qE` that reads the log that run's `tee` wrote, with the same
     pattern in both files (issue #341, tightened in issue #455: a guard is
     paired with the run it reads, not counted). The pre-flight script only
     has value while it runs what CI runs, and this pair is written out twice
     with nothing comparing it -- the same shape as checks 2 and 3.
  9. Every first-party source file carries the CURRENT AGPL-3.0-or-later
     header: the "This file is part of <Component>." line plus the short
     form that names the licence id and points at the LICENSE file for the
     vendor-SDK linking exception (issue #450). The rule was prompt-only,
     and two files added on 2026-09-09 carried the pre-#113 long-form GNU
     boilerplate, which names neither. Vendored SDKs under external/ are
     excluded, as everywhere else.
 10. Every backticked repo path in the Cursor rule files
     (AlpacaCore/.cursor/rules/*.mdc, AlpacaHTTP/.cursor/rules/*.mdc) and in
     AlpacaCore/external/README.md exists, the same way check 7 does it for
     AGENTS.md (issue #457). Those files are `alwaysApply: true`, so an agent
     reads them before touching a driver, and #453 found a versioned QHY SDK
     path and a directory that never existed in one of them. Paths in these
     files are relative to the component the file lives under
     (`external/QHY/...` in an AlpacaCore rule means `AlpacaCore/external/QHY/...`),
     so each span is resolved against its component root as well as the
     repo root. Fenced blocks, including the illustrative directory trees,
     are skipped exactly as in check 7; a stale entry inside a tree block
     stays unchecked, and that is an accepted limit of this check, not an
     oversight. Each file carries its own floor.
 11. The SHA-256 of the LF-normalized docs/AlpacaDeviceAPI_v1.yaml matches
     the one pinned in the ascom-alpaca-protocol skill's
     references/version-and-sources.md. The skill's endpoint catalog was
     built from that snapshot; /driver-build Step 0 refreshes the schema
     from ascom-standards.org, and without this pin the catalog would keep
     describing the old one.
 12. Every model in SUPPORTED-DRIVERS.md's GPhoto table is named in the STATUS
     paragraph of .github/instructions/gphoto.instructions.md, the only file a
     scoped agent reads for that vendor, which restated the validated set by
     hand and fell behind when the Canon EOS 4000D row was added (PR #626).
     By name, one-directional, and only for rows whose Connection cell starts
     with USB and whose status cell is a check mark.
 13. The fake-connectable roster in AlpacaCore/tests/contract_sweep.h agrees
     with the fake_*.h files on disk in both directions (issue #571): a fake
     that is neither in the roster nor in HELPER_FAKES fails, and a roster row
     whose fake no longer exists fails. The roster is the list the tier-2
     (connected-over-a-fake) contract cases will iterate; those cases are a
     follow-up PR to #571, so today this check pins the list, not any case. HELPER_FAKES names the
     fakes that are not a driver's connect path, each with a reason, and a
     helper that has since been given a roster row is itself a finding.
 14. Every std::regex in AlpacaHTTP/src/http/router.cpp is a static object
     (issue #657, from #646). Router::route once built a std::regex per
     request, which made a device-path request about 14x a management request
     (30x under ASan). This replaces a wall-clock ratio test, which was coarse
     in Release and flaky under load. A construction with no `static` in its
     statement, or an unnamed temporary, is a finding, and so is finding no
     construction at all (the extractor is stale or the regexes moved).
 15. The README headline ("N validated devices. <Word> brands. One server."
     plus the brand list) agrees with SUPPORTED-DRIVERS.md (issue #684). N is
     recomputed with the row filter this script owns, count_validated_device_rows
     (table rows that are not a separator, a header or a `| Source` row,
     counted when they carry a check mark; /bump-release Step 2.4 reads it
     through --counts instead of restating it, issue #689); the spelled-out
     brand word must equal the number
     of items in the README list; and every `### ` vendor heading in
     SUPPORTED-DRIVERS.md must map, through the explicit table
     SUPPORTED_HEADING_TO_README_BRAND, onto an item in that list, with
     README_BRANDS_WITHOUT_HEADING declaring the items that have no heading
     (the Unihedron SQM-LE is a sensor the WeeWX driver reads through the
     feed's sqm fields; it has no row or heading of its own). Only check 4 gated
     the README, and it compares the version badge alone, so the headline
     was three releases stale at 4.0.0 and moved by hand again at 4.1.0.
     Parse failures name the real cause (issue #690): a declared brand item
     that contains a comma is consumed whole before the list is split, and a
     multi-word brand count ("Twenty One") reaches the number-word message
     instead of failing the headline regex as "headline not found".
 16. The hand-maintained `## Updated YYYY-MM-DD` line in SUPPORTED-DRIVERS.md
     is a real date no older than the README badge's release date (issue
     #692). It read 2026-09-24 on the 2026-09-27 release. Every release
     re-verifies the file (Step 2.4 recounts from it), so /bump-release sets
     the line to the release date and this gate holds it there; between
     releases it may run ahead (/conformu and /commit bump it) but never
     behind, and never more than MAX_UPDATED_DAYS_AHEAD past the badge, which
     catches a typo'd year without consulting the clock. Pure, so it runs in
     pre-flight; no git state is consulted.
 17. Every AlpacaError value AGENTS.md, the ascom-contract and testing
     instructions and .claude/commands/driver-build.md write down matches the enum in AlpacaCore/include/alpacacore/alpaca_errors.h
     (issue #682): the name must be a constant in `namespace AlpacaError` and
     the hex value must be the one the header defines. Four written forms are
     read: `Name` (0xNNN), Name `0xNNN`, `A` (or `B` ..., both 0xNNN), and an
     exception-table row `| `Name` | ... (0xNNN ... |`, whose values belong to
     the row's name. CamelCase names only, so a protocol constant such as
     MC_AUX_GUIDE 0x26 is not read as a pin; a floor catches an extractor that
     stops matching.
 18. The first bullet under the "Continuous Integration and Pre-flight" heading
     of .github/instructions/ci-gates.instructions.md (`- CI (...`) names exactly the job ids under `jobs:` in
     .github/workflows/ci.yml, in both directions and by id, not by count
     (issue #702): it listed 11 of 18 jobs and called `format` `clang-format`.
     The roster is every backticked token in the bullet outside parentheses,
     so each id's description goes in parentheses after it.
 19. No vendored shared library under AlpacaCore/external/ is committed as two
     regular files with the same content in one directory (`libfoo.so` next to
     `libfoo.so.1.2`): the unversioned name has to be a symlink. `cp -a` in
     debian/rules copies a regular file as a second full copy, so the ZWO
     camera SDK shipped twice in every .deb (3.4 MB) from March 2026 until this
     check. Read from the git index, so a staged fix is seen. The empty blob is
     skipped: a 0-byte placeholder is not a library copy.
 20. No `restore-keys:` prefix in a workflow under .github/workflows/ is a
     strict prefix of another cache key's prefix (issue #1355). A restore by
     prefix takes the newest cache whose key starts with it, so the ASan job's
     `ccache-sanitizers-` restored the TSan job's `ccache-sanitizers-tsan-`
     cache on every run and missed every compile. A prefix is the key's text
     before its first `${{`; restoring another job's whole prefix on purpose
     (clang-tidy reads `ccache-all-vendors-coverage-`) is an equal prefix and
     is allowed.
 21. AGENTS.md stays at or under AGENTS_MD_MAX_BYTES (15,000 bytes). It is
     loaded whole into every agent session; it grew from 100k to 144k in five
     weeks after the #523 split because every cross-vendor lesson was appended
     to it. Shared rules go in the topic files under .github/instructions/ and
     AGENTS.md carries a one-line pointer.
 22. Every decision record in docs/decisions/ has the shape README.md there
     describes: an NNNN-<slug>.md name with no gap in the numbers, a
     `Status:` line on line 3 (a superseded record names a record that
     exists; a retired one names its issue or PR), the sections Context,
     Decision, Rules, Alternatives rejected, Consequences and Links in that
     order (Adoption and How to reverse may sit between them; a superseded
     record needs no Rules and a retired one drops Rules and Adoption; How to
     reverse is required from record 0020 on and in any record that supersedes
     another), numbered Rules lines that end
     `Scope: `glob`. Checked by: <check>.` with every glob matching a tracked
     file (`**` only in 0013), no except/unless/until/for now on a rule line
     without the `<!-- allowed -->` marker, bare `Checked by: review` rules
     held to the per-record ratchet DECISION_BARE_REVIEW_BASELINE, and an
     index that lists every record once, in order, with `(retired)` on exactly
     the retired ones. The decision-record gate reads these sections, so a
     record it cannot parse would pass every pull request.
 23. Every `## Adoption` table agrees with the code, both ways: every file
     the `Scan:` glob matches that names the `Symbol:` (with no `Symbol:`
     line, every scanned file) has a row; a Symbol column overrides the symbol
     per row and a blank cell falls back to the `Symbol:` line; comments are blanked before matching; an adopted or partial row's
     source names its symbol; a planned row's source does not; a partial row names
     the issue that finishes it. The rosters were prose in five places and
     three disagreed with the code (PR #1410). Check 7 also resolves a
     comment that cites a record by number only (`docs/decisions/0008`).
 24. The review job's time budget agrees everywhere it is restated: the
     `timeout-minutes` of the `review` job and the `N * 60` wait deadline in
     claude-review.yml against the ci-gates instructions (wait, job timeout,
     poll) and both skills (the "up to N minutes" wait, the `BUDGET:-<seconds>`
     defaults and the "N-minute budget" prose); the two skills agree with each
     other, and the poll budget is greater than the job timeout (a poll that
     ends first reports a healthy run as a stall).
 25. The README "Authored code" badge (`NNNk lines`) is within
     AUTHORED_LINES_TOLERANCE (5%) of the authored-line count computed from
     the git index (issue #1465). Which tracked files count, and why the
     rest do not, is written at AUTHORED_LINES_ROOTS and
     AUTHORED_LINES_EXCLUDED_PREFIXES. The badge shows the count rounded to the
     nearest 1,000, so an ordinary PR that adds or removes lines never trips the
     check; only a badge that has fallen behind (or a missing badge) does.
     /bump-release regenerates it with --badge.
"""

import glob
import hashlib
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import changelog_fragments  # noqa: E402  (VERSION_RE, the one spelling of X.Y.Z[~betaN])

ROOT = Path(__file__).resolve().parent.parent


def read(path, root=ROOT):
    return (root / path).read_text(encoding="utf-8", errors="replace")


# --- check 1: CMake options vs docs/development.md --------------------------

CMAKE_OPTION_RE = re.compile(r"option\((ALPACACORE_ENABLE_\w+)\b")
DOCS_OPTION_RE = re.compile(r"`(ALPACACORE_ENABLE_\w+)`")


def check_cmake_options_documented():
    failures = []
    cmake_text = read("AlpacaCore/CMakeLists.txt")
    cmake_options = set(CMAKE_OPTION_RE.findall(cmake_text))

    docs_text = read("docs/development.md")
    docs_options = set(DOCS_OPTION_RE.findall(docs_text))

    missing = sorted(cmake_options - docs_options)
    for opt in missing:
        failures.append(
            "docs/development.md build-options table is missing %s "
            "(defined in AlpacaCore/CMakeLists.txt)" % opt
        )
    return failures


# --- check 2: zizmor pin sync ------------------------------------------------

def check_zizmor_pin_sync():
    failures = []
    ci = read(".github/workflows/ci.yml")
    preflight = read("scripts/ci_preflight.sh")

    # ci.yml has more than one `ver=`/`sha256=` shell pin (cppcheck's
    # from-source build has its own); scope to the "Install zizmor" step so
    # this doesn't accidentally read cppcheck's pin instead.
    ci_block = _scoped_block(ci, "- name: Install zizmor", ("- name:",))
    if ci_block is None:
        failures.append("could not find the 'Install zizmor' step in ci.yml")
        return failures

    ci_ver = re.search(r"^\s*ver=([0-9.]+)\s*$", ci_block, re.MULTILINE)
    ci_sha = re.search(r"^\s*sha256=([0-9a-f]{64})\s*$", ci_block, re.MULTILINE)
    pf_ver = re.search(r'^ZIZMOR_VER="([0-9.]+)"', preflight, re.MULTILINE)
    pf_sha = re.search(r'^ZIZMOR_SHA256="([0-9a-f]{64})"', preflight, re.MULTILINE)

    if not (ci_ver and ci_sha and pf_ver and pf_sha):
        failures.append(
            "could not find zizmor ver/sha256 pins in both ci.yml (zizmor "
            "job) and ci_preflight.sh (ZIZMOR_VER/ZIZMOR_SHA256) -- update "
            "this check's regexes if the format changed"
        )
        return failures

    if ci_ver.group(1) != pf_ver.group(1):
        failures.append(
            "zizmor version mismatch: ci.yml has %s, ci_preflight.sh has %s"
            % (ci_ver.group(1), pf_ver.group(1))
        )
    if ci_sha.group(1) != pf_sha.group(1):
        failures.append(
            "zizmor sha256 mismatch: ci.yml has %s, ci_preflight.sh has %s"
            % (ci_sha.group(1), pf_sha.group(1))
        )
    return failures


# --- check 3: cppcheck --suppress list sync ---------------------------------

SUPPRESS_RE = re.compile(r"--suppress=(\S+)")


_CI_JOB_RE = re.compile(r"^  [A-Za-z0-9_-]+:[ \t]*$", re.MULTILINE)


def _ci_job_block(ci_text, job_name):
    r"""The text of one top-level job in ci.yml, from its `  <job>:` line to the
    next job's line (or EOF).

    Checks 3 and 8 used to end their scope at a NAMED neighbour (`\n  zizmor:`,
    `\n  format:`), which silently widened the scope whenever a job was
    inserted between the two -- and a widened scope carrying a `grep -qE` or a
    `--suppress=` of its own would then fire the check with a message pointing
    at the wrong gate (issue #455). Ending at "the next job at this indent"
    makes the scope follow the job, not the file's current ordering.
    """
    start = ci_text.find("\n  %s:" % job_name)
    if start == -1:
        return None
    start += 1
    # Search from the end of the job's own line, not from an arithmetic
    # offset that hard-codes the indent and the colon.
    line_end = ci_text.find("\n", start)
    if line_end == -1:
        return ci_text[start:]
    m = _CI_JOB_RE.search(ci_text, line_end)
    return ci_text[start:m.start()] if m else ci_text[start:]


def _scoped_block(text, start_marker, end_markers):
    """text from start_marker to the first of end_markers found after it (or EOF).

    Used to scope a regex scan to one CI step/gate instead of the whole file,
    the same way check_zizmor_pin_sync does -- a file-wide scan is only safe
    while the flag being matched (--suppress=, ver=, ...) appears nowhere
    else in the file, which is an assumption worth pinning down rather than
    leaving implicit.
    """
    start = text.find(start_marker)
    if start == -1:
        return None
    end = len(text)
    for marker in end_markers:
        pos = text.find(marker, start + len(start_marker))
        if pos != -1:
            end = min(end, pos)
    return text[start:end]


def check_cppcheck_suppress_sync():
    failures = []
    ci_full = read(".github/workflows/ci.yml")
    preflight_full = read("scripts/ci_preflight.sh")

    # Both files currently have exactly one --suppress= invocation, so a
    # whole-file scan happens to be equivalent to a scoped one today -- but
    # scope explicitly anyway (mirroring check_zizmor_pin_sync) so this stays
    # correct if a second tool with its own --suppress flag is ever added to
    # either file.
    ci = _scoped_block(_ci_job_block(ci_full, "cppcheck") or "", "- name: Analyze changed C/C++ files", ())
    preflight = _scoped_block(preflight_full, 'section "cppcheck (changed files)"', ('section "',))
    if ci is None or preflight is None:
        failures.append(
            "could not find the cppcheck step in ci.yml or the cppcheck "
            "gate in ci_preflight.sh -- update this check's markers if "
            "either file's structure changed"
        )
        return failures

    ci_suppress = SUPPRESS_RE.findall(ci)
    pf_suppress = SUPPRESS_RE.findall(preflight)

    if not ci_suppress or not pf_suppress:
        failures.append(
            "could not find --suppress= flags in both ci.yml and "
            "ci_preflight.sh -- update this check's regex if the cppcheck "
            "invocation changed"
        )
        return failures

    if set(ci_suppress) != set(pf_suppress):
        failures.append(
            "cppcheck --suppress list differs: ci.yml has %s, "
            "ci_preflight.sh has %s"
            % (sorted(set(ci_suppress)), sorted(set(pf_suppress)))
        )
    return failures


# --- check 4: VERSION vs README badge ---------------------------------------

# A beta VERSION spells its Debian pre-release suffix `~betaN` (5.0.0~beta1);
# the README badge carries the same spelling. The form is owned by
# scripts/changelog_fragments.py (VERSION_RE; release_tag.py's TAG_RE is its
# tag-side twin) and only borrowed here, so there is one Python spelling.
VERSION_FORM_RE = changelog_fragments.VERSION_RE
# A plain string with no capturing group, embedded below; both badge regexes
# read their fields by name, so a group added to it cannot shift them.
VERSION_PATTERN = changelog_fragments.VERSION_PATTERN
README_BADGE_RE = re.compile(
    r"^####\s*\[(?P<version>" + VERSION_PATTERN + r")\]\s*-\s*[0-9-]+\s*&middot;\s*\[Changelog\]", re.MULTILINE)


def _version_badge_findings(version, readme):
    """Check 4 over the VERSION text and README text. Pure, so --self-test can drive it."""
    if not VERSION_FORM_RE.match(version.strip()):
        return ["VERSION (%s) is not X.Y.Z or X.Y.Z~betaN (betas count from 1)" % version.strip()]
    m = README_BADGE_RE.search(readme)
    if not m:
        return ["could not find the version badge line in README.md"]
    if m.group("version") != version.strip():
        return [
            "VERSION (%s) does not match the README badge version (%s)"
            % (version.strip(), m.group("version"))
        ]
    return []


def check_version_matches_readme():
    return _version_badge_findings(read("VERSION"), read("README.md"))


# --- check 5: the blocking-get_connected() list vs the code -----------------
#
# issue #381. Four comments and prose passages used to hand-maintain COUNTS of
# this split; nothing checked them and they drifted repeatedly, including a
# stale count introduced by the PR that was correcting the others. The counts
# are gone (the rule is stated instead), but the NAMED lists remain
# load-bearing: `async_connectable.h`'s connection-task tail must read
# get_connected() before taking pending_mutex_ precisely because these drivers
# nest the driver mutex outside it, and the router must never read
# get_connected() mid-task because these drivers block on it. A reader who
# trusts a stale list is misled in the direction that matters -- believing a
# driver is safe to read mid-task when it blocks.
#
# So the code is the source of truth here and the comment must follow it: every
# get_connected() override under AlpacaCore/src/vendors is classified by its
# body, and the two blocking classes must be named in that header. A driver
# whose file is not in DRIVER_PROSE_NAMES fails loudly rather than being
# bucketed silently -- add it there AND to the header's list in the same
# change.

GET_CONNECTED_RE = re.compile(r"bool\s+get_connected\s*\(\s*\)\s*const\s+override\s*\{")

# Driver file basename -> the name the async_connectable.h comment uses for it.
# Only the drivers that CAN be classified as blocking need an entry; the
# lock-free majority is not named anywhere, by design. Two groups are the
# exception, and both keep their entries although the header's list is empty:
# the Bisque and iOptron telescopes (lock-free since #910) and the four
# wrapper-backed switch drivers (iOptron iMate PowerBox, ToupTek StellaVita,
# ASIAIR, ASIAIR Plus; lock-free since #382). The entry is what lets the gate
# report MISSING FROM THE BLOCKING LIST if one of them re-locks get_connected()
# (or a wrapper re-locks is_open()). Do not remove them on the STALE
# BLOCKING-LIST ENTRY advice.
# The value is (prose name, which list it belongs to). The list matters: the
# header names these in TWO sentences, and one name is a prefix of another
# across them -- "iOptron" (telescope) inside "iOptron iMate PowerBox"
# (switch). A plain `name in header` test therefore answered wrongly in both
# directions: dropping iOptron from the telescope list still "found" it via the
# switch entry (the exact drift this check exists to catch, passing green), and
# a correctly-removed iOptron reported a STALE entry that could not be resolved
# without editing an unrelated sentence. Each name is now looked for only in
# its own list.
DRIVER_PROSE_NAMES = {
    "bisque_telescope_driver.cpp": ("Bisque", "telescopes"),
    "ioptron_telescope_driver.cpp": ("iOptron", "telescopes"),
    "onstep_telescope_driver.cpp": ("OnStep", "telescopes"),
    "ioptron_switch_driver.cpp": ("iOptron iMate PowerBox", "switches"),
    "touptek_switch_driver.cpp": ("ToupTek StellaVita", "switches"),
    "zwo_asiair_switch_driver.cpp": ("ZWO ASIAIR", "switches"),
    "zwo_asiair_plus_switch_driver.cpp": ("ASIAIR Plus", "switches"),
}

# Where each list lives in async_connectable.h, as (start marker, end marker).
# Both must be found or the check fails loudly: a header rewrite that moves
# them must not silently turn this gate into a no-op.
BLOCKING_LIST_SPANS = {
    "telescopes": ("The blocking telescopes (", ")"),
    "switches": ("The wrapper-backed switch drivers (", ")"),
}


def _blocking_list_spans(header):
    """{list name: text} for each blocking list, or ({}, [failure])."""
    # Strip `//` comment markers and collapse the wrapping so a name split
    # across two comment lines still matches.
    flat = " ".join(line.strip().lstrip("/").strip() for line in header.splitlines())
    flat = re.sub(r"\s+", " ", flat)
    spans = {}
    failures = []
    for name, (start, end) in BLOCKING_LIST_SPANS.items():
        i = flat.find(start)
        j = flat.find(end, i + len(start)) if i >= 0 else -1
        if i < 0 or j < 0:
            failures.append(
                "BLOCKING-LIST SPAN NOT FOUND: could not locate the '%s' list in "
                "async_connectable.h (looked for %r ... %r). The list was moved or reworded -- "
                "update BLOCKING_LIST_SPANS in %s, or this check silently stops checking."
                % (name, start, end, Path(__file__).name))
            continue
        spans[name] = flat[i:j + len(end)]
    return spans, failures


def _names_in_span(which, span):
    """The driver names a blocking-list sentence actually lists."""
    body = span.split("(", 1)[1].rsplit(")", 1)[0]
    # "A, B, C and D" -> [A, B, C, D]
    parts = []
    for chunk in body.split(","):
        parts.extend(re.split(r"\band\b", chunk))
    return [p.strip() for p in parts if p.strip()]

QHY_INTERFACE_HEADER = "AlpacaCore/include/alpacacore/vendor/qhy/qhy_sdk_wrapper.h"
QHY_LOCKED_HEADER = "AlpacaCore/tests/locked_qhy_sdk.h"
QHY_SWEEP_TEST = "AlpacaCore/tests/test_qhy_fake_sdk.cpp"

# The cv/exception qualifiers between the closing paren and `= 0` / `override`
# are optional but must be TOLERATED: without them a `const` method is invisible
# to both patterns, so a 27th pure virtual that happens to be const would be
# omitted from every set, all three differences would come out empty, and the
# gate would pass on exactly the drift it exists to catch. (A developer who did
# add the sweep entry got the opposite: a STALE SWEEP ENTRY naming the wrong
# cause.) Nothing on this seam is const today, so the hole was only reachable
# by the next method added -- which is the whole population this gate is for.
_QUALIFIERS = r"(?:\s*(?:const|noexcept|final|override))*"
PURE_VIRTUAL_RE = re.compile(
    r"\bvirtual\b[^;{}]*?(\w+)\s*\([^;{}]*\)" + _QUALIFIERS + r"\s*=\s*0\s*;", re.S)
OVERRIDE_RE = re.compile(
    r"(\w+)\s*\([^;{}]*\)(?:\s*(?:const|noexcept|final))*\s*override\s*\{", re.S)
BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT_RE = re.compile(r"//[^\n]*")


def _strip_comments(text):
    """Comments blanked (newlines kept). These headers carry long doc comments
    whose prose contains parentheses and identifiers, and both patterns above
    scan across whitespace -- without this a sentence in a comment is matched
    as a method signature. Written for the two QHY headers, which hold no raw
    string literals. Check 14 also runs it over router.cpp, which does (R"(...)"
    regexes) and holds "://" (origin.find), so LINE_COMMENT_RE blanks the rest of
    that line; that is harmless only while no std::regex sits on such a line or
    after a stray /* -- re-check both if the gate ever misses one."""
    text = BLOCK_COMMENT_RE.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    return LINE_COMMENT_RE.sub("", text)


def _matching_brace(text, open_index):
    """Index just past the `}` closing the `{` at open_index."""
    depth = 0
    for i in range(open_index, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i + 1
    return len(text)


IS_OPEN_BODY_RE = re.compile(r"\bis_open\s*\(\s*\)\s*const\s*\{")


def _vendor_is_open_locks(vendor_dir):
    """True if any is_open() body in the vendor's wrapper sources takes a lock.

    A pimpl forward (`return impl_->is_open();`) does not count; the Impl's
    own body does. No is_open() at all also counts as locking: an unknown
    shape must classify as blocking, never silently as lock-free.

    Same limitation as classify_get_connected_bodies(): only the three RAII
    lock types are recognised. A body that re-locks through mutex_.lock(),
    std::shared_lock, or a locking helper it calls would still classify as
    lock-free, so this pin fails open for those shapes.
    """
    found = False
    for path in sorted(vendor_dir.glob("*wrapper*.cpp")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in IS_OPEN_BODY_RE.finditer(text):
            found = True
            open_index = match.end() - 1
            body = text[open_index:_matching_brace(text, open_index)]
            if re.search(r"\b(lock_guard|unique_lock|scoped_lock)\b", body):
                return True
    return not found


def classify_get_connected_bodies():
    """({basename: kind}, [findings]) for every vendor get_connected() override.

    kind is "driver-mutex" (takes a lock_guard/unique_lock, so it blocks behind
    the connect sequence that holds the same mutex), "wrapper" (reaches a
    vendor wrapper is_open() that takes the wrapper mutex open() holds
    throughout), or "lock-free" (an atomic load, a plain return, or a wrapper
    is_open() that is itself lock-free, issue #382).
    """
    kinds = {}
    failures = []
    vendors = ROOT / "AlpacaCore" / "src" / "vendors"
    for path in sorted(vendors.rglob("*")):
        if path.suffix not in (".cpp", ".h", ".hpp"):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in GET_CONNECTED_RE.finditer(text):
            open_index = match.end() - 1
            body = text[open_index:_matching_brace(text, open_index)]
            if re.search(r"\b(lock_guard|unique_lock|scoped_lock)\b", body):
                kind = "driver-mutex"
            elif re.search(r"(\.|->)is_open\s*\(\s*\)", body):
                # Blocking only while the wrapper's is_open() itself takes a
                # lock. Since issue #382 the four wrapper-backed switches read
                # an atomic the wrapper publishes, and this is what pins that:
                # re-adding the lock_guard to any is_open() in the vendor's
                # wrapper turns the driver back into "wrapper" (blocking) and
                # the empty switch list in async_connectable.h fails.
                kind = "wrapper" if _vendor_is_open_locks(path.parent) else "lock-free"
            elif re.search(r"\bload\s*\(|\breturn\s+connected_\s*;", body):
                kind = "lock-free"
            else:
                failures.append(
                    "UNCLASSIFIABLE get_connected(): %s -- it neither takes a lock, nor reaches a "
                    "wrapper's is_open(), nor reads an atomic. Classify it by hand: if it can block, "
                    "add it to DRIVER_PROSE_NAMES in %s and to the list in async_connectable.h; if it "
                    "cannot, make that visible in the body (an atomic load or a plain return)."
                    % (path.relative_to(ROOT), Path(__file__).name))
                continue
            # A file with two driver classes (gemini_flatpanel_driver.cpp) is
            # only interesting if they disagree, which would mean the file
            # cannot be named as one thing in the prose list.
            if kinds.get(path.name, kind) != kind:
                failures.append(
                    "MIXED get_connected() kinds in %s: %s and %s. The prose list names files, not "
                    "classes, so split the drivers or name them individually."
                    % (path.relative_to(ROOT), kinds[path.name], kind))
            kinds[path.name] = kind
    return kinds, failures


def blocking_list_findings(header, kinds):
    """Check 5's comparison over (async_connectable.h text, {basename: kind}). Pure, so
    --self-test can drive it."""
    failures = []
    spans, span_failures = _blocking_list_spans(header)
    failures.extend(span_failures)
    if span_failures:
        return failures

    listed_names = {which: set(_names_in_span(which, span)) for which, span in spans.items()}

    def named(entry):
        # Whole parsed names, not a substring of the sentence: substring
        # matching is only safe here because no name is a prefix of another
        # WITHIN one list today, and that is not a property worth depending on
        # (it already failed across the two lists -- "iOptron" inside "iOptron
        # iMate PowerBox").
        prose, which = entry
        return prose in listed_names[which]

    # Parse the names OUT of each list, so a name that should not be there is
    # caught even when no driver file maps to it. Probing only for the names in
    # DRIVER_PROSE_NAMES could never see that: the dict holds only the drivers
    # that ARE blocking, so a lock-free driver named in the header (SynScan
    # after #130 -- the drift this whole check exists for) matched nothing and
    # passed.
    expected = {which: set() for which in BLOCKING_LIST_SPANS}
    for basename, kind in kinds.items():
        entry = DRIVER_PROSE_NAMES.get(basename)
        if entry is not None and kind != "lock-free":
            expected[entry[1]].add(entry[0])
    for which, span in spans.items():
        for listed in _names_in_span(which, span):
            if listed not in expected[which]:
                failures.append(
                    "STALE BLOCKING-LIST ENTRY: async_connectable.h's %s list names '%s', but no "
                    "driver with that name has a blocking get_connected(). Either it was made "
                    "lock-free and the name must come out, or the name does not match "
                    "DRIVER_PROSE_NAMES." % (which, listed))

    for basename, kind in sorted(kinds.items()):
        if kind == "lock-free":
            # The lock-free majority is deliberately unnamed. What matters is
            # that it is not named as blocking: a driver made lock-free (as
            # SynScan was by #130) must come OUT of the list.
            entry = DRIVER_PROSE_NAMES.get(basename)
            if entry and named(entry):
                failures.append(
                    "STALE BLOCKING-LIST ENTRY: %s's get_connected() is lock-free, but "
                    "async_connectable.h still names '%s' among the drivers that block. Remove it there "
                    "and from DRIVER_PROSE_NAMES." % (basename, entry[0]))
            continue
        entry = DRIVER_PROSE_NAMES.get(basename)
        if entry is None:
            failures.append(
                "UNNAMED BLOCKING DRIVER: %s's get_connected() is %s, so it blocks behind its connect "
                "sequence, but no prose name is registered for it. Add it to DRIVER_PROSE_NAMES in %s "
                "and to the list in async_connectable.h -- the router rule and the pending_mutex_ "
                "ordering both depend on that list being complete." % (basename, kind, Path(__file__).name))
            continue
        if not named(entry):
            failures.append(
                "MISSING FROM THE BLOCKING LIST: %s's get_connected() is %s, but async_connectable.h's "
                "%s list does not name '%s'." % (basename, kind, entry[1], entry[0]))
    return failures


def check_blocking_get_connected_list():
    kinds, failures = classify_get_connected_bodies()
    if not kinds:
        return ["No get_connected() overrides found under AlpacaCore/src/vendors -- the scan is broken."]

    failures.extend(blocking_list_findings(
        read("AlpacaCore/include/alpacacore/async_connectable.h"), kinds))

    # The counts this PR removed must not creep back. A number in front of
    # "telescopes"/"wrapper-backed switch(es)" is exactly the thing nothing
    # checks and that went stale repeatedly -- the lists themselves are gated
    # above, so a count adds nothing but a second source of truth.
    count_re = re.compile(
        r"\b(two|three|four|five|six|seven|eight|nine|ten|\d+)\s+"
        r"(?:named\s+|more\s+)?(?:telescopes?|wrapper-backed\s+switch(?:es)?)\b",
        re.IGNORECASE)
    # Anchored on the noun deliberately. A rule that also caught a bare "the
    # five" (noun implied) was tried and dropped: "the two locked phases", "the
    # two ZWO drivers" and similar ordinary prose light it up, so it fails on
    # correct text. A count with the noun left implied has to be caught by
    # review -- which is how the one at AGENTS.md:777 was.
    # Globbed, not a hand-written file list: the first version of this loop
    # named four files and missed synscan_telescope_driver.cpp, which carried
    # the counts -- a gate against drift that itself drifts is worth very
    # little. CHANGELOG.md is deliberately out of scope: its historical entries
    # describe what was true when they were written.
    # ROOT.glob, not glob.glob: the latter is cwd-relative, so running this
    # from anywhere but the repo root would return nothing and turn the rule
    # into a silent no-op -- the same failure shape as the hand-written file
    # list it replaced. Headers and tests are in scope too; a count is just as
    # stale in test_async_connectable.cpp as in a driver.
    counted_paths = sorted(
        str(p.relative_to(ROOT))
        for pattern in ("AGENTS.md", "README.md", "docs/**/*.md",
                        ".github/instructions/**/*.instructions.md",
                        "AlpacaCore/include/**/*.h", "AlpacaCore/src/**/*.h",
                        "AlpacaCore/src/**/*.cpp", "AlpacaCore/tests/**/*.h",
                        "AlpacaCore/tests/**/*.cpp",
                        "AlpacaHTTP/include/**/*.h", "AlpacaHTTP/src/**/*.cpp",
                        "AlpacaHTTP/tests/**/*.cpp")
        for p in ROOT.glob(pattern))
    for path in counted_paths:
        for match in count_re.finditer(read(path)):
            failures.append(
                "COUNTED BLOCKING DRIVERS: %s says %r. These lists are gated by name; a count is a "
                "second source of truth that nothing checks and that has gone stale before "
                "(issue #381). Refer to the named list instead." % (path, match.group(0)))

    return failures


# --- check 6: the QHYSDK seam's three parallel lists ------------------------
#
# issue #394. The forward sweep in test_qhy_fake_sdk.cpp drives every QHYSDK
# method through LockedQHYSDK and asserts each landed on its own counterpart
# exactly once, which is a genuine (mutation-verified) guard against a
# TRANSPOSED forward. It is not a guard against an ABSENT one: the method list
# is hand-written in the test and closed with `methods.size() == N`, a literal
# compared to a literal. Add a pure virtual to QHYSDK and the compiler forces a
# LockedQHYSDK override -- the class would otherwise be abstract -- but nothing
# forces a test entry, so the sweep passes having exercised N of N+1 forwards.
#
# And the compiler only guarantees the forward EXISTS. Nothing guarantees it
# takes the mutex, which is the only reason the decorator exists: its job is to
# keep ThreadSanitizer findings pointing at driver code rather than at the
# deliberately unhardened fake, and one unlocked forward makes the fake racy
# under a storm and produces a TSan report naming the fake -- the exact
# confusion the decorator was built to prevent, arriving silently.


def _class_body(text, class_name):
    """The text between `class <name> ... {` and its matching brace, or None."""
    match = re.search(r"\bclass\s+%s\b[^{;]*\{" % re.escape(class_name), text)
    if not match:
        return None
    return text[match.end():_matching_brace(text, match.end() - 1) - 1]


QHY_CANCEL_EXEMPT = "cancel_exposure"
QHY_VERSION_EXEMPT = "get_sdk_version"


def _qhy_forward_lock_findings(locked_methods, interface_methods):
    """The lock each LockedQHYSDK forward takes. Pure, so --self-test can drive it.

    Two methods own a mutex of their own: cancel_exposure (cancel_mutex_,
    open-astro#339) and get_sdk_version (version_mutex_, open-astro#1646: the
    hook must not queue behind an in-flight open_camera). Every other forward
    must take the shared mutex through the one-argument `locked([&] {...})`;
    `locked(other_mutex_, ...)` skips the shared mutex and is rejected."""
    failures = []
    for name in sorted(set(locked_methods) & interface_methods):
        body = locked_methods[name]
        shared_named = re.search(r"(?<![\w])mutex_\b", body)
        if name == QHY_CANCEL_EXEMPT:
            # Mechanical, not a substring: the body must take the lock_guard on
            # cancel_mutex_ AND must not go through locked(). A body that does
            # both (shared mutex plus a mention of cancel_mutex_) is the
            # inverted shape #339 fixed, and a substring test passed it.
            # `lock_guard<std::mutex>` or CTAD `lock_guard` both count; any
            # mention of the shared `mutex_` (locked() or a hand-rolled guard)
            # is the inverted shape, so it is rejected by name, not by idiom.
            takes_own = re.search(r"lock_guard\s*(?:<\s*std::mutex\s*>)?\s*\w+\s*\(\s*cancel_mutex_\s*\)", body)
            touches_shared = "locked(" in body or shared_named
            if not takes_own or touches_shared:
                failures.append(
                    "LockedQHYSDK::cancel_exposure() must take cancel_mutex_ -- its own lock, separate "
                    "from the shared one, per open-astro#339. Through the shared mutex it queues behind "
                    "the in-flight call it exists to interrupt; with no mutex at all it becomes the one "
                    "racy forward in the decorator.")
            continue
        if name == QHY_VERSION_EXEMPT:
            takes_own = re.search(r"\blocked\s*\(\s*version_mutex_\s*,", body)
            if not takes_own or shared_named or re.search(r"\blocked\s*\(\s*\[", body):
                failures.append(
                    "LockedQHYSDK::get_sdk_version() must take version_mutex_ (locked(version_mutex_, ...)) "
                    "and not the shared mutex_, per open-astro#1646: through the shared mutex the hook "
                    "queues behind an in-flight open_camera(), which the real wrapper's leaf "
                    "sdk_version_mutex does not.")
            continue
        # Every other forward: the one-argument locked([&] {...}), which takes mutex_.
        if not re.search(r"\blocked\s*\(\s*\[", body) or re.search(r"\blocked\s*\(\s*[A-Za-z_]", body):
            failures.append(
                "UNLOCKED FORWARD: LockedQHYSDK::%s() does not go through the one-argument locked([&] {...}). "
                "The decorator exists only to take the shared mutex -- an unlocked forward (or one that "
                "passes locked() another mutex) makes the fake racy under a [stress] storm and produces a "
                "ThreadSanitizer report naming the FAKE, which is the confusion the decorator was built "
                "to prevent." % name)
    return failures


def check_qhy_seam_lists():
    failures = []
    interface_body = _class_body(_strip_comments(read(QHY_INTERFACE_HEADER)), "QHYSDK")
    locked_body = _class_body(_strip_comments(read(QHY_LOCKED_HEADER)), "LockedQHYSDK")
    if interface_body is None or locked_body is None:
        return ["Could not locate class QHYSDK and/or class LockedQHYSDK -- this check's parser is broken."]

    interface_methods = set(PURE_VIRTUAL_RE.findall(interface_body))
    if not interface_methods:
        return ["No pure virtuals found on QHYSDK -- this check's parser is broken."]

    # Cross-check the count against the literal the sweep test already asserts
    # (CHECK(methods.size() == N)). Everything else here is set differences, so
    # a method the regex fails to see drops out of ALL of them and the gate goes
    # quietly green -- which is exactly what a const method did until round 2.
    # Comparing against a number maintained elsewhere turns the next such miss
    # into a loud failure, and pins that literal at the same time.
    sweep_text = read(QHY_SWEEP_TEST)
    size_match = re.search(r"\bmethods\.size\(\)\s*==\s*(\d+)", sweep_text)
    if size_match is None:
        failures.append(
            "Could not find the `CHECK(methods.size() == N)` literal in %s -- this check uses it to "
            "detect a method its regexes silently missed, so losing it would make that failure "
            "invisible again." % QHY_SWEEP_TEST)
    elif int(size_match.group(1)) != len(interface_methods):
        failures.append(
            "QHY SEAM COUNT MISMATCH: %s asserts methods.size() == %s, but %d pure virtuals were "
            "parsed off QHYSDK. Either the sweep literal is stale, or a method's declaration has a "
            "shape the parser in %s does not match (a const or noexcept qualifier did exactly that "
            "once) -- in which case the set comparisons below would pass while missing it."
            % (QHY_SWEEP_TEST, size_match.group(1), len(interface_methods), Path(__file__).name))

    # Each override, with its body, so the lock can be checked too.
    locked_methods = {}
    for match in OVERRIDE_RE.finditer(locked_body):
        open_index = match.end() - 1
        locked_methods[match.group(1)] = locked_body[open_index:_matching_brace(locked_body, open_index)]

    for name in sorted(interface_methods - set(locked_methods)):
        failures.append(
            "QHYSDK::%s() has no LockedQHYSDK override. (If this fires, the parser in %s is wrong: an "
            "unimplemented pure virtual would make LockedQHYSDK abstract and fail the build.)"
            % (name, Path(__file__).name))
    for name in sorted(set(locked_methods) - interface_methods):
        failures.append(
            "LockedQHYSDK::%s() overrides nothing on QHYSDK -- stale forward, or the interface lost a "
            "method." % name)

    # Two documented exemptions from the shared mutex, each taking its own
    # mutex so the fake stays non-racy (rationale in the helper's docstring):
    # cancel_exposure (open-astro#339) must not queue behind the call it
    # interrupts, and get_sdk_version (open-astro#1646) must not queue behind
    # an in-flight open_camera.
    failures.extend(_qhy_forward_lock_findings(locked_methods, interface_methods))

    # The hand-written sweep list in the test.
    sweep = read(QHY_SWEEP_TEST)
    list_match = re.search(r"const std::vector<std::string> methods\{(.*?)\};", sweep, re.S)
    if not list_match:
        failures.append(
            "Could not find the `const std::vector<std::string> methods{...}` sweep list in %s."
            % QHY_SWEEP_TEST)
        return failures
    swept = set(re.findall(r'"([^"]+)"', list_match.group(1)))
    for name in sorted(interface_methods - swept):
        failures.append(
            "NOT SWEPT: QHYSDK::%s() is not in the forward sweep's method list in %s. The sweep is what "
            "checks the forward reaches its own counterpart; a method missing from the list is verified "
            "by inspection only." % (name, QHY_SWEEP_TEST))
    for name in sorted(swept - interface_methods):
        failures.append(
            "STALE SWEEP ENTRY: %s is in the sweep list in %s but is not a QHYSDK method."
            % (name, QHY_SWEEP_TEST))
    return failures


# --- check 7: AGENTS.md path references exist -------------------------------

# Backtick-quoted spans that look like a repo-relative path: start with one of
# these top-level dirs/files (spaces allowed only for a verbatim tracked path), and are not a bare CLI flag
# or a URL.
PATH_PREFIXES = (
    "AlpacaCore/", "AlpacaHTTP/", "scripts/", "docs/", ".github/",
    ".claude/", "debian/",
)
# Spans are matched only after fenced code blocks are removed (their triple
# backticks would otherwise pair across lines and invert every later match,
# which silently dropped 66 of 83 path references in the first cut of this
# fix). With fences gone, pairing is consistent even for a span that wraps
# onto the next line; such a span is skipped, since a path never wraps.
# Fences: three or more backticks or tildes, optionally indented (list items).
FENCED_BLOCK_RE = re.compile(r"^[ \t]*(`{3,}|~{3,}).*?^[ \t]*\1[ \t]*$", re.S | re.M)
# A double-backtick span shows a literal backtick (`` ` ``) and would break
# single-backtick parity; it is never a path reference, so drop it first.
DOUBLE_BACKTICK_SPAN_RE = re.compile(r"``.+?``")
CODE_SPAN_RE = re.compile(r"`([^`]+)`")
# Tripwire for the span matcher, not a rule about document size: if
# AGENTS.md is legitimately trimmed below this, lower the floor.
MIN_AGENTS_MD_PATH_REFS = 15
# Tripwire for a docs/agents/ rename, not a target file count.
MIN_AGENTS_DIR_FILES = 1
# Tripwire for a .claude/skills/ rename, not a target file count.
MIN_SKILL_FILES = 1
# Trailing punctuation/anchors that can ride along inside a backtick span.
TRIM_SUFFIX_RE = re.compile(r"[),.;:]+$")


def _run_git(args, check=True, root=ROOT):
    return subprocess.run(
        ["git"] + args, cwd=root, capture_output=True, text=True, check=check
    )


def _is_gitignored(path, root=ROOT):
    """True if git would ignore this path (e.g. a generated file/dir).

    Used instead of a plain filesystem exists() check: a generated file like
    debian/changelog can be present in one developer's tree from a past
    local build (making exists() pass by accident there) while being absent
    from every clean checkout, including CI's. A path git ignores is
    expected to be absent and isn't a documentation error.
    """
    return _run_git(["check-ignore", "-q", path], check=False, root=root).returncode == 0


# Keyed by root so a fixture repository never sees the real tree's listing (or
# another fixture's); the real run uses one root, so it is still computed once.
_TRACKED_PATHS_CACHE = {}


def _tracked_paths(root=ROOT):
    """(tracked files, tracked directories with a trailing slash), computed once
    per root: six documents share it and the listing walks the vendored SDK
    trees. `root` is a parameter so the self-test can drive this over a
    fixture repository; the real run passes nothing."""
    key = str(root)
    if key in _TRACKED_PATHS_CACHE:
        return _TRACKED_PATHS_CACHE[key]
    # core.quotePath=false: a tracked path with non-ASCII bytes must not
    # come back quoted, or it would never match a span.
    tracked = set(_run_git(["-c", "core.quotePath=false", "ls-files"], root=root).stdout.splitlines())
    # A migration creates instruction files before they are staged. Include
    # those files so references to them can be checked in the working tree.
    # Every directory check_agents_md_paths_exist scans must appear here, or a
    # doc written but not yet `git add`ed is reported as drift (issue #556a);
    # the skills tree is nested, so its pattern is recursive to match the
    # rglob that scans it.
    for pattern in (".github/instructions/*.instructions.md", "docs/failures/*.md", "docs/decisions/*.md",
                    "docs/agents/*.md", ".claude/skills/**/*.md"):
        tracked.update(p.relative_to(root).as_posix() for p in root.glob(pattern))
    tracked_dirs = set()
    for f in tracked:
        parts = f.split("/")
        for i in range(1, len(parts)):
            tracked_dirs.add("/".join(parts[:i]) + "/")
    _TRACKED_PATHS_CACHE[key] = (tracked, tracked_dirs)
    return _TRACKED_PATHS_CACHE[key]


# Spans under these trees may carry `<vendor>` placeholders; see _check_doc_path_refs.
DESCRIPTOR_TEMPLATE_PREFIXES = ("AlpacaCore/src/vendors/", "AlpacaCore/src/catalog/")


def _check_doc_path_refs(doc, floor, floor_name, component=None, relative_prefixes=(), root=ROOT):
    """Every backticked path span in `doc` names a tracked file or directory.

    `component` (e.g. "AlpacaCore/") is where a span starting with one of
    `relative_prefixes` ("src/", "external/", ...) is resolved: the Cursor
    rule files write paths relative to the component they live under, not
    to the repo root. Repo-root spans (PATH_PREFIXES) are accepted in every
    document. Returns (failures, checked). `root` is a parameter so the
    self-test can drive this over a fixture repository; the real run passes
    nothing.
    """
    failures = []
    if not (root / doc).is_file():
        return (["%s is listed for path checking but does not exist -- it was renamed or deleted; "
                 "update the list" % doc], 0)
    text = FENCED_BLOCK_RE.sub("", read(doc, root))
    text = DOUBLE_BACKTICK_SPAN_RE.sub("", text)
    # With fences gone every backtick must pair up; one stray backtick would
    # invert every span after it, and the count floor below only catches a
    # large inversion. Fail loudly on parity instead.
    if text.count("`") % 2 != 0:
        return ["%s has an unbalanced backtick outside fenced blocks; "
                "the path-reference check cannot pair code spans reliably" % doc], 0
    seen = set()
    tracked, tracked_dirs = _tracked_paths(root)
    tracked_all = tracked | tracked_dirs

    checked = 0
    for m in CODE_SPAN_RE.finditer(text):
        span = m.group(1)
        if "\n" in span:
            continue
        if span.startswith(PATH_PREFIXES):
            span_path = span
        elif component and span.startswith(relative_prefixes):
            span_path = component + span
        else:
            continue
        checked += 1
        path = TRIM_SUFFIX_RE.sub("", span_path)
        # Historical audit records cite file:line and file:start-end. Check
        # the file itself; line numbers in a resolved snapshot need not stay
        # current as code moves.
        path = re.sub(r":\d+(?:-\d+)?$", "", path)
        # Markdown anchors / fragments (`docs/x.md#section`) and glob patterns
        # aren't real filesystem paths.
        if "#" in path or "*" in path:
            continue
        # A template placeholder (`<vendor>`) is skipped, except under the
        # descriptor trees: there `.../<vendor>/<vendor>_fields.h` must match
        # at least one tracked file, `<x>` standing for one name.
        if "<" in path and path.startswith(DESCRIPTOR_TEMPLATE_PREFIXES):
            pattern = re.compile(re.sub(r"<[^<>/]+>", "[^/]+", re.escape(path)) + "/?")
            if path not in seen and not any(pattern.fullmatch(t) for t in tracked_all):
                failures.append("%s references a descriptor path that matches no tracked file: %s" % (doc, path))
            seen.add(path)
            continue
        if "<" in path or ">" in path:
            continue
        if path in seen:
            continue
        seen.add(path)

        # A directory may be named with or without its trailing slash.
        if path in tracked or path in tracked_dirs or path + "/" in tracked_dirs:
            continue
        # A span with whitespace is validated only when it names a tracked
        # file or directory verbatim (e.g. `AlpacaCore/conformu/Astroasis/
        # Oasis Focuser/`, handled above). Otherwise it is SKIPPED, not
        # failed: it may be prose (`AlpacaCore/tests/ and AlpacaHTTP/`) and
        # cannot be told apart from a drifted spaced path. So a spaced path
        # that later drifts stays green here; that is the accepted trade.
        if any(ch.isspace() for ch in path):
            continue
        # Not a tracked file or the directory of one: a generated/ignored
        # path (debian/changelog, a `.../build/` output dir) is expected to
        # be absent from a clean checkout, so it isn't a documentation error.
        if _is_gitignored(path, root):
            continue
        failures.append("%s references a path that does not exist: %s" % (doc, path))
    if checked < floor:
        failures.append(
            "only %d backticked path references found in %s (floor %d): "
            "either the code-span matcher regressed, or the file was trimmed "
            "and %s should be lowered" % (checked, doc, floor, floor_name))
    return failures, checked


def check_agents_md_paths_exist(root=ROOT):
    """`root` is a parameter so the self-test can drive this over a fixture
    repository; the real run passes nothing.

    The `ls-files` pathspecs are `:(glob)dir/*.md`: a bare `dir/*.md` lets git's
    `*` cross `/`, so a tracked `docs/agents/sub/y.md` was listed by git but
    never found by the one-level `Path.glob` below and reported as "missing".
    `.claude/skills/` is recursive on both sides and stays a directory spec.
    """
    failures, _ = _check_doc_path_refs("AGENTS.md", MIN_AGENTS_MD_PATH_REFS, "MIN_AGENTS_MD_PATH_REFS", root=root)
    context_failures, _ = _check_doc_path_refs("CONTEXT.md", 0, "CONTEXT.md floor", root=root)
    failures.extend(context_failures)
    # README.md carries no path spans today, so the floor is 0 like CONTEXT.md;
    # the file itself must exist. Its relative links are the instruction-
    # structure check's job (ROOT_DOCUMENT_LINK_FLOORS['README.md'] in
    # scripts/check_instruction_structure.py), issue #693.
    readme_failures, _ = _check_doc_path_refs("README.md", 0, "README.md floor", root=root)
    failures.extend(readme_failures)
    instruction_dir = root / ".github/instructions"
    files = sorted(instruction_dir.glob("*.instructions.md"))
    tracked = _run_git(["-c", "core.quotePath=false", "ls-files", ":(glob).github/instructions/*.instructions.md"], root=root).stdout.splitlines()
    for missing in sorted(set(tracked) - {p.relative_to(root).as_posix() for p in files}):
        failures.append("%s is a tracked instruction file but is missing" % missing)
    for path in files:
        doc = path.relative_to(root).as_posix()
        doc_failures, _ = _check_doc_path_refs(doc, 0, "instruction file floor", root=root)
        failures.extend(doc_failures)
    agents_dir = root / "docs/agents"
    agents_files = sorted(agents_dir.glob("*.md"))
    tracked_agents = _run_git(["-c", "core.quotePath=false", "ls-files", ":(glob)docs/agents/*.md"], root=root).stdout.splitlines()
    # A directory rename would make both the glob and ls-files go empty and
    # this whole block would silently pass nothing -- the vacuity class
    # decision record 0003 calls out. Floor is today's file count (1); it is
    # a tripwire, not a target.
    if len(tracked_agents) < MIN_AGENTS_DIR_FILES:
        failures.append(
            "only %d tracked file(s) found under docs/agents/*.md (floor %d): "
            "the directory may have been renamed, or check_agents_md_paths_exist "
            "should be updated" % (len(tracked_agents), MIN_AGENTS_DIR_FILES))
    for missing in sorted(set(tracked_agents) - {p.relative_to(root).as_posix() for p in agents_files}):
        failures.append("%s is a tracked agent-skills doc but is missing" % missing)
    for path in agents_files:
        doc = path.relative_to(root).as_posix()
        doc_failures, _ = _check_doc_path_refs(doc, 0, "agent-skills doc floor", root=root)
        failures.extend(doc_failures)
    skill_files = sorted((root / ".claude/skills").rglob("*.md"))
    tracked_skills = _run_git(["-c", "core.quotePath=false", "ls-files", ".claude/skills/"], root=root).stdout.splitlines()
    tracked_skills = [p for p in tracked_skills if p.endswith(".md")]
    if len(tracked_skills) < MIN_SKILL_FILES:
        failures.append(
            "only %d tracked file(s) found under .claude/skills/ (floor %d): "
            "the directory may have been renamed, or check_agents_md_paths_exist "
            "should be updated" % (len(tracked_skills), MIN_SKILL_FILES))
    for missing in sorted(set(tracked_skills) - {p.relative_to(root).as_posix() for p in skill_files}):
        failures.append("%s is a tracked skill doc but is missing" % missing)
    for path in skill_files:
        doc = path.relative_to(root).as_posix()
        doc_failures, _ = _check_doc_path_refs(doc, 0, "skill doc floor", root=root)
        failures.extend(doc_failures)
    for directory in ("docs/failures", "docs/decisions"):
        paths = sorted((root / directory).glob("*.md"))
        tracked_memory = _run_git(["-c", "core.quotePath=false", "ls-files", ":(glob)" + directory + "/*.md"], root=root).stdout.splitlines()
        for missing in sorted(set(tracked_memory) - {p.relative_to(root).as_posix() for p in paths}):
            failures.append("%s is a tracked memory record but is missing" % missing)
        for path in paths:
            doc = path.relative_to(root).as_posix()
            doc_failures, _ = _check_doc_path_refs(doc, 0, "memory record floor", root=root)
            failures.extend(doc_failures)
    failures.extend(check_memory_comment_paths_exist(root))
    return failures


MEMORY_COMMENT_PATH_RE = re.compile(r"\bdocs/(?:failures|decisions)/[A-Za-z0-9._-]+\.md\b")
# A comment may cite a record by number only (`docs/decisions/0008`, PR #1410 audit):
# the number must name exactly one record.
MEMORY_COMMENT_ID_RE = re.compile(r"\bdocs/(failures|decisions)/(\d{4})(?!-|\w|\.\w)")
FIRST_PARTY_COMMENT_EXTENSIONS = {".cpp", ".h", ".js", ".py", ".sh"}
# The real tree has ~330 candidate files; the self-test fixture has a handful.
MIN_MEMORY_COMMENT_FILES = 5


def check_memory_comment_paths_exist(root=ROOT):
    """Validate memory-record paths named in first-party source comments.

    `root` is a parameter so the self-test can drive this over a fixture
    tree; the real run passes nothing.
    """
    failures = []
    scanned = 0
    for component in ("AlpacaCore", "AlpacaHTTP", "scripts"):
        if not (root / component).is_dir():
            continue
        for path in (root / component).rglob("*"):
            if not path.is_file() or path.suffix not in FIRST_PARTY_COMMENT_EXTENSIONS:
                continue
            rel = path.relative_to(root)
            # Exclusions match repo-relative DIRECTORY components only. The
            # first version tested `path.parts`, which is absolute and ends in
            # the file's own name: `scripts/build_deb.sh` was never scanned,
            # and a checkout under a directory whose name starts with "build"
            # skipped every file and reported a pass having read nothing.
            directories = rel.parts[:-1]
            if "external" in directories or any(part.startswith("build") for part in directories):
                continue
            scanned += 1
            for lineno, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                if not line.lstrip().startswith(("//", "#", "*")):
                    continue
                for reference in MEMORY_COMMENT_PATH_RE.findall(line):
                    if not (root / reference).is_file():
                        failures.append("%s:%d references a missing memory record: %s" % (rel, lineno, reference))
                for directory, number in MEMORY_COMMENT_ID_RE.findall(line):
                    hits = list((root / "docs" / directory).glob(number + "-*.md"))
                    if len(hits) != 1:
                        failures.append("%s:%d cites docs/%s/%s, which names %d records (expected 1)"
                                        % (rel, lineno, directory, number, len(hits)))
    # Floor, like the other gates: a renamed component directory or a wrong
    # root would otherwise read nothing and report a clean pass.
    if scanned < MIN_MEMORY_COMMENT_FILES:
        failures.append("memory-comment check scanned only %d first-party source file(s) (floor %d): "
                        "the component roots or the exclusion rule regressed" % (scanned, MIN_MEMORY_COMMENT_FILES))
    return failures


# --- check 10: Cursor rule files' path references exist (issue #457) --------

# Spans in a rule file that are relative to its component root. `include/`
# is here for `include/alpacacore/...`; a vendor SDK's own `include/` is
# not a repo-relative path and must be written out from `external/` (the
# ZWO block was, in #457).
# No `docs/` here: PATH_PREFIXES already holds it and is tried first, so a
# `docs/x` span in a rule file always resolves at the repo root (neither
# component has a docs/ tree of its own today).
# `.cursor/` is component-relative on purpose: there is no repo-root .cursor/
# tree, and the rule files cross-reference each other as `.cursor/rules/...`.
RULE_FILE_RELATIVE_PREFIXES = (
    "src/", "include/", "tests/", "external/", "conformu/", "examples/", "web/", ".cursor/",
)
# (document, component root, floor). Floors are per file, as the issue asks,
# so a matcher that stops working on one of them fails rather than reporting
# nothing to check; each is set well under today's count and is a tripwire,
# not a target. AlpacaHTTP's rule file names no repo paths today (its spans
# are URL shapes), so its floor is 0: it is listed so a path added there
# later is checked, not to guard the matcher.
RULE_FILE_PATH_CHECKS = (
    ("AlpacaCore/.cursor/rules/driver_build.mdc", "AlpacaCore/", 20),
    ("AlpacaCore/.cursor/rules/driver_test.mdc", "AlpacaCore/", 3),
    ("AlpacaCore/.cursor/rules/rules.mdc", "AlpacaCore/", 5),
    ("AlpacaHTTP/.cursor/rules/rules.mdc", "AlpacaHTTP/", 0),
    ("AlpacaCore/external/README.md", "AlpacaCore/", 3),
)


def check_rule_file_paths_exist(root=ROOT):
    """`root` is a parameter so the self-test can drive this over a fixture
    repository; the real run passes nothing."""
    failures = []
    # The tuple is hand-written; every tracked rule file must be in it, or a
    # fifth .mdc added later is silently unchecked -- the drift class this
    # check exists for (review note on PR #474).
    listed = {doc for doc, _, _ in RULE_FILE_PATH_CHECKS}
    tracked_rule_files = [f for f in _run_git(["-c", "core.quotePath=false", "ls-files",
                                               "*/.cursor/rules/*.mdc", ".cursor/rules/*.mdc"],
                                              root=root).stdout.splitlines() if f]
    for f in sorted(set(tracked_rule_files) - listed):
        failures.append("%s is a tracked Cursor rule file but is not in RULE_FILE_PATH_CHECKS -- add it "
                        "with its component and a floor" % f)
    for doc, component, floor in RULE_FILE_PATH_CHECKS:
        doc_failures, _ = _check_doc_path_refs(
            doc, floor, "its RULE_FILE_PATH_CHECKS floor", component, RULE_FILE_RELATIVE_PREFIXES, root=root)
        failures.extend(doc_failures)
    return failures


# --- check 8: TSan filtered-run sync (ci.yml vs ci_preflight.sh) ------------

# Both files spell out the same filtered TSan runs -- today `[stress]` for the
# vendor registrations and `[stress-guard]` for the harness self-tests -- and
# each run carries its own zero-test grep, because a tag filter matching no
# tests exits 0 and Catch2 reports that as success. Nothing compared the two
# files, so editing one filtered run (retagging it, tightening the grep,
# adding a third invocation) could silently leave the other behind and the
# pre-flight would stop being a faithful mirror of CI. Issue #341.
#
# Matches both spellings of the invocation: bare in ci.yml
# (`./AlpacaCore/build-tsan/tests/alpacacore_tests "[stress]" | tee stress-run.log`)
# and quoted-path in ci_preflight.sh
# (`"${TSAN_BUILD_DIR}/tests/alpacacore_tests" "[stress]" | tee "${TSAN_BUILD_DIR}/stress-run.log"`).
# The trailing quoted tag is what distinguishes a real run from the `test -x`
# / `[ -x ... ]` existence probes on the same binary in both files. The build
# directory (the path segment before `/tests/`) and the `tee` target are
# captured too: the first so a build-directory rename in one file only is
# visible, the second so each zero-test grep can be paired with the log its
# run actually wrote (issue #455).
# The tag is what makes an invocation a run; the `| tee <log>` that follows is
# OPTIONAL in the match so a run written without one is still counted, still
# tag-compared and still required to have a guard -- which it cannot have,
# since no grep can read a log it never wrote, so the pairing reports it
# (review finding on PR #472: with `tee` mandatory, such a run was invisible
# to the whole check).
TSAN_RUN_RE = re.compile(
    r'([^\s"]*)/tests/alpacacore_tests"?\s+"(\[[^"]+\])"'
    r'(?:\s*(?:2>&1\s*)?\|&?\s*tee\s+"?([^\s"]+)"?)?')
TSAN_GREP_RE = re.compile(r"grep\s+-qE\s+'([^']+)'\s+(?:<\s*)?\"?([^\s\"<]+)\"?")
# ci_preflight.sh spells the build directory through a variable; this is its
# one assignment, so the two paths can be compared by basename.
TSAN_BUILD_DIR_RE = re.compile(r'^\s*TSAN_BUILD_DIR="?([^"\n]+?)"?\s*$', re.MULTILINE)

# A floor, not a count: it exists only so a regex that stops matching fails
# loudly instead of comparing two empty sets and passing. Deliberately NOT set
# to today's two runs -- the number of filtered runs is the check's subject,
# not a second place to state it (see check 5 on counts as a second source of
# truth).
MIN_TSAN_FILTERED_RUNS = 1


def _basename(path):
    return path.rsplit("/", 1)[-1]


def _tsan_events(block):
    """The filtered runs and zero-test greps of one TSan block, in file order.

    Each run is ("run", tag, build_dir, tee_log) and each grep is
    ("grep", pattern, log), all paths reduced to their basename so ci.yml's
    literal `stress-run.log` pairs with ci_preflight.sh's
    `"${TSAN_BUILD_DIR}/stress-run.log"` without expanding the variable.
    A run with no `| tee` has tee_log None: it is still a run, and the
    pairing below reports it, because no grep can guard a log never written.
    """
    events = []
    for m in TSAN_RUN_RE.finditer(block):
        tee_log = _basename(m.group(3)) if m.group(3) else None
        events.append((m.start(), ("run", m.group(2), _basename(m.group(1)), tee_log)))
    for m in TSAN_GREP_RE.finditer(block):
        events.append((m.start(), ("grep", m.group(1), _basename(m.group(2)))))
    return [event for _, event in sorted(events)]


def _pair_tsan_runs(label, events, failures):
    """Pair every run with the grep that follows it; return [(tag, pattern)].

    The rule is positional, not a count: a retagged run whose `tee` target
    was not retagged would leave the run count and the grep count equal
    while its grep read the PREVIOUS run's log, which is the vacuous-pass
    shape the greps exist to prevent, reached from a direction a count
    cannot see (issue #455).
    """
    pairs = []
    pending = None
    for event in events:
        if event[0] == "run":
            if pending is not None:
                failures.append(
                    "%s: filtered TSan run %s is not followed by a zero-test "
                    "grep of its log %s before the next run -- every filtered "
                    "run needs its own guard, or the run reports success having "
                    "executed nothing" % (label, pending[1], pending[3] or "(no tee)"))
            pending = event
            continue
        _, pattern, log = event
        if pending is None:
            failures.append(
                "%s: zero-test grep of %s has no filtered TSan run before it -- "
                "an unpaired guard means the two files have drifted"
                % (label, log))
            continue
        if pending[3] is None:
            failures.append(
                "%s: filtered TSan run %s does not `| tee` a log, so the grep of "
                "%s that follows it cannot be reading that run's output -- pipe "
                "the run into a log and grep that log" % (label, pending[1], log))
            pending = None
            continue
        if log != pending[3]:
            failures.append(
                "%s: filtered TSan run %s writes %s but the grep that follows "
                "it reads %s -- the guard must read the log of the run it "
                "guards" % (label, pending[1], pending[3], log))
        pairs.append((pending[1], pattern))
        pending = None
    if pending is not None:
        failures.append(
            "%s: filtered TSan run %s is not followed by a zero-test grep of "
            "its log %s -- every filtered run needs its own guard, or the run "
            "reports success having executed nothing"
            % (label, pending[1], pending[3] or "(no tee)"))
    return pairs


def check_tsan_filtered_runs_sync():
    return _tsan_findings(read(".github/workflows/ci.yml"), read("scripts/ci_preflight.sh"))


def _tsan_findings(ci_full, preflight_full):
    """Check 8 over the two files' text. Pure, so --self-test can drive it
    over literal fixtures without touching the repo."""
    failures = []
    ci = _ci_job_block(ci_full, "sanitizers-tsan")
    preflight = _scoped_block(
        preflight_full,
        'section "ThreadSanitizer (concurrency stress, all vendors)"',
        ("\n# --- ",),
    )
    if ci is None or preflight is None:
        failures.append(
            "could not find the sanitizers-tsan job in ci.yml or the "
            "ThreadSanitizer gate in ci_preflight.sh -- update this check's "
            "markers if either file's structure changed"
        )
        return failures

    ci_events = _tsan_events(ci)
    pf_events = _tsan_events(preflight)
    ci_runs = [e for e in ci_events if e[0] == "run"]
    pf_runs = [e for e in pf_events if e[0] == "run"]

    for label, runs in (("ci.yml", ci_runs), ("ci_preflight.sh", pf_runs)):
        if len(runs) < MIN_TSAN_FILTERED_RUNS:
            failures.append(
                "found %d filtered TSan run(s) in %s (floor %d) -- either the "
                "invocation matcher regressed or the TSan gate was removed"
                % (len(runs), label, MIN_TSAN_FILTERED_RUNS)
            )
    if failures:
        return failures

    # Ordered, not a set: swapping the two guards between the two runs in one
    # file is harmless only while both patterns are identical.
    ci_pairs = _pair_tsan_runs("ci.yml", ci_events, failures)
    pf_pairs = _pair_tsan_runs("ci_preflight.sh", pf_events, failures)
    if [tag for tag, _ in ci_pairs] != [tag for tag, _ in pf_pairs]:
        failures.append(
            "TSan filtered runs differ: ci.yml runs %s, ci_preflight.sh runs "
            "%s -- a filtered run must be added, retagged or removed in both, "
            "in the same order"
            % ([tag for tag, _ in ci_pairs], [tag for tag, _ in pf_pairs])
        )
    elif ci_pairs != pf_pairs:
        failures.append(
            "TSan zero-test grep pattern differs: ci.yml has %s, "
            "ci_preflight.sh has %s"
            % (ci_pairs, pf_pairs)
        )

    # The build directory: ci.yml names it literally, ci_preflight.sh through
    # TSAN_BUILD_DIR. Renaming it in one file only leaves the other running
    # (or failing to find) a different tree.
    ci_dirs = sorted({run[2] for run in ci_runs})
    pf_dirs = set()
    for run in pf_runs:
        d = run[2]
        if d.startswith("${") and d.endswith("}"):
            m = TSAN_BUILD_DIR_RE.search(preflight_full)
            if m is None:
                failures.append(
                    "ci_preflight.sh runs the TSan binary out of %s but assigns "
                    "no TSAN_BUILD_DIR -- update this check's variable matcher "
                    "if the gate's structure changed" % d)
                continue
            d = _basename(m.group(1))
        pf_dirs.add(d)
    pf_dirs = sorted(pf_dirs)
    if ci_dirs != pf_dirs:
        failures.append(
            "TSan build directory differs: ci.yml runs out of %s, "
            "ci_preflight.sh out of %s" % (ci_dirs, pf_dirs))
    return failures


# --- check 9: AGPL header form (issue #450) ---------------------------------

# First-party source trees and the extensions that must carry the header.
# AlpacaCore/external/ is third-party and never scanned.
LICENSE_HEADER_PREFIXES = (
    "AlpacaCore/src/", "AlpacaCore/include/", "AlpacaCore/tests/",
    "AlpacaCore/examples/",
    "AlpacaHTTP/src/", "AlpacaHTTP/include/", "AlpacaHTTP/tests/",
    "AlpacaHTTP/examples/",
)
# Every C/C++ extension git ls-files could hand back, not only the ones in use
# today: the 100-file floor cannot notice a single unscanned file.
LICENSE_HEADER_EXTENSIONS = (".h", ".hpp", ".hxx", ".hh", ".inl", ".ipp", ".c", ".cc", ".cpp", ".cxx")
# The header must START within this many lines. The block itself is matched
# against the whole file from that point, so a block that begins on line 20
# is not cut mid-way and misreported as missing.
LICENSE_HEADER_LINES = 25

# The current form, one block per component. Matched as a whole, not by a
# single token: grepping for `AGPL` misses nothing but reports the old form
# as "no header", and grepping for `Affero General Public License` accepts
# both forms and catches neither drift (issue #450).
LICENSE_HEADER_FORM = (
    "// {c} is licensed under the GNU Affero General Public License,\n"
    "// version 3 or (at your option) any later version (AGPL-3.0-or-later),\n"
    "// with an additional permission allowing combination with proprietary\n"
    "// device-vendor SDKs. See the LICENSE file in this repository for the full\n"
    "// license text and the vendor-SDK linking exception, or the license online at:\n"
    "// https://www.gnu.org/licenses/agpl-3.0.html\n"
)
# The pre-#113 form, named in the finding so the fix is obvious.
LICENSE_HEADER_OLD_FORM_MARK = "is free software: you can redistribute it and/or modify"

# A floor, not a count (see check 5): the tree has a few hundred first-party
# source files, so a file list that shrinks to a handful means the listing
# or the prefixes regressed, not that the tree did.
MIN_LICENSE_HEADER_FILES = 100


def check_license_headers():
    failures = []
    tracked = _run_git(["-c", "core.quotePath=false", "ls-files"]).stdout.splitlines()
    files = [f for f in tracked
             if f.startswith(LICENSE_HEADER_PREFIXES) and f.endswith(LICENSE_HEADER_EXTENSIONS)]
    if len(files) < MIN_LICENSE_HEADER_FILES:
        return ["found only %d first-party source file(s) to check for a licence "
                "header (floor %d) -- the file listing or LICENSE_HEADER_PREFIXES "
                "regressed" % (len(files), MIN_LICENSE_HEADER_FILES)]
    for f in files:
        component = f.split("/", 1)[0]
        text = read(f)
        head = "".join(text.splitlines(keepends=True)[:LICENSE_HEADER_LINES])
        if "This file is part of %s." % component not in head:
            failures.append("%s: missing the 'This file is part of %s.' line in its "
                            "first %d lines" % (f, component, LICENSE_HEADER_LINES))
        block_at = text.find(LICENSE_HEADER_FORM.format(c=component))
        if block_at >= 0 and text.count("\n", 0, block_at) < LICENSE_HEADER_LINES:
            continue
        if LICENSE_HEADER_OLD_FORM_MARK in head:
            failures.append("%s: carries the pre-#113 long-form GNU header, which names "
                            "neither AGPL-3.0-or-later nor the vendor-SDK linking "
                            "exception -- replace it with the current six-line form "
                            "(copy it from any sibling file)" % f)
        else:
            failures.append("%s: missing the current AGPL-3.0-or-later header block in its "
                            "first %d lines (copy it from any sibling file)"
                            % (f, LICENSE_HEADER_LINES))
    return failures


from check_instruction_structure import check as check_instruction_structure


SKILL_SPEC_PATH = "docs/AlpacaDeviceAPI_v1.yaml"
SKILL_SOURCES_PATH = ".claude/skills/ascom-alpaca-protocol/references/version-and-sources.md"
SKILL_SPEC_PIN_RE = re.compile(r"SHA-256:\s*`([0-9a-f]{64})`")


def check_skill_spec_hash(root=ROOT):
    """Check 11. `root` is a parameter so the self-test can drive it."""
    spec = root / SKILL_SPEC_PATH
    sources = root / SKILL_SOURCES_PATH
    for path in (spec, sources):
        if not path.is_file():
            return ["%s does not exist -- it was renamed or deleted; update check_skill_spec_hash"
                    % path.relative_to(root)]
    pins = SKILL_SPEC_PIN_RE.findall(sources.read_text(encoding="utf-8"))
    if len(pins) != 1:
        return ["%s %s (found %d `SHA-256:` pins, expected 1)"
                % (SKILL_SOURCES_PATH,
                   "has no pinned Device API SHA-256" if not pins
                   else "pins more than one Device API SHA-256, so which one is "
                        "authoritative is ambiguous",
                   len(pins))]
    actual = hashlib.sha256(spec.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
    if actual != pins[0]:
        return ["%s (LF-normalized SHA-256 %s) does not match the snapshot pinned in %s (%s): "
                "regenerate the skill's references/device-api-catalog.md from the new schema, "
                "then update the pin and the verification date"
                % (SKILL_SPEC_PATH, actual, SKILL_SOURCES_PATH, pins[0])]
    return []


# --- check 12: the GPhoto STATUS paragraph names every validated body -------
#
# SUPPORTED-DRIVERS.md's GPhoto table is where a body becomes ConformU-validated;
# .github/instructions/gphoto.instructions.md is the only file a scoped agent
# reads for that vendor, and its STATUS paragraph restated the set by hand
# ("three real Nikon bodies"). Adding the Canon EOS 4000D row left it saying no
# Canon body was validated (PR #626 review). Gated by NAME, like check 5: a
# model in the table that the paragraph does not mention is drift.

GPHOTO_TABLE_ROW_RE = re.compile(r"^\|\s*([^|]+?)\s*\|\s*USB[^|]*\|\s*\u2713\s*\|", re.MULTILINE)


def _gphoto_status_findings(supported, instructions):
    failures = []
    start = supported.find("### GPhoto")
    if start < 0:
        return ["SUPPORTED-DRIVERS.md has no '### GPhoto' section"]
    # The section ends at the next heading of either level, so reordering the
    # file cannot make the gate demand another vendor's models here.
    ends = [i for i in (supported.find("\n### ", start + 1), supported.find("\n## ", start + 1)) if i >= 0]
    section = supported[start:min(ends) if ends else len(supported)]
    models = GPHOTO_TABLE_ROW_RE.findall(section)
    if not models:
        return ["SUPPORTED-DRIVERS.md GPhoto table lists no validated USB models"]

    m = re.search(r"\*\*STATUS:.*?(?:\n\s*\n|\Z)", instructions, re.DOTALL)
    if not m:
        return ["gphoto.instructions.md has no '**STATUS:' paragraph"]
    status = m.group(0)
    for model in models:
        # The paragraph may write the whole model or just the body designation
        # ("D3300"). A designation only counts as a whole token containing a
        # digit, so "Sony A7 III" is not satisfied by an unrelated "Part III".
        designation = model.split()[-1]
        named = model in status or (
            any(c.isdigit() for c in designation)
            and re.search(r"(?<![A-Za-z0-9])%s(?![A-Za-z0-9])" % re.escape(designation), status)
        )
        if not named:
            failures.append(
                "SUPPORTED-DRIVERS.md lists %r as ConformU-validated but the STATUS paragraph "
                "in .github/instructions/gphoto.instructions.md does not name it" % model
            )
    return failures


def check_gphoto_status_names_validated_bodies():
    return _gphoto_status_findings(
        read("SUPPORTED-DRIVERS.md"),
        read(".github/instructions/gphoto.instructions.md"),
    )


# Check 13 (issue #571): the tier-2 roster in contract_sweep.h names the drivers
# that can connect to an in-process fake. Pinned to the fakes on disk the way
# check 5 pins the blocking-get_connected() list to async_connectable.h.

ROSTER_ROW_RE = re.compile(r'\{\s*"([a-z0-9]+)"\s*,\s*"([a-z0-9]+)"\s*,\s*"(fake_[a-z0-9_]+\.h)"\s*\}')
ROSTER_ARRAY_RE = re.compile(r"kFakeConnectableRoster\[\]\s*=\s*\{(.*?)\n\};", re.DOTALL)

# fake_*.h files that are helpers other fakes and tests build on, not a driver's
# connect path, so they carry no roster row.
HELPER_FAKES = {
    "fake_skywatcher_board.h": "transport-free Sky-Watcher axis model and command handler; the UDP fake in fake_skywatcher_mount.h is the transport the roster connects",
    "fake_bisque_mount.h": "stateful TheSkyX responder plugged into FakeMountServer for the Bisque task-clock cases",
    "fake_pty_write.h": "bounded pty-master write and PtyPair, shared by the pty-backed fakes",
    "fake_raw_decoder.h": "fake RawDecoder seam for the gphoto camera, used together with fake_gphoto_sdk.h",
    "fake_weewx_fetcher.h": "scripted WeeWxFetcher injected through the WeeWX factory overload; no transport of its own to roster",
    "fake_task_clock.h": "virtual-time TaskClock for driver wait tests",
    "fake_zwo_mount.h": "stateful ZWO mount responder plugged into FakeMountServer, motion derived from the shared FakeTaskClock",
}


ROSTER_REGISTRY_ID_RE = re.compile(r"\bX\(\s*([a-z0-9]+_[a-z0-9]+(?:_[a-z0-9]+)*)\s*\)")


def _fake_roster_findings(header_text, disk_fakes, helpers=None):
    helpers = HELPER_FAKES if helpers is None else helpers
    failures = []
    m = ROSTER_ARRAY_RE.search(_strip_comments(header_text))
    rows = ROSTER_ROW_RE.findall(m.group(1)) if m else []
    if not rows:
        return ["AlpacaCore/tests/contract_sweep.h has no kFakeConnectableRoster rows (or the array moved): "
                "the roster cannot be pinned to the fakes on disk"]
    rostered = {fake for _, _, fake in rows}
    # Each row's (vendor, type) must be a pair the registry sweeps: an id is
    # <vendor>_<devicetype>[_<backend>], so the pair is its first two segments.
    registry_pairs = {tuple(i.split("_")[:2]) for i in ROSTER_REGISTRY_ID_RE.findall(_strip_comments(header_text))}
    for vendor, dtype, fake in rows:
        if (vendor, dtype) not in registry_pairs:
            failures.append(
                "kFakeConnectableRoster row {%s, %s, %s} names a (vendor, type) pair with no registry entry: "
                "correct the row or add the X(%s_%s) entry to CONTRACT_SWEEP_ENTRIES" % (vendor, dtype, fake, vendor, dtype))
    for fake in sorted(disk_fakes):
        if fake not in rostered and fake not in helpers:
            failures.append(
                "AlpacaCore/tests/%s is not in kFakeConnectableRoster (AlpacaCore/tests/contract_sweep.h) and is "
                "not a HELPER_FAKES entry in scripts/check_docs_drift.py: add a roster row for the driver it "
                "connects, or name it a helper with a reason" % fake)
    for vendor, dtype, fake in rows:
        if fake not in disk_fakes:
            failures.append(
                "kFakeConnectableRoster row {%s, %s, %s} names a fake that does not exist under "
                "AlpacaCore/tests/: remove or repoint the row" % (vendor, dtype, fake))
    for fake in sorted(helpers):
        if fake in rostered:
            failures.append(
                "STALE HELPER_FAKES entry: %s now has a kFakeConnectableRoster row -- remove it from "
                "HELPER_FAKES in scripts/check_docs_drift.py" % fake)
        if fake not in disk_fakes:
            failures.append(
                "STALE HELPER_FAKES entry: %s does not exist under AlpacaCore/tests/ -- remove it from "
                "HELPER_FAKES in scripts/check_docs_drift.py" % fake)
    return failures


def check_fake_roster_matches_disk(root=ROOT):
    disk = {Path(p).name for p in glob.glob(str(root / "AlpacaCore" / "tests" / "fake_*.h"))}
    return _fake_roster_findings(read("AlpacaCore/tests/contract_sweep.h", root), disk)


STD_REGEX_TEMP_RE = re.compile(r"\bstd::regex\s*[({]")
STD_REGEX_NAMED_RE = re.compile(r"\bstd::regex\s+(?:const\s+)?(\w+)\s*[({=]")


def _static_regex_findings(text, path="AlpacaHTTP/src/http/router.cpp"):
    code = _strip_comments(text)
    findings = []
    seen = 0
    for m in STD_REGEX_TEMP_RE.finditer(code):
        seen += 1
        line = code.count("\n", 0, m.start()) + 1
        findings.append("%s:%d: std::regex constructed as a temporary -- make it a static const "
                        "(a per-request build costs 14x a management request, #646)" % (path, line))
    for m in STD_REGEX_NAMED_RE.finditer(code):
        seen += 1
        # The statement so far: back to the previous ';', '{' or '}'.
        start = max(code.rfind(c, 0, m.start()) for c in ";{}") + 1
        if not re.search(r"\bstatic\b", code[start:m.start()]):
            line = code.count("\n", 0, m.start()) + 1
            findings.append("%s:%d: std::regex %s is not static -- a per-request build costs 14x a "
                            "management request (#646); the gate wants the static keyword and does not model "
                            "scope, so a namespace-scope regex needs it too" % (path, line, m.group(1)))
    if seen == 0:
        findings.append("no std::regex construction found in %s: the extractor is stale or the regexes moved" % path)
    return findings


def check_router_regexes_static(root=ROOT):
    return _static_regex_findings(read("AlpacaHTTP/src/http/router.cpp", root))

# --- check 15: README headline counts vs SUPPORTED-DRIVERS.md (issue #684) --
#
# README.md carries "- **N validated devices. <Word> brands. One server.**
# <brand list>". Check 4 gates only the version badge, so this line was three
# releases stale at 4.0.0 and was moved by hand on the 4.1.0 release PR. N is
# the recount /bump-release Step 2.4 once spelled out as a grep chain; the
# script owns it now and the release recipe reads it through --counts, so
# there is one encoding of the row filter (issue #689). The brand
# count is not the heading count: two SUPPORTED-DRIVERS.md headings collapse
# into "Sky-Watcher" and one README item (the Unihedron SQM-LE, a sensor read
# through the WeeWX driver) has no heading, so the mapping is declared here
# rather than inferred. A new
# `### ` heading with no entry in the map is a finding, which is what a new
# vendor landing without a README mention looks like.

SUPPORTED_HEADING_TO_README_BRAND = {
    "Altair": "Altair",
    "Astroasis": "Astroasis",
    "Celestron": "Celestron",
    "Gemini": "Gemini",
    "GPhoto": "Canon and Nikon DSLRs",  # no library name in user-facing text since 4.1.0 (issue #691)
    "iOptron": "iOptron",
    "OnStep": "OnStep",
    "Player One": "Player One Astronomy",
    "QHY": "QHY",
    "Sky-Watcher Direct Motor Controller": "Sky-Watcher",
    "SynScan V3/V4": "Sky-Watcher",
    "SVBONY": "SVBONY",
    "ToupTek": "ToupTek Astro",
    "WandererAstro": "WandererAstro",
    "WeeWX": "WeeWX",
    "ZWO": "ZWO",
}

# README brand-list items that are not a SUPPORTED-DRIVERS.md heading, each
# with the reason. An item in neither this table nor the map above is drift.
# The reason must be true of the repo as it is: the review of #685 caught the
# first entry claiming a table row that does not exist.
README_BRANDS_WITHOUT_HEADING = {
    "Unihedron SQM-LE (WeeWX plugin)": (
        "a sky-quality sensor the WeeWX ObservingConditions driver reads through the feed's "
        "sqm/sqmTemp fields (SkyQuality/SkyTemperature); it has no driver, table row or heading "
        "of its own in SUPPORTED-DRIVERS.md, and the WeeWX table's only row is the feed itself"),
}

# The brand-count group admits spaces on purpose (issue #690): a multi-word
# count ("Twenty One brands") used to fail the whole regex and the gate said
# "could not find the headline" instead of naming the count; now the line is
# found and _number_word_to_int rejects the word with the specific message.
README_HEADLINE_RE = re.compile(
    r"^- \*\*(\d+) validated devices\. ([A-Za-z][A-Za-z -]*) brands\. One server\.\*\* (.+?)\.?\s*$",
    re.MULTILINE,
)
# Every `### ` heading in SUPPORTED-DRIVERS.md is a vendor today, and this treats
# them all as one deliberately: a new heading that is NOT a vendor (a `### Notes`
# or `### Legend` subsection) fails the gate demanding a map entry. That is the
# strictness the gate is for, not a false drift; give such a heading a different
# level, or add a declared exemption here, rather than loosening the regex.
SUPPORTED_VENDOR_HEADING_RE = re.compile(r"^### (.+?)\s*$", re.MULTILINE)

_ONES = ["zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
         "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen",
         "nineteen"]
_TENS = ["", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"]


def _number_word_to_int(word):
    """'Fifteen' -> 15, 'twenty-one' -> 21; None when it is not a number word.
    Exactly the forms _int_to_number_word emits: a dangling hyphen ('twenty-')
    and a spaced compound ('twenty one') are rejected, not rounded (#690)."""
    w = word.strip().lower()
    if w in _ONES:
        return _ONES.index(w)
    tens, sep, ones = w.partition("-")
    if tens in _TENS[2:]:
        if not sep:
            return _TENS.index(tens) * 10
        if ones in _ONES[1:10]:
            return _TENS.index(tens) * 10 + _ONES.index(ones)
    return None


def count_validated_device_rows(supported):
    """The validated-model recount (the only copy of the rule, issue #689):
    table rows that are not a separator, a header ('Model Series' /
    'Device Type') or a '| Source' row, counted when they carry a check mark.
    /bump-release Step 2.4 gets the number from --counts."""
    n = 0
    for line in supported.splitlines():
        if not line.startswith("| "):
            continue
        if re.match(r"^\| *-", line) or "Model Series" in line or "Device Type" in line or line.startswith("| Source"):
            continue
        if "\u2713" in line:
            n += 1
    return n


def _int_to_number_word(n):
    """15 -> 'fifteen', 21 -> 'twenty-one'; None above 99 (check 15's parser
    stops there too, so a three-digit brand count needs a wider vocabulary in
    both directions, not a digit string)."""
    if not 0 <= n <= 99:
        return None
    if n < 20:
        return _ONES[n]
    tens, ones = divmod(n, 10)
    return _TENS[tens] + ("-" + _ONES[ones] if ones else "")


def _split_readme_brand_list(text, declared=()):
    """Split the README brand list on commas, consuming a DECLARED item whole
    even when it contains a comma (issue #690). `declared` is the set of brand
    strings the gate accounts for (the map's values plus the no-heading table):
    at each position the longest run of comma-separated tokens that spells a
    declared item is taken as one item, so a future "Canon, Nikon and Sony
    DSLRs" is one brand and not two unaccounted ones plus a brand-word
    mismatch. An undeclared item with a comma still splits, and each half is
    then reported as unaccounted, which points at the item itself. The "and "
    that introduces the last item is stripped from the last item only."""
    tokens = [t.strip() for t in text.split(",")]
    tokens = [t for t in tokens if t]
    declared = set(declared)

    def candidate(run, is_last):
        item = ", ".join(run)
        if is_last and item.lower().startswith("and "):
            item = item[4:].strip()
        return item

    items = []
    i = 0
    while i < len(tokens):
        taken = None
        for j in range(len(tokens), i + 1, -1):  # longest run first, single token last
            item = candidate(tokens[i:j], j == len(tokens))
            if item in declared:
                taken = (item, j)
                break
        if taken is None:
            taken = (candidate(tokens[i:i + 1], i + 1 == len(tokens)), i + 1)
        items.append(taken[0])
        i = taken[1]
    return items


def _readme_headline_findings(readme, supported, heading_map=None, extras=None):
    """Check 15 over the two files' text. Pure, so --self-test can drive it."""
    if heading_map is None:
        heading_map = SUPPORTED_HEADING_TO_README_BRAND
    if extras is None:
        extras = README_BRANDS_WITHOUT_HEADING
    failures = []

    m = README_HEADLINE_RE.search(readme)
    if not m:
        return ["could not find the '- **N validated devices. <Word> brands. One server.** ...' headline in README.md"]
    readme_devices = int(m.group(1))
    brand_word = m.group(2)
    items = _split_readme_brand_list(m.group(3), set(heading_map.values()) | set(extras))

    expected_devices = count_validated_device_rows(supported)
    if expected_devices == 0:
        failures.append("SUPPORTED-DRIVERS.md has no validated model rows by count_validated_device_rows -- the row "
                        "filter is stale or the tables changed shape; check 15 cannot count anything")
    elif readme_devices != expected_devices:
        failures.append("README.md says %d validated devices but SUPPORTED-DRIVERS.md has %d validated model rows"
                        % (readme_devices, expected_devices))

    brand_count = _number_word_to_int(brand_word)
    if brand_count is None:
        failures.append("README.md brand count %r is not a number word this check knows (spell it as one word or "
                        "hyphenated, e.g. 'Fifteen' or 'Twenty-one')" % brand_word)
    elif brand_count != len(items):
        failures.append("README.md says %s (%d) brands but its brand list has %d items"
                        % (brand_word, brand_count, len(items)))
    if len(set(items)) != len(items):
        failures.append("README.md brand list repeats an item: %s"
                        % ", ".join(sorted({i for i in items if items.count(i) > 1})))

    headings = []
    for h in SUPPORTED_VENDOR_HEADING_RE.findall(supported):
        if h not in headings:
            headings.append(h)
    if not headings:
        failures.append("SUPPORTED-DRIVERS.md has no '### ' vendor headings -- the heading extractor is stale")
    for h in headings:
        if h not in heading_map:
            failures.append("SUPPORTED-DRIVERS.md heading '### %s' has no entry in SUPPORTED_HEADING_TO_README_BRAND "
                            "(scripts/check_docs_drift.py): a new brand needs its README list item and a map entry" % h)
    for h in heading_map:
        if h not in headings:
            failures.append("SUPPORTED_HEADING_TO_README_BRAND names '### %s' but SUPPORTED-DRIVERS.md has no such "
                            "heading -- remove or rename the map entry" % h)

    # Only the brands of headings that still exist count as accounted for, so a
    # dropped vendor reports its stale map entry AND its orphaned README item in
    # one run rather than two.
    mapped = {brand for h, brand in heading_map.items() if h in headings}
    for h, brand in heading_map.items():
        if h in headings and brand not in items:
            failures.append("README.md brand list does not name %r (SUPPORTED-DRIVERS.md heading '### %s')" % (brand, h))
    for brand in extras:
        if brand not in items:
            failures.append("README_BRANDS_WITHOUT_HEADING names %r but the README.md brand list does not -- "
                            "remove the entry or restore the item" % brand)
    for item in items:
        if item not in mapped and item not in extras:
            failures.append("README.md brand list item %r matches no SUPPORTED-DRIVERS.md heading and is not declared "
                            "in README_BRANDS_WITHOUT_HEADING" % item)
    return failures


def check_readme_headline_counts(root=ROOT):
    return _readme_headline_findings(read("README.md", root), read("SUPPORTED-DRIVERS.md", root))


def _expected_readme_brands(supported, heading_map, extras):
    """The README brand items check 15 accounts for: one per mapped heading
    that exists in SUPPORTED-DRIVERS.md, in heading order and de-duplicated,
    then the declared no-heading items."""
    brands = []
    for h in SUPPORTED_VENDOR_HEADING_RE.findall(supported):
        brand = heading_map.get(h)
        if brand is not None and brand not in brands:
            brands.append(brand)
    for brand in extras:
        if brand not in brands:
            brands.append(brand)
    return brands


def readme_headline_counts_summary(readme, supported, heading_map=None, extras=None):
    """The --counts report: check 15's numbers computed by check 15's own
    functions, plus a paste-ready headline. Pure, so --self-test drives it.

    The headline keeps the README's current item order for items that stay
    and appends new ones at the end, so a release diff shows the additions
    and not a reorder. A heading with no map entry, and a map entry whose
    heading is gone, are each named rather than silently left out of the
    list: the full check fails on both, so a pasted headline that hid either
    would still leave check 15 red (review of #694)."""
    if heading_map is None:
        heading_map = SUPPORTED_HEADING_TO_README_BRAND
    if extras is None:
        extras = README_BRANDS_WITHOUT_HEADING
    devices = count_validated_device_rows(supported)
    expected = _expected_readme_brands(supported, heading_map, extras)
    m = README_HEADLINE_RE.search(readme)
    current = [i for i in _split_readme_brand_list(m.group(3), expected) if i in expected] if m else []
    ordered = current + [b for b in expected if b not in current]
    word = _int_to_number_word(len(ordered))
    word_shown = word.capitalize() if word else "%d (no number word for this count)" % len(ordered)
    if len(ordered) > 1:
        listed = ", ".join(ordered[:-1]) + ", and " + ordered[-1]
    else:
        listed = ", ".join(ordered)
    lines = [
        "validated devices: %d" % devices,
        "brands: %d (%s)" % (len(ordered), word_shown),
        "brand list: %s" % ", ".join(ordered),
        "headline: - **%d validated devices. %s brands. One server.** %s." % (devices, word_shown, listed),
        "README.md now: %s" % (m.group(0).strip() if m else "(headline not found)"),
    ]
    headings = SUPPORTED_VENDOR_HEADING_RE.findall(supported)
    for h in dict.fromkeys(h for h in headings if h not in heading_map):
        lines.append("unmapped heading '### %s': add it to SUPPORTED_HEADING_TO_README_BRAND and to the README list" % h)
    for h in heading_map:
        if h not in headings:
            lines.append("stale map entry '### %s': SUPPORTED-DRIVERS.md has no such heading; remove or rename it in "
                         "SUPPORTED_HEADING_TO_README_BRAND (check 15 fails on it)" % h)
    return lines


def print_counts(root=ROOT):
    for line in readme_headline_counts_summary(read("README.md", root), read("SUPPORTED-DRIVERS.md", root)):
        print(line)
    return 0



# --- check 16: SUPPORTED-DRIVERS.md Updated date vs README release date (#692)

SUPPORTED_UPDATED_RE = re.compile(r"^## Updated (\S+)\s*$", re.MULTILINE)
# How far past the badge date the Updated line may run. Releases are weeks
# apart, so a real gap never approaches this; a typo'd year ("2126-09-27")
# overshoots it at once. Relative to the badge, not to today, so the check
# stays pure.
MAX_UPDATED_DAYS_AHEAD = 366
README_BADGE_DATE_RE = re.compile(
    r"^####\s*\[" + VERSION_PATTERN + r"\]\s*-\s*(?P<date>\S+)\s*&middot;\s*\[Changelog\]", re.MULTILINE)


def _iso_date(text):
    """datetime.date for a YYYY-MM-DD string, None for anything else."""
    import datetime
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", text):
        return None
    try:
        return datetime.date.fromisoformat(text)
    except ValueError:
        return None


def _updated_date_findings(supported, readme):
    """Check 16 over the two files' text. Pure, so --self-test can drive it."""
    failures = []
    m = SUPPORTED_UPDATED_RE.search(supported)
    if not m:
        return ["could not find the '## Updated YYYY-MM-DD' line in SUPPORTED-DRIVERS.md"]
    updated = _iso_date(m.group(1))
    if updated is None:
        failures.append("SUPPORTED-DRIVERS.md '## Updated %s' is not a YYYY-MM-DD date" % m.group(1))
    b = README_BADGE_DATE_RE.search(readme)
    if not b:
        failures.append("could not find the release date in the README.md version badge line")
        return failures
    released = _iso_date(b.group("date"))
    if released is None:
        failures.append("README.md badge date %r is not a YYYY-MM-DD date" % b.group("date"))
    elif updated is not None and updated < released:
        failures.append("SUPPORTED-DRIVERS.md says '## Updated %s' but README.md was released %s: set the Updated "
                        "line to the release date (/bump-release Step 2.5) or later" % (m.group(1), b.group("date")))
    elif updated is not None and (updated - released).days > MAX_UPDATED_DAYS_AHEAD:
        failures.append("SUPPORTED-DRIVERS.md says '## Updated %s', more than %d days after the README.md release "
                        "date %s: a mistyped year, or a release is long overdue"
                        % (m.group(1), MAX_UPDATED_DAYS_AHEAD, b.group("date")))
    return failures


def check_supported_drivers_updated_date(root=ROOT):
    return _updated_date_findings(read("SUPPORTED-DRIVERS.md", root), read("README.md", root))


# --- check 17: documented AlpacaError values vs alpaca_errors.h ---------------

ALPACA_ERRORS_H = "AlpacaCore/include/alpacacore/alpaca_errors.h"
ERROR_CODE_DOCS = ("AGENTS.md", ".github/instructions/ascom-contract.instructions.md",
                   ".github/instructions/testing.instructions.md", ".claude/commands/driver-build.md")
# The four docs carry ~20 pins today (AGENTS.md none); a floor well under that catches an
# extractor that stops matching without tripping on an ordinary rewording.
MIN_ERROR_CODE_PINS = 8

_ALPACA_ERROR_NS_RE = re.compile(r"namespace\s+AlpacaError\s*\{(.*?)\}", re.DOTALL)
_ALPACA_ERROR_DEF_RE = re.compile(r"constexpr\s+int\s+([A-Za-z]\w*)\s*=\s*(0[xX][0-9A-Fa-f]+|\d+)\s*;")
# An error name as the docs write it: CamelCase, optionally backticked and
# optionally qualified. CamelCase only, so a protocol constant such as
# MC_AUX_GUIDE 0x26 is never read as a pin.
_EC_NAME = r"(?<!\w)`?(?:alpacacore::)?(?:AlpacaError::)?([A-Z][a-z]+(?:[A-Z][a-z]*)+)`?"
_EC_HEX = r"(0[xX][0-9A-Fa-f]+)\b"
# `Name` (0xNNN ...)
_EC_PAREN_RE = re.compile(_EC_NAME + r"\s*\(\s*" + _EC_HEX)
# Name `0xNNN`
_EC_TICK_RE = re.compile(_EC_NAME + r"\s+`" + _EC_HEX + r"`")
# `A` (or `B` for a property, both 0xNNN)
_EC_BOTH_RE = re.compile(_EC_NAME + r"\s*\(\s*or\s+" + _EC_NAME + r"[^)]*?\bboth\s+" + _EC_HEX)
# | `Name` | ... (0xNNN ... |  -- an exception-table row: every value in the
# row belongs to the row's name, whatever other names the cell mentions.
_EC_ROW_RE = re.compile(r"^\|\s*`([A-Z][A-Za-z]+)`\s*\|(.*)$")
_EC_ROW_VALUE_RE = re.compile(r"\(\s*" + _EC_HEX)


def _alpaca_error_values(header_text):
    """{name: int} for every constant inside `namespace AlpacaError { ... }`."""
    m = _ALPACA_ERROR_NS_RE.search(header_text)
    if not m:
        return {}
    return {name: int(value, 0) for name, value in _ALPACA_ERROR_DEF_RE.findall(m.group(1))}


def _error_code_pins(text):
    """(line number, name, hex text) for every error value the doc writes down."""
    pins = []
    for lineno, line in enumerate(text.splitlines(), 1):
        row = _EC_ROW_RE.match(line)
        if row:
            pins.extend((lineno, row.group(1), h) for h in _EC_ROW_VALUE_RE.findall(row.group(2)))
            continue
        for m in _EC_BOTH_RE.finditer(line):
            pins.append((lineno, m.group(1), m.group(3)))
            pins.append((lineno, m.group(2), m.group(3)))
        for regex in (_EC_PAREN_RE, _EC_TICK_RE):
            pins.extend((lineno, m.group(1), m.group(2)) for m in regex.finditer(line))
    return pins


def _error_code_findings(header_text, docs, min_pins=MIN_ERROR_CODE_PINS):
    """Check 17 over the header and the docs' text ({path: text}). Pure, so
    --self-test can drive it."""
    values = _alpaca_error_values(header_text)
    if not values:
        return ["found no AlpacaError constants in %s: the parser is stale or the namespace moved" % ALPACA_ERRORS_H]
    failures = []
    total = 0
    for path, text in docs.items():
        for lineno, name, hex_text in _error_code_pins(text):
            total += 1
            if name not in values:
                failures.append("%s:%d: `%s` (%s) is not an AlpacaError constant in %s (known: %s)"
                                % (path, lineno, name, hex_text, ALPACA_ERRORS_H, ", ".join(sorted(values))))
            elif int(hex_text, 16) != values[name]:
                failures.append("%s:%d: `%s` is written as %s but %s defines it as 0x%X"
                                % (path, lineno, name, hex_text, ALPACA_ERRORS_H, values[name]))
    if total < min_pins:
        failures.append("found only %d documented error-code value(s) across %s (floor %d): the extractor is "
                        "stale or the docs stopped writing the values down" % (total, ", ".join(docs), min_pins))
    return failures


def check_error_codes_match_header(root=ROOT):
    return _error_code_findings(read(ALPACA_ERRORS_H, root), {p: read(p, root) for p in ERROR_CODE_DOCS})


# --- check 18: CI job roster vs ci.yml --------------------------------------

CI_ROSTER_DOC = ".github/instructions/ci-gates.instructions.md"
CI_ROSTER_HEADING = "## Continuous Integration and Pre-flight"
CI_ROSTER_BULLET = "- CI ("
_CI_JOBS_BLOCK_RE = re.compile(r"^jobs:[ \t]*\n(.*?)(?=^\S|\Z)", re.MULTILINE | re.DOTALL)
_CI_JOB_ID_RE = re.compile(r"^  ([A-Za-z0-9_-]+):[ \t]*$", re.MULTILINE)
_BACKTICK_RE = re.compile(r"`([^`]+)`")


def _ci_job_ids(ci_text):
    """The top-level job ids under `jobs:`, in file order (not the `on:` keys)."""
    m = _CI_JOBS_BLOCK_RE.search(ci_text)
    return _CI_JOB_ID_RE.findall(m.group(1)) if m else []


def _strip_parenthesized(text):
    """text with every (possibly nested) parenthesised group removed."""
    out, depth = [], 0
    for ch in text:
        if ch == "(":
            depth += 1
        elif ch == ")" and depth:
            depth -= 1
        elif not depth:
            out.append(ch)
    return "".join(out)


def _ci_roster_bullet(agents_text):
    """The first bullet under the CI heading that starts with `- CI (`,
    continuation lines included, or None."""
    start = agents_text.find("\n" + CI_ROSTER_HEADING + "\n")
    if start == -1:
        return None
    section_end = agents_text.find("\n## ", start + 1)
    section = agents_text[start:section_end if section_end != -1 else len(agents_text)]
    lines = section.splitlines()
    for i, line in enumerate(lines):
        if line.startswith(CI_ROSTER_BULLET):
            bullet = [line]
            for more in lines[i + 1:]:
                if not more.startswith("  "):
                    break
                bullet.append(more)
            return "\n".join(bullet)
    return None


def _ci_roster_findings(ci_text, agents_text):
    """Check 18 over the two files' text. Pure, so --self-test can drive it.

    The roster is every backticked token in the bullet outside parentheses:
    the descriptions (the ci.yml path, the `[stress]` tags) sit in parentheses
    after each id, so they are not read as job ids.
    """
    jobs = _ci_job_ids(ci_text)
    if not jobs:
        return ["found no job ids under `jobs:` in .github/workflows/ci.yml: the parser is stale"]
    bullet = _ci_roster_bullet(agents_text)
    if bullet is None:
        return ["could not find the '%s...' job roster bullet under '%s' in %s"
                % (CI_ROSTER_BULLET, CI_ROSTER_HEADING, CI_ROSTER_DOC)]
    named = _BACKTICK_RE.findall(_strip_parenthesized(bullet))
    failures = []
    seen = set()
    for job in named:
        if job in seen:
            failures.append("%s CI roster names '%s' twice" % (CI_ROSTER_DOC, job))
        seen.add(job)
        if job not in jobs:
            failures.append("%s CI roster names '%s', which does not exist as a job id in "
                            ".github/workflows/ci.yml (ids: %s)" % (CI_ROSTER_DOC, job, ", ".join(jobs)))
    for job in jobs:
        if job not in seen:
            failures.append("ci.yml job '%s' is missing from the %s CI roster bullet ('%s...')"
                            % (job, CI_ROSTER_DOC, CI_ROSTER_BULLET))
    return failures


def check_ci_roster_matches_workflow(root=ROOT):
    return _ci_roster_findings(read(".github/workflows/ci.yml", root), read(CI_ROSTER_DOC, root))


# Check 19: a vendored `.so` committed twice as regular files. Matches
# `libfoo.so` and `libfoo.so.1.2.3`, not `libfoo.a` or `libfoo.so.txt`.
_VENDORED_SO_RE = re.compile(r"^lib[^/]*\.so(\.\d+)*$")
_GIT_EMPTY_BLOB = "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391"
VENDORED_SO_ROOT = "AlpacaCore/external/"


def _duplicate_so_findings(entries):
    """Check 19 over `git ls-files -s` entries as (mode, blob, path). Pure, so
    --self-test can drive it."""
    libs = [(mode, blob, path) for mode, blob, path in entries
            if path.startswith(VENDORED_SO_ROOT) and _VENDORED_SO_RE.match(path.rsplit("/", 1)[-1])]
    if not libs:
        return ["found no vendored lib*.so under %s in the git index: the reader is stale" % VENDORED_SO_ROOT]
    groups = {}
    for mode, blob, path in libs:
        if mode in ("100644", "100755") and blob != _GIT_EMPTY_BLOB:
            groups.setdefault((path.rsplit("/", 1)[0], blob), []).append(path)
    failures = []
    for (_, _), paths in sorted(groups.items()):
        if len(paths) > 1:
            paths = sorted(paths, key=len)
            target = paths[-1].rsplit("/", 1)[-1]
            failures.append("%s are identical regular files; keep %s and make %s symlink(s) to it, "
                            "or debian/rules' `cp -a` ships every copy"
                            % (", ".join(paths), target,
                               ", ".join(q.rsplit("/", 1)[-1] for q in paths[:-1])))
    return failures


def check_vendored_so_not_duplicated(root=ROOT):
    out = _run_git(["ls-files", "-s", "--", VENDORED_SO_ROOT], root=root).stdout
    entries = []
    for line in out.splitlines():
        meta, _, path = line.partition("\t")
        mode, blob, _stage = meta.split()
        entries.append((mode, blob, path))
    return _duplicate_so_findings(entries)


# Check 20: a restore-keys prefix that also matches another cache's keys.
_CACHE_KEY_RE = re.compile(r"^\s*key:\s*(\S.*?)\s*$")
_RESTORE_KEYS_RE = re.compile(r"^(\s*)restore-keys:\s*(.*?)\s*$")


def _static_prefix(value):
    """A cache key's text before its first `${{` expression, unquoted."""
    return value.strip().strip("'\"").split("${{", 1)[0]


def _cache_prefix_findings(workflows):
    """Check 20 over {workflow path: text}. Pure, so --self-test can drive it."""
    keys = {}       # prefix -> first "path:line" that saves or restores it
    restores = []   # (prefix, "path:line")
    for path, text in sorted(workflows.items()):
        lines = text.splitlines()
        for n, line in enumerate(lines, 1):
            m = _CACHE_KEY_RE.match(line)
            if m:
                keys.setdefault(_static_prefix(m.group(1)), "%s:%d" % (path, n))
                continue
            m = _RESTORE_KEYS_RE.match(line)
            if not m:
                continue
            values = [m.group(2)] if m.group(2) not in ("", "|", ">") else []
            if not values:
                for more in lines[n:]:
                    if more.strip() and len(more) - len(more.lstrip()) <= len(m.group(1)):
                        break
                    if more.strip():
                        values.append(more)
            restores.extend((_static_prefix(v), "%s:%d" % (path, n)) for v in values)
    if not keys:
        return ["found no cache `key:` in .github/workflows/: the reader is stale"]
    failures = []
    for prefix, where in restores:
        for other, other_where in sorted(keys.items()):
            if prefix and other != prefix and other.startswith(prefix):
                failures.append("%s restores by the prefix '%s', which also matches the '%s' keys saved at %s: "
                                "a restore takes the newest match, so this job can get that cache; give one of "
                                "them a prefix the other does not start with" % (where, prefix, other, other_where))
    return failures


def check_cache_restore_prefixes_distinct(root=ROOT):
    paths = sorted(glob.glob(str(Path(root) / ".github/workflows/*.yml")))
    return _cache_prefix_findings({Path(p).relative_to(root).as_posix(): Path(p).read_text(encoding="utf-8")
                                   for p in paths})



# Check 21: AGENTS.md size budget.
AGENTS_MD_MAX_BYTES = 15000


def _agents_md_size_findings(size, limit=AGENTS_MD_MAX_BYTES):
    if size > limit:
        return ["AGENTS.md is %d bytes, over its %d-byte budget: move the rule into the matching shared topic "
                "file under .github/instructions/ (see docs/agent-instructions.md) and leave a one-line "
                "pointer in AGENTS.md" % (size, limit)]
    return []


def check_agents_md_size(root=ROOT):
    path = Path(root) / "AGENTS.md"
    if not path.is_file():
        return ["AGENTS.md is missing"]
    return _agents_md_size_findings(len(path.read_bytes()))


# --- checks 22 and 23: decision records ---------------------------------------
#
# The decision records in docs/decisions/ are what the planned decision-record
# gate compares a pull request against, so their shape has to be machine
# readable and their adoption rosters have to agree with the code. Before
# these checks the rosters lived in prose in five places and three of them
# disagreed with the code (PR #1410).

DECISION_DIR = "docs/decisions"
DECISION_FILE_RE = re.compile(r"^(\d{4})-[a-z0-9]+(?:-[a-z0-9]+)*\.md$")
DECISION_STATUS_RE = re.compile(r"^Status: (proposed|accepted|superseded by (\d{4})|retired \(#\d+\))$")
DECISION_SECTIONS = ("Context", "Decision", "Rules", "Alternatives rejected", "Consequences", "Links")
# Sections a record may add between the required ones; anything else is a typo
# or a new convention that README.md has to describe first.
DECISION_OPTIONAL_SECTIONS = ("Adoption", "How to reverse")
DECISION_RULE_RE = re.compile(
    r"^(?P<n>\d+)\. (?P<text>.+?) Scope: (?P<scope>`[^`]+`(?:, `[^`]+`)*)\. Checked by: (?P<check>.+?)\."
    r"(?P<allowed> <!-- allowed -->)?$")
DECISION_INDEX_LINK_RE = re.compile(r"^- \[[^\]]+\]\((\d{4}-[^)]+\.md)\)( \(retired\))?$", re.M)
# A rule's bound is part of the rule; a carve-out for code that deviates is a
# defect with an issue, never a clause (README.md, "Rules"). A rule whose bound
# needs one of these words ends with ` <!-- allowed -->` (DECISION_RULE_RE's
# `allowed` group, so check_adr_scope.py reads a marked rule as any other).
DECISION_EXCEPTION_WORD_RE = re.compile(r"\b(except|unless|until|for now)\b", re.I)
# `Checked by: review` names no evidence, so the bot has nothing to apply. New
# rules write `review (<what to look for>)`; the records below keep the bare
# form they had when the form changed, and this baseline may only fall: lower
# a record's figure (or drop the record) in the PR that converts its rules.
DECISION_BARE_REVIEW_BASELINE = {
    "0001-skywatcher-pointing-clock.md": 1,
    "0002-server-thread-ownership.md": 1,
    "0003-docs-drift-gates.md": 1,
    "0005-task-clock.md": 4,
    "0006-async-operation-ownership.md": 3,
    "0007-lan-surface-threat-model.md": 13,
    "0009-link-loss-and-relink-policy.md": 0,
    "0011-cgx-logical-reference-continuity.md": 4,
    "0012-bundled-libgphoto2.md": 1,
    "0013-designs-to-avoid.md": 7,
    "0014-utc-offset.md": 2,
    "0015-sdk-seams.md": 4,
    "0016-getters-never-wait-on-device-io.md": 0,
    "0017-serial-port-ownership.md": 8,
    "0019-connect-without-site.md": 1,
}
# The negative catalogue's rules have no natural file scope, so it alone may
# write `Scope: `**``; anywhere else that scope marks a rule that belongs in
# the catalogue or in a topic file.
DECISION_ANY_SCOPE_RECORDS = ("0013-designs-to-avoid.md",)
# Records from this number on say how to undo themselves (README.md, "How to
# reverse"); the earlier ones add the section when next touched.
DECISION_REVERSE_REQUIRED_FROM = 20
ADOPTION_STATES = ("adopted", "partial", "planned")
ISSUE_REF_RE = re.compile(r"#\d+")
CODE_SPAN_ONLY_RE = re.compile(r"`([^`]+)`")


def _glob_match(path, pattern):
    # One glob rule for the repo: a single star stays inside one directory,
    # ** crosses them. The instruction-structure check owns it.
    from check_instruction_structure import matches
    return matches(path, pattern)


def _decision_sections(text):
    """[(title, body)] for every `## ` section, in order."""
    sections = []
    current = None
    for line in text.splitlines():
        if line.startswith("## "):
            current = [line[3:].strip(), []]
            sections.append(current)
        elif current is not None:
            current[1].append(line)
    return [(title, "\n".join(body)) for title, body in sections]


def _decision_record_findings(records, readme, tracked, baseline=DECISION_BARE_REVIEW_BASELINE,
                              reverse_required_from=DECISION_REVERSE_REQUIRED_FROM):
    """records: {file name: text}; readme: docs/decisions/README.md text;
    tracked: repo-relative paths the Rules scope globs are matched against;
    baseline: {file name: bare `Checked by: review` rules still allowed};
    reverse_required_from: first record number that needs `## How to reverse`."""
    findings = []
    numbers = []
    retired = set()
    superseded_by = {}   # superseding record number -> the record it replaces
    has_reverse = {}     # record number -> whether it has `## How to reverse`
    for name in sorted(records):
        text = records[name]
        m = DECISION_FILE_RE.match(name)
        if not m:
            findings.append("%s/%s: the name is not NNNN-<lowercase-slug>.md" % (DECISION_DIR, name))
            continue
        numbers.append(int(m.group(1)))
        lines = text.splitlines()
        where = "%s/%s" % (DECISION_DIR, name)
        if not lines or not lines[0].startswith("# "):
            findings.append("%s: the first line is not a `# ` title" % where)
        status = DECISION_STATUS_RE.match(lines[2]) if len(lines) > 2 else None
        if not status:
            findings.append("%s: line 3 is not `Status: proposed`, `Status: accepted`, "
                            "`Status: superseded by NNNN` or `Status: retired (#NNNN)`" % where)
            continue
        if status.group(2) and not any(n.startswith(status.group(2) + "-") for n in records):
            findings.append("%s: superseded by %s, which does not exist" % (where, status.group(2)))
        if status.group(2):
            superseded_by.setdefault(status.group(2), name)
        is_retired = status.group(1).startswith("retired")
        if is_retired:
            retired.add(name)
        no_rules = bool(status.group(2)) or is_retired
        sections = _decision_sections(text)
        titles = [t for t, _ in sections]
        unknown = [t for t in titles if t not in DECISION_SECTIONS + DECISION_OPTIONAL_SECTIONS]
        for t in unknown:
            findings.append("%s: unknown section `## %s` (README.md lists the allowed ones)" % (where, t))
        has_reverse[m.group(1)] = "How to reverse" in titles
        if is_retired:
            for t in ("Rules", "Adoption"):
                if t in titles:
                    findings.append("%s: a retired record drops its `## %s` section" % (where, t))
        elif int(m.group(1)) >= reverse_required_from and "How to reverse" not in titles:
            findings.append("%s: a record from %04d on needs a `## How to reverse` section"
                            % (where, reverse_required_from))
        required = [t for t in DECISION_SECTIONS if not (t == "Rules" and no_rules)]
        present = [t for t in titles if t in required]
        for t in required:
            if t not in titles:
                findings.append("%s: missing section `## %s`" % (where, t))
        if present != [t for t in required if t in present]:
            findings.append("%s: sections are out of order; expected %s" % (where, ", ".join(required)))
        for t in set(titles):
            if titles.count(t) > 1:
                findings.append("%s: section `## %s` appears %d times" % (where, t, titles.count(t)))
        rules = dict(sections).get("Rules")
        if rules is None or is_retired:
            continue
        expected = 1
        bare_review = 0
        for line in rules.splitlines():
            if not re.match(r"^\d+\. ", line):
                continue
            rule = DECISION_RULE_RE.match(line)
            if not rule:
                findings.append("%s: rule line does not end `Scope: `glob`[, `glob`]. Checked by: <check>.`: %s"
                                % (where, line[:80]))
                expected += 1
                continue
            if int(rule.group("n")) != expected:
                findings.append("%s: rule %s is out of sequence (expected %d)" % (where, rule.group("n"), expected))
            expected = int(rule.group("n")) + 1
            word = DECISION_EXCEPTION_WORD_RE.search(rule.group("text"))
            if word and not rule.group("allowed"):
                findings.append("%s: rule %s says `%s`; a carve-out is a defect with an issue, not a rule clause "
                                "(end the line with `<!-- allowed -->` when the bound is the rule)"
                                % (where, rule.group("n"), word.group(1)))
            if rule.group("check") == "review":
                bare_review += 1
            for glob_pattern in CODE_SPAN_ONLY_RE.findall(rule.group("scope")):
                if glob_pattern == "**" and name not in DECISION_ANY_SCOPE_RECORDS:
                    findings.append("%s: rule %s scope `**` is allowed only in %s; name the files or move "
                                    "the rule" % (where, rule.group("n"), ", ".join(DECISION_ANY_SCOPE_RECORDS)))
                elif not any(_glob_match(p, glob_pattern) for p in tracked):
                    findings.append("%s: rule %s scope `%s` matches no tracked file"
                                    % (where, rule.group("n"), glob_pattern))
        if expected == 1:
            findings.append("%s: `## Rules` has no numbered rule" % where)
        allowed_bare = baseline.get(name, 0)
        if bare_review > allowed_bare:
            findings.append("%s: %d rules say a bare `Checked by: review` but the ratchet allows %d; write "
                            "`review (<what to look for>)`" % (where, bare_review, allowed_bare))
        elif bare_review < allowed_bare:
            findings.append("%s: %d bare `Checked by: review` rules remain but DECISION_BARE_REVIEW_BASELINE "
                            "allows %d; lower it so the ratchet holds" % (where, bare_review, allowed_bare))
    # README.md: a record that supersedes another says how to reverse, whatever
    # its number (the number cut-over above covers only new records).
    for number, old in sorted(superseded_by.items()):
        if has_reverse.get(number) is False and int(number) < reverse_required_from:
            new = next(n for n in records if n.startswith(number + "-"))
            findings.append("%s/%s: supersedes %s and needs a `## How to reverse` section"
                            % (DECISION_DIR, new, old))
    numbers.sort()
    if numbers and numbers != list(range(1, len(numbers) + 1)):
        findings.append("%s: record numbers are not 0001..%04d with no gap or repeat: %s"
                        % (DECISION_DIR, len(numbers), ", ".join("%04d" % n for n in numbers)))
    index_lines = DECISION_INDEX_LINK_RE.findall(readme)
    indexed = [n for n, _ in index_lines]
    for n, suffix in index_lines:
        if bool(suffix) != (n in retired):
            findings.append("%s/README.md: the index line for %s %s the `(retired)` suffix"
                            % (DECISION_DIR, n, "lacks" if n in retired else "carries"))
    record_names = sorted(n for n in records if DECISION_FILE_RE.match(n))
    if indexed != record_names:
        missing = sorted(set(record_names) - set(indexed))
        extra = sorted(set(indexed) - set(record_names))
        detail = []
        if missing:
            detail.append("not indexed: " + ", ".join(missing))
        if extra:
            detail.append("indexed but absent: " + ", ".join(extra))
        if not detail:
            detail.append("index order differs from the record numbers")
        findings.append("%s/README.md: the index does not list every record once, in order (%s)"
                        % (DECISION_DIR, "; ".join(detail)))
    return findings


def check_decision_records(root=ROOT):
    directory = Path(root) / DECISION_DIR
    records = {p.name: p.read_text(encoding="utf-8", errors="replace")
               for p in directory.glob("*.md") if p.name != "README.md"}
    if not records:
        return ["%s holds no decision record: the directory moved or the glob regressed" % DECISION_DIR]
    tracked, _ = _tracked_paths(Path(root))
    return _decision_record_findings(records, read("%s/README.md" % DECISION_DIR, Path(root)), tracked)


def _adoption_findings(where, body, files):
    """One record's `## Adoption` section against the files it names.

    files: {repo path: text} for every tracked file (the Scan glob and the
    Source cells are resolved against it). Comments are blanked before a
    symbol is matched, so a comment that names one is not adoption."""
    findings = []
    code = {}

    def code_of(path):
        if path not in code:
            code[path] = _strip_comments(files[path])
        return code[path]

    scan = re.search(r"^Scan: `([^`]+)`$", body, re.M)
    symbol = re.search(r"^Symbol: `([^`]+)`$", body, re.M)
    if not scan:
        return ["%s: `## Adoption` has no `Scan: `glob`` line" % where]
    table = [l for l in body.splitlines() if l.startswith("|")]
    if len(table) < 3:
        return ["%s: `## Adoption` has no table rows" % where]
    header = [c.strip() for c in table[0].strip("|").split("|")]
    for column in ("Driver", "Source", "State", "Issue"):
        if column not in header:
            return ["%s: the Adoption table has no `%s` column" % (where, column)]
    if not symbol and "Symbol" not in header:
        return ["%s: `## Adoption` names no `Symbol: `name`` line and has no Symbol column" % where]
    listed = set()
    for row in table[2:]:
        cells = [c.strip() for c in row.strip("|").split("|")]
        if len(cells) != len(header):
            findings.append("%s: Adoption row has %d cells, the header %d: %s" % (where, len(cells), len(header), row[:60]))
            continue
        cell = dict(zip(header, cells))
        name = cell["Driver"]
        state = cell["State"]
        row_symbol = CODE_SPAN_ONLY_RE.findall(cell.get("Symbol", ""))
        word = row_symbol[0] if row_symbol else (symbol.group(1) if symbol else None)
        if not word:
            findings.append("%s: Adoption row %s has no symbol" % (where, name))
            continue
        if state not in ADOPTION_STATES:
            findings.append("%s: Adoption row %s has state %r; use one of %s"
                            % (where, name, state, ", ".join(ADOPTION_STATES)))
            continue
        if state == "partial" and not ISSUE_REF_RE.search(cell["Issue"]):
            findings.append("%s: Adoption row %s is partial but names no issue that finishes it" % (where, name))
        sources = CODE_SPAN_ONLY_RE.findall(cell["Source"])
        if not sources:
            if state != "planned" or cell["Source"] not in ("—", "-"):
                findings.append("%s: Adoption row %s names no source file (only a planned row may say —)"
                                % (where, name))
            continue
        pattern = re.compile(r"\b%s\b" % re.escape(word))
        uses = False
        for source in sources:
            listed.add(source)
            if source not in files:
                findings.append("%s: Adoption row %s names %s, which is not a tracked AlpacaCore/ or AlpacaHTTP/ "
                                ".cpp or .h file (the files check 23 reads)" % (where, name, source))
                continue
            uses = uses or bool(pattern.search(code_of(source)))
        if state in ("adopted", "partial") and not uses:
            findings.append("%s: Adoption row %s is %s but no source names `%s`" % (where, name, state, word))
        if state == "planned" and uses:
            findings.append("%s: Adoption row %s is planned but its source already names `%s`; "
                            "mark it partial or adopted" % (where, name, word))
    scanned = [p for p in files if _glob_match(p, scan.group(1))]
    if not scanned:
        findings.append("%s: Adoption scan `%s` matches no tracked file" % (where, scan.group(1)))
    for path in sorted(scanned):
        if path in listed:
            continue
        if symbol and not re.search(r"\b%s\b" % re.escape(symbol.group(1)), code_of(path)):
            continue
        findings.append("%s: %s %s but has no Adoption row"
                        % (where, path, ("names `%s`" % symbol.group(1)) if symbol else "is in the scan"))
    return findings


def check_decision_adoption(root=ROOT):
    root = Path(root)
    tracked, _ = _tracked_paths(root)
    files = {}
    for path in tracked:
        if path.startswith(("AlpacaCore/", "AlpacaHTTP/")) and not path.startswith("AlpacaCore/external/") \
                and path.endswith((".cpp", ".h")):
            full = root / path
            if full.is_file():
                files[path] = full.read_text(encoding="utf-8", errors="replace")
    findings = []
    adopting = 0
    for record in sorted((root / DECISION_DIR).glob("0*.md")):
        body = dict(_decision_sections(record.read_text(encoding="utf-8", errors="replace"))).get("Adoption")
        if body is None:
            continue
        adopting += 1
        findings.extend(_adoption_findings("%s/%s" % (DECISION_DIR, record.name), body, files))
    if adopting < MIN_ADOPTION_RECORDS:
        findings.append("only %d decision record(s) carry an Adoption section (floor %d): a section was renamed "
                        "or the parser regressed" % (adopting, MIN_ADOPTION_RECORDS))
    return findings


# Records 0005, 0006, 0009, 0010, 0014 and 0015 carry an Adoption table.
MIN_ADOPTION_RECORDS = 6


# --- check 24: review job time budget ------------------------------------------

REVIEW_WORKFLOW = ".github/workflows/claude-review.yml"
REVIEW_GATES_DOC = ".github/instructions/ci-gates.instructions.md"
REVIEW_SKILLS = (".claude/commands/pr-checker.md", ".claude/commands/submit-pr.md")


def _review_budget_findings(workflow, gates, skills):
    """Check 24 over the texts ({skill path: text} for skills). Pure, so --self-test can drive it.

    The workflow owns two figures: the review job's `timeout-minutes` and the
    `N * 60` deadline of its wait step. The ci-gates line and both skills
    restate them (wait minutes, job minutes, poll budget), and the poll has to
    outlast the job or a healthy run reads as a stall.
    """
    findings = []
    job = re.search(r"^  review:\n(?:(?!^  \S).*\n)*?    timeout-minutes: (\d+)", workflow, re.M)
    wait = re.search(r"\+ (\d+) \* 60 \)\)", workflow)
    if not job or not wait:
        return ["%s: could not read the review job's timeout-minutes or the wait step's `N * 60` deadline: "
                "the reader is stale" % REVIEW_WORKFLOW]
    job_min, wait_min = int(job.group(1)), int(wait.group(1))

    def figures(path, text, pattern, what, scale=1):
        found = [int(m.group(1)) // scale for m in re.finditer(pattern, text)]
        if not found:
            findings.append("%s: found no %s: the reader is stale" % (path, what))
        return [(path, n) for n in found]

    polls = []
    expected = []
    flat = {path: re.sub(r"\s+", " ", text) for path, text in [(REVIEW_GATES_DOC, gates)] + sorted(skills.items())}
    g = flat[REVIEW_GATES_DOC]
    expected += [(n, "wait", wait_min) for n in figures(REVIEW_GATES_DOC, g, r"waits up to (\d+) minutes", "wait prose")]
    expected += [(n, "wait", wait_min) for n in figures(REVIEW_GATES_DOC, g, r"up to (\d+) waiting for CI", "wait figure")]
    expected += [(n, "job timeout", job_min) for n in figures(REVIEW_GATES_DOC, g, r"`claude-review` \((\d+) min", "job timeout figure")]
    gate_poll = figures(REVIEW_GATES_DOC, g, r"(\d+)-minute verdict poll", "poll figure")
    polls += gate_poll
    for path, text in sorted(flat.items()):
        if path == REVIEW_GATES_DOC:
            continue
        expected += [(n, "wait", wait_min) for n in figures(path, text, r"waits (?:first )?up to (\d+) minutes", "wait prose")]
        budget = figures(path, text, r"BUDGET:-(\d+)", "`BUDGET:-<seconds>` default", scale=60)
        prose = figures(path, text, r"(\d+)-minute (?:budget|deadline)", "`N-minute budget` prose")
        polls += budget + prose
    for (path, n), what, want in expected:
        if n != want:
            findings.append("%s says %d minutes for the %s, but %s says %d" % (path, n, what, REVIEW_WORKFLOW, want))
    values = sorted({n for _, n in polls})
    if len(values) > 1:
        findings.append("the poll budget disagrees across the docs: %s" % "; ".join(
            "%s says %d minutes" % (path, n) for path, n in sorted(polls)))
    for path, n in polls:
        if n <= job_min:
            findings.append("%s: the %d-minute poll budget must be greater than the review job's %d-minute "
                            "timeout in %s, or a healthy run reads as a stall" % (path, n, job_min, REVIEW_WORKFLOW))
    return findings


def check_review_budget_agrees(root=ROOT):
    root = Path(root)
    try:
        return _review_budget_findings(
            (root / REVIEW_WORKFLOW).read_text(encoding="utf-8"),
            (root / REVIEW_GATES_DOC).read_text(encoding="utf-8"),
            {p: (root / p).read_text(encoding="utf-8") for p in REVIEW_SKILLS})
    except OSError as e:
        return ["could not read %s: %s" % (e.filename, e.strerror)]


# --- check 25: README authored-line badge vs the git index (issue #1465) ------

# What counts as authored code. Tracked files only (`git ls-files`), so a local
# build tree or an ignored file never moves the number. Roots: the two
# components and scripts/, which hold every first-party source, test and the
# web UI. Extensions: C/C++ sources and headers, Python, shell and the web UI's
# JS/HTML/CSS. Blank lines are not counted, so reformatting does not move the
# figure. Docs, CMake files, data (HEX, rules, yaml, conformu logs) are not code.
AUTHORED_LINES_ROOTS = ("AlpacaCore/", "AlpacaHTTP/", "scripts/")
AUTHORED_LINES_EXTENSIONS = frozenset((
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx",
    ".py", ".sh", ".js", ".html", ".css",
))
# Third-party trees: the vendored SDKs and the bundled libgphoto2 both live
# under AlpacaCore/external/, and counting them would let vendor code dominate
# the number. Build directories are untracked, so the index already skips them.
AUTHORED_LINES_EXCLUDED_PREFIXES = ("AlpacaCore/external/",)
AUTHORED_LINES_ROUND = 1000
# Fail only when the badge is further than this fraction of the computed count
# from it. 5% of ~247k lines is ~12k, i.e. months of ordinary churn.
AUTHORED_LINES_TOLERANCE = 0.05
MIN_AUTHORED_LINES = 1000  # floor: fewer means the path filter or git listing broke

AUTHORED_BADGE_RE = re.compile(
    r"\[!\[Authored code\]\(https://img\.shields\.io/badge/Authored%20code-(?P<k>\d+)k%20lines-[A-Za-z0-9]+\.svg\)\]\([^)]*\)")


def is_authored_path(path):
    """True when a tracked path counts toward the authored-line badge."""
    if not path.startswith(AUTHORED_LINES_ROOTS) or path.startswith(AUTHORED_LINES_EXCLUDED_PREFIXES):
        return False
    return Path(path).suffix in AUTHORED_LINES_EXTENSIONS


def count_authored_lines(files):
    """Non-blank lines over {path: text}, for the paths is_authored_path accepts."""
    return sum(1 for path, text in files.items() if is_authored_path(path)
               for line in text.splitlines() if line.strip())


def _authored_lines_from_index(root=ROOT):
    tracked = _run_git(["-c", "core.quotePath=false", "ls-files"], root=root).stdout.splitlines()
    files = {}
    for path in tracked:
        if is_authored_path(path) and (root / path).is_file():
            files[path] = read(path, root)
    return count_authored_lines(files)


def authored_badge_line(count):
    k = int(round(count / AUTHORED_LINES_ROUND))
    return ("[![Authored code](https://img.shields.io/badge/Authored%%20code-%dk%%20lines-informational.svg)]"
            "(docs/architecture.md#modules)" % k)


def _authored_badge_findings(readme, count):
    """Check 25 over the README text and the computed count. Pure, so --self-test can drive it."""
    if count < MIN_AUTHORED_LINES:
        return ["authored-line count is %d, below the floor %d: the path filter or `git ls-files` listing is broken"
                % (count, MIN_AUTHORED_LINES)]
    m = AUTHORED_BADGE_RE.search(readme)
    if not m:
        return ["could not find the Authored code badge in README.md; paste: %s" % authored_badge_line(count)]
    shown = int(m.group("k")) * AUTHORED_LINES_ROUND
    if abs(shown - count) > AUTHORED_LINES_TOLERANCE * count:
        return ["README Authored code badge says %dk lines but %d lines are authored (more than %d%% apart); "
                "run `python3 scripts/check_docs_drift.py --badge` and paste: %s"
                % (shown // 1000, count, round(AUTHORED_LINES_TOLERANCE * 100), authored_badge_line(count))]
    return []


def check_authored_lines_badge(root=ROOT):
    return _authored_badge_findings(read("README.md", root), _authored_lines_from_index(root))


def print_badge(root=ROOT):
    count = _authored_lines_from_index(root)
    m = AUTHORED_BADGE_RE.search(read("README.md", root))
    print("authored lines: %d" % count)
    print("rounded: %dk" % int(round(count / AUTHORED_LINES_ROUND)))
    print("README.md now: %s" % (m.group(0) if m else "(badge not found)"))
    print("badge: %s" % authored_badge_line(count))
    return 0


CHECKS = [
    ("Instruction discovery and Claude adapters", check_instruction_structure),
    ("CMake options documented in docs/development.md", check_cmake_options_documented),
    ("zizmor pin sync (ci.yml vs ci_preflight.sh)", check_zizmor_pin_sync),
    ("cppcheck --suppress sync (ci.yml vs ci_preflight.sh)", check_cppcheck_suppress_sync),
    ("VERSION matches README badge", check_version_matches_readme),
    ("Blocking get_connected() list matches the code", check_blocking_get_connected_list),
    ("Agent and memory path references exist", check_agents_md_paths_exist),
    ("QHY SDK seam lists agree (interface / LockedQHYSDK / sweep)", check_qhy_seam_lists),
    ("TSan filtered runs sync (ci.yml vs ci_preflight.sh)", check_tsan_filtered_runs_sync),
    ("AGPL header form on every first-party source file", check_license_headers),
    ("Cursor rule file path references exist", check_rule_file_paths_exist),
    ("Skill Device API snapshot matches docs/ schema", check_skill_spec_hash),
    ("GPhoto STATUS paragraph names every validated body", check_gphoto_status_names_validated_bodies),
    ("Fake-connectable roster matches the fakes on disk", check_fake_roster_matches_disk),
    ("Every std::regex in router.cpp is built once (static)", check_router_regexes_static),
    ("README headline counts match SUPPORTED-DRIVERS.md", check_readme_headline_counts),
    ("SUPPORTED-DRIVERS.md Updated date is not behind the README release date", check_supported_drivers_updated_date),
    ("Documented AlpacaError values match alpaca_errors.h", check_error_codes_match_header),
    ("CI job roster (ci-gates instructions) matches ci.yml job ids", check_ci_roster_matches_workflow),
    ("Vendored shared libraries are not committed twice", check_vendored_so_not_duplicated),
    ("Cache restore-keys prefixes match only their own caches", check_cache_restore_prefixes_distinct),
    ("AGENTS.md stays within its size budget", check_agents_md_size),
    ("Decision records follow the README shape (status, sections, Rules lines, index)", check_decision_records),
    ("Decision record Adoption tables match the code", check_decision_adoption),
    ("Review job timeout, wait and poll budgets agree across the workflow, ci-gates and skills",
     check_review_budget_agrees),
    ("README Authored code badge is within tolerance of the tracked line count", check_authored_lines_badge),
]


def main():
    all_failures = []
    for name, fn in CHECKS:
        failures = fn()
        if failures:
            print("[FAIL] %s" % name)
            for f in failures:
                print("  - %s" % f)
        else:
            print("[PASS] %s" % name)
        all_failures.extend(failures)

    if all_failures:
        print("\n%d finding(s)." % len(all_failures))
        return 1

    print("\nDocs drift check OK.")
    return 0


# --- self-test ---------------------------------------------------------------

# Literal fixtures in the shape of today's two files (the [ -x ... ] probe and
# the unrelated jobs after the TSan and cppcheck jobs are deliberate: they are
# what the scoping rules must ignore).
_SELF_TEST_CI = """\
jobs:
  build:
    steps:
      - run: echo build
  sanitizers-tsan:
    steps:
      - name: Run
        run: |
          test -x build-tsan/tests/alpacacore_tests  # probe, not a run
          build-tsan/tests/alpacacore_tests "[stress]" 2>&1 | tee stress-run.log
          grep -qE 'test cases: *[1-9]' stress-run.log
          build-tsan/tests/alpacacore_tests "[stress-guard]" 2>&1 | tee stress-guard-run.log
          grep -qE 'test cases: *[1-9]' stress-guard-run.log
  format:
    steps:
      - run: grep -qE 'unrelated' other.log
  cppcheck:
    steps:
      - run: cppcheck --suppress=missingInclude --suppress=unusedFunction src
  zizmor:
    steps:
      - run: echo --suppress=notCppcheck
"""

_SELF_TEST_PREFLIGHT = """\
# --- gate 3: sanitizers ------------------------------------------------------
TSAN_BUILD_DIR="AlpacaCore/build-tsan"
section "ThreadSanitizer (concurrency stress, all vendors)"
if [ -x "${TSAN_BUILD_DIR}/tests/alpacacore_tests" ]; then
  "${TSAN_BUILD_DIR}/tests/alpacacore_tests" "[stress]" 2>&1 | tee "${TSAN_BUILD_DIR}/stress-run.log"
  if grep -qE 'test cases: *[1-9]' "${TSAN_BUILD_DIR}/stress-run.log"; then echo ok; fi
  "${TSAN_BUILD_DIR}/tests/alpacacore_tests" "[stress-guard]" 2>&1 | tee "${TSAN_BUILD_DIR}/stress-guard-run.log"
  if grep -qE 'test cases: *[1-9]' "${TSAN_BUILD_DIR}/stress-guard-run.log"; then echo ok; fi
fi
# --- gate 4: something else --------------------------------------------------
grep -qE 'unrelated' "${TSAN_BUILD_DIR}/other.log"
"""


def self_test():
    """Regression guard for check 8's helpers and the job-scoping rule.

    Every fixture is a literal string, so this needs no repo state. The
    mutations are the ones the #455 review tabulated; each must produce the
    finding named for it, and the unmutated pair must produce none (which is
    also what catches a run matcher that silently stops matching: the floor
    fires and the baseline is no longer clean).
    """
    checks = []

    def check(name, condition):
        checks.append((name, condition))

    def sub(text, old, new, count=1):
        assert text.count(old) == count, (old, text.count(old))
        return text.replace(old, new)

    # check 6: the lock each LockedQHYSDK forward takes (open-astro#339, #1646).
    qhy_ok = {
        "open_camera": "{ locked([&] { inner_.open_camera(); }); }",
        "cancel_exposure": "{ std::lock_guard<std::mutex> lk(cancel_mutex_); inner_.cancel_exposure(); }",
        "get_sdk_version": "{ return locked(version_mutex_, [&] { return inner_.get_sdk_version(); }); }",
    }
    qhy_names = set(qhy_ok)

    def qhy_with(name, body):
        return _qhy_forward_lock_findings(dict(qhy_ok, **{name: body}), qhy_names)

    check("QHY lock check: baseline forwards produce no finding",
          _qhy_forward_lock_findings(qhy_ok, qhy_names) == [])
    check("QHY lock check: get_sdk_version through the shared mutex fails",
          any("get_sdk_version() must take version_mutex_" in x
              for x in qhy_with("get_sdk_version", "{ return locked([&] { return inner_.get_sdk_version(); }); }")))
    check("QHY lock check: get_sdk_version with no lock fails",
          any("get_sdk_version() must take version_mutex_" in x
              for x in qhy_with("get_sdk_version", "{ return inner_.get_sdk_version(); }")))
    check("QHY lock check: get_sdk_version naming both mutexes fails",
          any("get_sdk_version() must take version_mutex_" in x
              for x in qhy_with("get_sdk_version",
                                "{ locked([&] {}); return locked(version_mutex_, [&] { return 1; }); }")))
    check("QHY lock check: another forward on locked(other_mutex_, ...) fails",
          any("UNLOCKED FORWARD: LockedQHYSDK::open_camera()" in x
              for x in qhy_with("open_camera", "{ locked(other_mutex_, [&] { inner_.open_camera(); }); }")))
    check("QHY lock check: another forward on locked(version_mutex_, ...) fails",
          any("UNLOCKED FORWARD: LockedQHYSDK::open_camera()" in x
              for x in qhy_with("open_camera", "{ locked(version_mutex_, [&] { inner_.open_camera(); }); }")))
    check("QHY lock check: an unlocked forward fails",
          any("UNLOCKED FORWARD: LockedQHYSDK::open_camera()" in x
              for x in qhy_with("open_camera", "{ inner_.open_camera(); }")))
    check("QHY lock check: cancel_exposure through locked() fails",
          any("cancel_exposure() must take cancel_mutex_" in x
              for x in qhy_with("cancel_exposure", "{ locked([&] { inner_.cancel_exposure(); }); }")))

    ci, pf = _SELF_TEST_CI, _SELF_TEST_PREFLIGHT
    baseline = _tsan_findings(ci, pf)
    check("baseline fixtures produce no finding: %r" % baseline, baseline == [])
    check("the [ -x ] / test -x probes are not counted as runs",
          len([e for e in _tsan_events(_ci_job_block(ci, "sanitizers-tsan")) if e[0] == "run"]) == 2)

    # 1. a retagged run whose tee target was not retagged.
    m = sub(ci, '"[stress-guard]" 2>&1 | tee stress-guard-run.log', '"[stress-guard]" 2>&1 | tee stress-run.log')
    f = _tsan_findings(m, pf)
    check("guard run keeping the old tee target is paired by log, not count",
          any("run [stress-guard] writes stress-run.log but the grep that follows it reads stress-guard-run.log" in x for x in f))

    # 2. one pattern loosened in ci.yml only.
    m = sub(ci, "grep -qE 'test cases: *[1-9]' stress-run.log", "grep -qE 'test cases' stress-run.log")
    check("a loosened pattern in one file is a pattern finding",
          any("grep pattern differs" in x for x in _tsan_findings(m, pf)))

    # 3. the two runs swapped in ci_preflight.sh only (greps left in place).
    m = pf.replace('"[stress]" 2>&1 | tee "${TSAN_BUILD_DIR}/stress-run.log"', "@A@")
    m = m.replace('"[stress-guard]" 2>&1 | tee "${TSAN_BUILD_DIR}/stress-guard-run.log"',
                  '"[stress]" 2>&1 | tee "${TSAN_BUILD_DIR}/stress-run.log"')
    m = m.replace("@A@", '"[stress-guard]" 2>&1 | tee "${TSAN_BUILD_DIR}/stress-guard-run.log"')
    f = _tsan_findings(ci, m)
    check("swapped runs report both pairs as reading the wrong log",
          len([x for x in f if "the guard must read the log of the run it guards" in x]) == 2)

    # 4. a grep deleted in ci.yml.
    m = sub(ci, "          grep -qE 'test cases: *[1-9]' stress-guard-run.log\n", "")
    check("a deleted grep leaves its run unguarded",
          any("run [stress-guard] is not followed by a zero-test grep" in x for x in _tsan_findings(m, pf)))

    # 5. the build directory renamed in ci.yml for one run only.
    m = sub(ci, 'build-tsan/tests/alpacacore_tests "[stress-guard]"', 'build-tsan2/tests/alpacacore_tests "[stress-guard]"')
    check("a build-dir rename in ci.yml is a build-directory finding",
          any("build directory differs" in x and "build-tsan2" in x for x in _tsan_findings(m, pf)))

    # 6. TSAN_BUILD_DIR renamed in ci_preflight.sh only.
    m = sub(pf, 'TSAN_BUILD_DIR="AlpacaCore/build-tsan"', 'TSAN_BUILD_DIR="AlpacaCore/tsan-build"')
    check("a TSAN_BUILD_DIR rename is a build-directory finding",
          any("build directory differs" in x and "tsan-build" in x for x in _tsan_findings(ci, m)))

    # 7. a run that does not tee at all.
    m = sub(ci, '"[stress]" 2>&1 | tee stress-run.log', '"[stress]"')
    check("a run without tee is reported, not skipped",
          any("does not `| tee` a log" in x for x in _tsan_findings(m, pf)))

    # 8. scope: the unrelated grep after the TSan job and the unrelated
    #    --suppress= after the cppcheck job are outside both blocks; an
    #    inserted job between them must not widen either.
    inserted = sub(ci, "  format:\n", "  extra-after-tsan:\n    steps:\n      - run: grep -qE 'x' x.log\n  format:\n")
    check("an inserted job after sanitizers-tsan does not widen check 8's scope",
          _tsan_findings(inserted, pf) == [])
    block = _ci_job_block(inserted, "cppcheck")
    check("check 3's cppcheck block ends at the next job whatever its name",
          block is not None and "unusedFunction" in block and "notCppcheck" not in block)
    check("an unknown job name yields None, not a widened block",
          _ci_job_block(ci, "no-such-job") is None)
    check("memory references are found in source comments",
          MEMORY_COMMENT_PATH_RE.findall("// See docs/decisions/0001-example.md") ==
          ["docs/decisions/0001-example.md"])
    check("memory comment ids: a bare record number is read, a full file name is not",
          MEMORY_COMMENT_ID_RE.findall("// (docs/decisions/0008).") == [("decisions", "0008")]
          and MEMORY_COMMENT_ID_RE.findall("// docs/decisions/0008-x.md") == []
          and MEMORY_COMMENT_ID_RE.findall("// see docs/decisions/0008.") == [("decisions", "0008")]
          and MEMORY_COMMENT_ID_RE.findall("// docs/decisions/0008.md") == [])

    # check_memory_comment_paths_exist over a fixture tree. The root lives
    # under a directory named "build-fixture": the checkout's own ancestors
    # must never count as an excluded component (they did once, and the
    # check went vacuously green). Each expectation is one rule of the check.
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / "build-fixture" / "repo"
        fixture = {
            "docs/failures/0001-present.md": "# present\n",
            "AlpacaCore/src/ok.cpp": "// See docs/failures/0001-present.md\n",
            "AlpacaCore/src/bad.cpp": "// See docs/failures/0002-missing.md\n",
            "scripts/build_deb.sh": "# See docs/failures/0003-missing.md\n",
            "AlpacaHTTP/src/code.cpp": 'std::string s = "docs/failures/0004-missing.md";\n',
            "AlpacaHTTP/src/plain.cpp": "// no memory reference here\n",
            # Two spare files above MIN_MEMORY_COMMENT_FILES, so raising the
            # floor by one does not turn this fixture into a floor failure.
            "AlpacaHTTP/src/spare_a.h": "// spare\n",
            "scripts/spare_b.py": "# spare\n",
            "AlpacaCore/build/gen.cpp": "// See docs/failures/0005-missing.md\n",
            "AlpacaCore/external/sdk.h": "// See docs/failures/0006-missing.md\n",
            "AlpacaCore/src/notes.txt": "// See docs/failures/0007-missing.md\n",
        }
        for name, text in fixture.items():
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_text(text, encoding="utf-8")
        found = check_memory_comment_paths_exist(root)
        check("memory comment check: a resolving reference is not reported",
              not any("ok.cpp" in f for f in found))
        check("memory comment check: a missing record in a source comment is reported",
              any(f.startswith("AlpacaCore/src/bad.cpp:1 ") and "0002-missing" in f for f in found))
        check("memory comment check: a file whose own name starts with build is scanned",
              any(f.startswith("scripts/build_deb.sh:1 ") for f in found))
        check("memory comment check: a path in a non-comment line is not a reference",
              not any("code.cpp" in f for f in found))
        check("memory comment check: build/ and external/ directories are skipped",
              not any("gen.cpp" in f or "sdk.h" in f for f in found))
        check("memory comment check: only first-party source extensions are scanned",
              not any("notes.txt" in f for f in found))
        check("memory comment check: exactly the two expected findings", len(found) == 2)
        empty = Path(tmp) / "empty"
        empty.mkdir()
        check("memory comment check: a root with no first-party files trips the floor",
              any("floor" in f for f in check_memory_comment_paths_exist(empty)))

    # check_skill_spec_hash over a fixture tree: the skill pins the SHA-256
    # of the LF-normalized Device API schema its endpoint catalog was built
    # from, so a refreshed docs/ schema must fail until the pin moves.
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        spec = "openapi: 3.1.1\npaths: {}\n"
        pin = hashlib.sha256(spec.encode("utf-8")).hexdigest()

        def write_fixture(spec_text, sources_text):
            for name, text in ((SKILL_SPEC_PATH, spec_text), (SKILL_SOURCES_PATH, sources_text)):
                if text is None:
                    (root / name).unlink(missing_ok=True)
                    continue
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_bytes(text.encode("utf-8"))
            return check_skill_spec_hash(root)

        sources = "The schema snapshot has SHA-256:\n\n`%s`\n" % pin
        check("skill spec hash: a schema matching the pin is not reported",
              write_fixture(spec, sources) == [])
        check("skill spec hash: the same schema with CRLF line endings is not reported",
              write_fixture(spec.replace("\n", "\r\n"), sources) == [])
        check("skill spec hash: a changed schema is reported",
              any("does not match" in f for f in write_fixture(spec + "x: 1\n", sources)))
        check("skill spec hash: a skill doc with no pinned hash is reported",
              any("no pinned" in f for f in write_fixture(spec, "no hash here\n")))
        # Two pins is ambiguity, not absence: the old message said "no pinned"
        # for both and sent the reader looking for a hash that is really there.
        check("skill spec hash: a skill doc pinning two hashes is reported as ambiguous",
              any("ambiguous" in f for f in write_fixture(spec, sources + sources)))
        check("skill spec hash: a missing schema file is reported",
              any("does not exist" in f for f in write_fixture(None, sources)))

    # check 7 (check_agents_md_paths_exist) over fixture git repositories. The
    # check shells out to git, so each fixture is a real repository; every
    # scenario builds its own, so no scenario sees another's cached listing.
    import os
    import tempfile

    def attempt(label, fn):
        # A signature that cannot take a root yet raises TypeError; record that
        # as a FAIL rather than crashing the whole self-test.
        try:
            return fn()
        except Exception as exc:  # noqa: BLE001 - any crash is a failed check
            check("%s (raised %s: %s)" % (label, type(exc).__name__, exc), False)
            return None

    # Isolated git: no user config or hooks, and no GIT_DIR from an enclosing
    # hook pointing the fixture's git at the real repository.
    git_env = {"GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1"}
    stripped = ("GIT_DIR", "GIT_INDEX_FILE", "GIT_WORK_TREE", "GIT_COMMON_DIR")
    saved_env = {k: os.environ.get(k) for k in list(git_env) + list(stripped)}
    os.environ.update(git_env)
    for name in stripped:
        os.environ.pop(name, None)

    def make_repo(base, agents_dir=True, extra=None):
        root = Path(base)
        files = {
            "AGENTS.md": "".join("See `scripts/f%d.py`.\n" % (i % MIN_MEMORY_COMMENT_FILES) for i in range(MIN_AGENTS_MD_PATH_REFS + 2)),
            "CONTEXT.md": "# Context\n",
            "README.md": "# Readme\n",
            ".github/instructions/a.instructions.md": "# a\n",
            ".claude/skills/s/SKILL.md": "# s\n",
            ".gitignore": "scripts/gen/\n",
        }
        # Two spare files above MIN_MEMORY_COMMENT_FILES (check 7 ends with the
        # memory-comment check), so raising that floor by one is not a failure here.
        for i in range(MIN_MEMORY_COMMENT_FILES + 2):
            files["scripts/f%d.py" % i] = "# first-party source\n"
        if agents_dir:
            files["docs/agents/x.md"] = "# x\n"
        files.update(extra or {})
        for name, text in files.items():
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_text(text, encoding="utf-8")
        for args in (["init", "-q"], ["add", "-A"],
                     ["-c", "user.email=t@t", "-c", "user.name=t", "commit", "-q", "-m", "fixture"]):
            subprocess.run(["git"] + args, cwd=root, check=True, capture_output=True)
        return root

    try:
        with tempfile.TemporaryDirectory() as tmp:
            def repo_fixture(name, **kwargs):
                (Path(tmp) / name).mkdir()
                return make_repo(Path(tmp) / name, **kwargs)

            def run_check(root):
                return attempt("agents md check: accepts a root", lambda: check_agents_md_paths_exist(root))

            found = run_check(repo_fixture("clean"))
            check("agents md check: a clean fixture repository reports nothing", found == [])

            drift = repo_fixture("drift")
            with open(drift / "AGENTS.md", "a", encoding="utf-8") as f:
                f.write("Also `scripts/nope.py` and `scripts/gen/out.py`.\n")
            found = run_check(drift)
            check("agents md check: a drifted span is reported",
                  found is not None and any("scripts/nope.py" in f for f in found))
            check("agents md check: a gitignored span is not reported",
                  found is not None and not any("scripts/gen/out.py" in f for f in found))

            # AGENTS.md is one of five document loops in this check; a root
            # threaded into that one only proves nothing about the other four,
            # so drift a skill doc and an agent doc too (issue #583).
            loops = repo_fixture("loops")
            with open(loops / ".claude/skills/s/SKILL.md", "a", encoding="utf-8") as f:
                f.write("See `scripts/skill_nope.py`.\n")
            with open(loops / "docs/agents/x.md", "a", encoding="utf-8") as f:
                f.write("See `scripts/agent_nope.py`.\n")
            found = run_check(loops)
            check("agents md check: a drifted span in a skill doc is reported",
                  found is not None and any("SKILL.md" in f and "scripts/skill_nope.py" in f for f in found))
            check("agents md check: a drifted span in an agent doc is reported",
                  found is not None and any("docs/agents/x.md" in f and "scripts/agent_nope.py" in f for f in found))

            # CONTEXT.md, the domain glossary, is scanned like AGENTS.md; a
            # rename must not silently drop it from the check.
            context = repo_fixture("context")
            with open(context / "CONTEXT.md", "a", encoding="utf-8") as f:
                f.write("See `scripts/context_nope.py`.\n")
            found = run_check(context)
            check("agents md check: a drifted span in CONTEXT.md is reported",
                  found is not None and any("CONTEXT.md" in f and "scripts/context_nope.py" in f for f in found))
            nocontext = repo_fixture("nocontext")
            (nocontext / "CONTEXT.md").unlink()
            found = run_check(nocontext)
            check("agents md check: a missing CONTEXT.md is reported",
                  found is not None and any("CONTEXT.md" in f and "does not exist" in f for f in found))

            # README.md is scanned by name too (issue #693): a drifted span and
            # a missing file are each a finding, like CONTEXT.md.
            readme = repo_fixture("readme")
            with open(readme / "README.md", "a", encoding="utf-8") as f:
                f.write("See `scripts/readme_nope.py`.\n")
            found = run_check(readme)
            check("agents md check: a drifted span in README.md is reported",
                  found is not None and any("README.md" in f and "scripts/readme_nope.py" in f for f in found))
            noreadme = repo_fixture("noreadme")
            (noreadme / "README.md").unlink()
            found = run_check(noreadme)
            check("agents md check: a missing README.md is reported",
                  found is not None and any("README.md" in f and "does not exist" in f for f in found))

            # A templated descriptor span (`<vendor>` placeholder) under the
            # vendor or catalog tree must match at least one tracked file.
            tmpl_files = {"AlpacaCore/src/vendors/acme/acme_fields.h": "//\n",
                          "AlpacaCore/src/catalog/builtin_catalog.cpp": "//\n"}
            tmpl_ok = repo_fixture("tmpl_ok", extra=tmpl_files)
            with open(tmpl_ok / ".github/instructions/a.instructions.md", "a", encoding="utf-8") as f:
                f.write("Write `AlpacaCore/src/vendors/<vendor>/<vendor>_fields.h` and "
                        "`AlpacaCore/src/catalog/builtin_catalog.cpp`.\n")
            found = run_check(tmpl_ok)
            check("agents md check: a templated descriptor span that matches a tracked file passes",
                  found is not None and found == [])
            tmpl_bad = repo_fixture("tmpl_bad", extra=tmpl_files)
            with open(tmpl_bad / ".github/instructions/a.instructions.md", "a", encoding="utf-8") as f:
                f.write("Write `AlpacaCore/src/vendors/<vendor>/<vendor>_gone.cpp`.\n")
            found = run_check(tmpl_bad)
            check("agents md check: a templated descriptor span that matches no tracked file is reported",
                  found is not None and any("<vendor>_gone.cpp" in f for f in found))
            tmpl_dir = repo_fixture("tmpl_dir", extra=tmpl_files)
            with open(tmpl_dir / ".github/instructions/a.instructions.md", "a", encoding="utf-8") as f:
                f.write("Write `AlpacaCore/src/catalog/<kind>_gone.cpp`.\n")
            found = run_check(tmpl_dir)
            check("agents md check: a templated span under the catalog tree is checked too",
                  found is not None and any("<kind>_gone.cpp" in f for f in found))
            tmpl_other = repo_fixture("tmpl_other", extra=tmpl_files)
            with open(tmpl_other / ".github/instructions/a.instructions.md", "a", encoding="utf-8") as f:
                f.write("See `scripts/<x>_nope.py`.\n")
            found = run_check(tmpl_other)
            check("agents md check: a templated span outside the descriptor trees stays skipped",
                  found is not None and found == [])

            tmpl_glob = repo_fixture("tmpl_glob", extra=tmpl_files)
            with open(tmpl_glob / ".github/instructions/a.instructions.md", "a", encoding="utf-8") as f:
                f.write("See `AlpacaCore/src/vendors/<vendor>/*.cpp` and "
                        "`AlpacaCore/src/catalog/<kind>.md#fields`.\n")
            found = run_check(tmpl_glob)
            check("agents md check: a templated descriptor span with a glob or fragment stays skipped",
                  found is not None and found == [])

            gone = repo_fixture("gone")
            (gone / ".github/instructions/a.instructions.md").unlink()
            found = run_check(gone)
            check("agents md check: a deleted tracked instruction file is reported as missing",
                  found is not None and any("a.instructions.md" in f and "missing" in f for f in found))

            nested = repo_fixture("nested", extra={"docs/agents/sub/y.md": "# y\n"})
            found = run_check(nested)
            check("agents md check: a nested docs/agents file is not reported as missing",
                  found is not None and found == [])

            # A file written but not yet staged is in the working tree, so a
            # reference to it is not drift (issue #556a). The fallback tuple in
            # _tracked_paths has to cover every directory check 7 scans.
            unstaged = repo_fixture("unstaged")
            (unstaged / "docs/agents/foo.md").write_text("# foo\n", encoding="utf-8")
            (unstaged / ".claude/skills/s/NOTES.md").write_text("# notes\n", encoding="utf-8")
            with open(unstaged / "AGENTS.md", "a", encoding="utf-8") as f:
                f.write("Also `docs/agents/foo.md` and `.claude/skills/s/NOTES.md`.\n")
            found = run_check(unstaged)
            check("agents md check: an unstaged agent or skill doc is not reported as drift",
                  found is not None and found == [])

            first = repo_fixture("first")
            second = repo_fixture("second", extra={"scripts/only_second.py": "# only here\n"})
            listed_first = attempt("tracked paths: accepts a root", lambda: _tracked_paths(first))
            listed_second = attempt("tracked paths: accepts a root", lambda: _tracked_paths(second))
            check("tracked paths: two roots in one process do not share a cache",
                  listed_first is not None and listed_second is not None
                  and "scripts/only_second.py" not in listed_first[0]
                  and "scripts/only_second.py" in listed_second[0])

            found = run_check(repo_fixture("noagents", agents_dir=False))
            check("agents md check: an empty docs/agents trips the floor",
                  found is not None and any("docs/agents" in f and "floor" in f for f in found))
    finally:
        for name, value in saved_env.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value

    gp_table = "### GPhoto\n\n| M | C | L | S |\n|--|--|--|--|\n| Nikon D3300 | USB | \u2713 | x |\n| Canon EOS 4000D | USB | \u2713 | x |\n\n### Next\n"
    check("gphoto status: a validated model the STATUS paragraph omits is flagged",
          len(_gphoto_status_findings(gp_table, "**STATUS: validated against the Nikon D3300.**\n\nrest\n")) == 1)
    gp_mixed = "### GPhoto\n\n| M | C | L | S |\n|--|--|--|--|\n| Nikon D3300 | USB | \u2713 | x |\n| Canon EOS 4000D | USB (PTP) | \u2713 | x |\n\n### Next\n"
    check("gphoto status: a row whose Connection cell is 'USB (PTP)' is still gated",
          len(_gphoto_status_findings(gp_mixed, "**STATUS: validated: Nikon D3300.**\n\nrest\n")) == 1)
    gp_roman = "### GPhoto\n\n| M | C | L | S |\n|--|--|--|--|\n| Sony A7 III | USB | \u2713 | x |\n\n### Next\n"
    check("gphoto status: a designation with no digit ('III') does not match by accident",
          len(_gphoto_status_findings(gp_roman, "**STATUS: validated: Canon EOS 4000D, see Part III.**\n\nrest\n")) == 1)
    gp_last = "### GPhoto\n\n| M | C | L | S |\n|--|--|--|--|\n| Nikon D3300 | USB | \u2713 | x |\n\n## Mounts\n\n| M | C | L | S |\n|--|--|--|--|\n| Other Mount 9000 | USB | \u2713 | x |\n"
    check("gphoto status: the GPhoto section ends at the next '## ' heading, not only the next '### '",
          _gphoto_status_findings(gp_last, "**STATUS: validated: Nikon D3300.**\n\nrest\n") == [])
    check("gphoto status: every validated model named passes",
          _gphoto_status_findings(gp_table, "**STATUS: validated: Nikon D3300, Canon EOS 4000D.**\n\nrest\n") == [])

    roster_hdr = ("#define CS_ZWO(X) X(zwo_telescope)\n"
                  "#define CS_GEMINI(X) X(gemini_switch) X(gemini_switch_b)\n"
                  "inline constexpr FakeRosterRow kFakeConnectableRoster[] = {\n"
                  "    {\"zwo\", \"telescope\", \"fake_mount_server.h\"},\n"
                  "    {\"gemini\", \"switch\", \"fake_gemini_pdh.h\"},\n"
                  "};\n")
    roster_disk = {"fake_mount_server.h", "fake_gemini_pdh.h", "fake_pty_write.h"}
    roster_helpers = {"fake_pty_write.h": "helper"}
    check("fake roster: a matching roster and disk produce no finding",
          _fake_roster_findings(roster_hdr, roster_disk, roster_helpers) == [])
    f = _fake_roster_findings(roster_hdr, roster_disk | {"fake_new_sdk.h"}, roster_helpers)
    check("fake roster: a fake on disk with no roster row is flagged by name",
          len(f) == 1 and "fake_new_sdk.h is not in kFakeConnectableRoster" in f[0])
    f = _fake_roster_findings(roster_hdr, {"fake_mount_server.h", "fake_pty_write.h"}, roster_helpers)
    check("fake roster: a roster row whose fake is gone is flagged",
          len(f) == 1 and "fake_gemini_pdh.h" in f[0] and "does not exist" in f[0])
    f = _fake_roster_findings(roster_hdr, roster_disk, {"fake_mount_server.h": "now rostered"})
    check("fake roster: a helper that gained a roster row is a stale helper",
          any("STALE HELPER_FAKES entry: fake_mount_server.h now has" in x for x in f))
    f = _fake_roster_findings(roster_hdr, roster_disk, {"fake_pty_write.h": "h", "fake_gone.h": "h"})
    check("fake roster: a helper whose file is gone is a stale helper",
          any("STALE HELPER_FAKES entry: fake_gone.h does not exist" in x for x in f))
    f = _fake_roster_findings(roster_hdr.replace('{"zwo", "telescope"', '{"zwoo", "telescope"'), roster_disk, roster_helpers)
    check("fake roster: a row naming an unknown vendor is flagged",
          any("zwoo" in x and "no registry entry" in x for x in f))
    f = _fake_roster_findings(roster_hdr.replace('{"gemini", "switch"', '{"gemini", "focuser"'), roster_disk, roster_helpers)
    check("fake roster: a row naming a pair the registry lacks is flagged",
          any("gemini, focuser" in x and "no registry entry" in x for x in f))
    only_backend = roster_hdr.replace("X(gemini_switch) X(gemini_switch_b)", "X(gemini_switch_b)")
    check("fake roster: a pair covered only by a second-backend id (three segments) is a registry pair",
          "X(gemini_switch)" not in only_backend and _fake_roster_findings(only_backend, roster_disk, roster_helpers) == [])
    check("fake roster: a missing roster array is a finding, not a silent pass",
          len(_fake_roster_findings("// nothing here\n", roster_disk, roster_helpers)) == 1)
    check("fake roster: a row inside a comment is not a row",
          len(_fake_roster_findings("// {\"a\", \"b\", \"fake_x.h\"},\n" + roster_hdr, roster_disk, roster_helpers)) == 0)

    rx_ok = 'static const std::regex kX(R"(a)");\n'
    check("static regex: a static const regex is clean", _static_regex_findings(rx_ok) == [])
    f = _static_regex_findings(rx_ok.replace("static ", ""))
    check("static regex: dropping 'static' is flagged as not static",
          len(f) == 1 and "kX is not static" in f[0])
    f = _static_regex_findings('if (std::regex_match(p, m, std::regex("a"))) {}\n' + rx_ok)
    check("static regex: an unnamed temporary is flagged (regex_match itself is not)",
          len(f) == 1 and "temporary" in f[0])
    check("static regex: a construction inside a comment is not one",
          _static_regex_findings("// std::regex x(\"a\");\n" + rx_ok) == [])
    check("static regex: 'static' on the previous line still counts",
          _static_regex_findings('static\nconst std::regex kX(R"(a)");\n') == [])
    check("static regex: 'static' belonging to an earlier statement does not count",
          len(_static_regex_findings('static int n = 0;\nconst std::regex kX(R"(a)");\n')) == 1)
    f = _static_regex_findings('auto r = std::regex{"a"};\n' + rx_ok)
    check("static regex: a brace-built temporary is flagged",
          len(f) == 1 and "temporary" in f[0])
    f = _static_regex_findings('std::regex const kY("a");\n' + rx_ok)
    check("static regex: 'std::regex const name' without static is flagged",
          len(f) == 1 and "kY is not static" in f[0])
    f = _static_regex_findings(rx_ok.replace("static ", ""))
    check("static regex: the not-static finding names the static keyword (scope is not modelled)",
          len(f) == 1 and "static keyword" in f[0])
    check("static regex: 'static std::regex const name' is clean",
          _static_regex_findings('static std::regex const kY("a");\n') == [])
    f = _static_regex_findings("int x = 1;\n")
    check("static regex: no construction at all is a floor finding, not a pass",
          len(f) == 1 and "extractor is stale" in f[0])

    # check 15: README headline vs SUPPORTED-DRIVERS.md (issue #684).
    hl_map = {"Alpha": "Alpha", "Beta Motor": "Beta", "Beta Handset": "Beta", "GPhoto": "DSLRs (via libgphoto2)"}
    hl_extras = {"Gamma (Alpha plugin)": "a row in the Alpha table"}
    hl_readme = ("intro\n- **3 validated devices. Four brands. One server.** Alpha, DSLRs (via libgphoto2), "
                 "Beta, and Gamma (Alpha plugin).\n- **Other.** text\n")
    hl_supported = ("### Alpha\n\n| Model Series | Connection | Linux | Status |\n|---|---|---|---|\n"
                    "| A1 | USB | \u2713 | ok |\n| A2 | USB |  | pending |\n\n### Beta Motor\n\n"
                    "| Model Series | Connection | Linux | Status |\n|---|---|---|---|\n| B1 | USB | \u2713 | ok |\n\n"
                    "### Beta Handset\n\n| Model Series | Connection | Linux | Status |\n|---|---|---|---|\n"
                    "| B2 | USB | \u2713 | ok |\n\n### GPhoto\n\n| Source | Connection | Linux | Status |\n"
                    "|---|---|---|---|\n")
    check("readme headline: baseline fixtures produce no finding",
          _readme_headline_findings(hl_readme, hl_supported, hl_map, hl_extras) == [])
    check("readme headline: the row filter counts check marks and skips separators, headers and '| Source' rows",
          count_validated_device_rows(hl_supported + "| Model Series \u2713 |\n| Source \u2713 |\n|-- \u2713 |\n") == 3)
    f = _readme_headline_findings(sub(hl_readme, "**3 validated", "**4 validated"), hl_supported, hl_map, hl_extras)
    check("readme headline: a device count that disagrees with the validated rows is flagged",
          len(f) == 1 and "says 4 validated devices but SUPPORTED-DRIVERS.md has 3" in f[0])
    f = _readme_headline_findings(hl_readme, hl_supported + "### Delta\n\n| Model Series | C | L | S |\n|--|--|--|--|\n| D1 | USB | \u2713 | ok |\n", hl_map, hl_extras)
    check("readme headline: a validated row added without a README edit is a device-count finding",
          any("says 3 validated devices but SUPPORTED-DRIVERS.md has 4" in x for x in f))
    check("readme headline: a new vendor heading with no map entry is flagged",
          any("heading '### Delta' has no entry in SUPPORTED_HEADING_TO_README_BRAND" in x for x in f))
    f = _readme_headline_findings(sub(hl_readme, "Four brands", "Five brands"), hl_supported, hl_map, hl_extras)
    check("readme headline: a brand word that disagrees with the list length is flagged",
          len(f) == 1 and "says Five (5) brands but its brand list has 4 items" in f[0])
    f = _readme_headline_findings(sub(hl_readme, "Four brands", "Several brands"), hl_supported, hl_map, hl_extras)
    check("readme headline: a brand count that is not a number word is flagged",
          len(f) == 1 and "not a number word" in f[0])
    check("readme headline: number words parse ('twenty-one' -> 21, 'Fifteen' -> 15, 'ninety' -> 90)",
          (_number_word_to_int("twenty-one"), _number_word_to_int("Fifteen"), _number_word_to_int("ninety")) == (21, 15, 90))
    f = _readme_headline_findings(sub(hl_readme, "Alpha, DSLRs", "Alpha, Alpha, DSLRs").replace("Four brands", "Five brands"), hl_supported, hl_map, hl_extras)
    check("readme headline: a repeated list item is flagged",
          len(f) == 1 and "repeats an item: Alpha" in f[0])
    f = _readme_headline_findings(sub(hl_readme, "Beta, and Gamma", "and Gamma").replace("Four brands", "Three brands"), hl_supported, hl_map, hl_extras)
    check("readme headline: a mapped brand missing from the README list is flagged for each heading that maps to it",
          len(f) == 2 and all("does not name 'Beta'" in x for x in f))
    f = _readme_headline_findings(sub(hl_readme, "Beta, and Gamma", "Beta, Omega, and Gamma").replace("Four brands", "Five brands"), hl_supported, hl_map, hl_extras)
    check("readme headline: a README list item with no heading and no declared reason is flagged",
          len(f) == 1 and "'Omega' matches no SUPPORTED-DRIVERS.md heading" in f[0])
    f = _readme_headline_findings(hl_readme, hl_supported.split("### GPhoto")[0], hl_map, hl_extras)
    check("readme headline: a dropped vendor reports the stale map entry and the orphaned README item in one run",
          len(f) == 2 and any("names '### GPhoto' but SUPPORTED-DRIVERS.md has no such heading" in x for x in f)
          and any("'DSLRs (via libgphoto2)' matches no SUPPORTED-DRIVERS.md heading" in x for x in f))
    f = _readme_headline_findings(hl_readme, sub(hl_supported, "### Beta Handset", "### Beta Wireless"), hl_map, hl_extras)
    check("readme headline: a renamed heading is both a stale map entry and an unmapped heading",
          any("names '### Beta Handset' but SUPPORTED-DRIVERS.md has no such heading" in x for x in f)
          and any("heading '### Beta Wireless' has no entry" in x for x in f))
    f = _readme_headline_findings(sub(hl_readme, ", and Gamma (Alpha plugin)", "").replace("Four brands", "Three brands"), hl_supported, hl_map, hl_extras)
    check("readme headline: a declared no-heading brand missing from the list is a stale-table finding",
          len(f) == 1 and "README_BRANDS_WITHOUT_HEADING names 'Gamma (Alpha plugin)'" in f[0])
    f = _readme_headline_findings(hl_readme.replace("\u2713", ""), hl_supported.replace("\u2713", ""), hl_map, hl_extras)
    check("readme headline: no validated rows at all is a floor finding, not a pass",
          any("cannot count anything" in x for x in f))
    f = _readme_headline_findings("- **Other.** text\n", hl_supported, hl_map, hl_extras)
    check("readme headline: a missing headline is a finding",
          len(f) == 1 and "could not find" in f[0])
    # --counts (issue #689): the release recipe's numbers come from the same
    # functions the gate uses, so the summary must agree with a clean baseline
    # and its number words must round-trip through the gate's parser.
    check("counts: every number word 0..99 round-trips through the check 15 parser",
          all(_number_word_to_int(_int_to_number_word(n)) == n for n in range(100))
          and _int_to_number_word(100) is None)
    lines = readme_headline_counts_summary(hl_readme, hl_supported, hl_map, hl_extras)
    check("counts: the summary reports the baseline's device count, brand count and word",
          lines[0] == "validated devices: 3" and lines[1] == "brands: 4 (Four)")
    check("counts: the paste-ready headline for a clean baseline is the README's own line",
          lines[3] == "headline: " + hl_readme.splitlines()[1])
    check("counts: the summary names no unmapped heading on a clean baseline",
          not any(x.startswith("unmapped heading") for x in lines))
    lines = readme_headline_counts_summary(
        hl_readme, hl_supported + "### Delta\n\n| Model Series | C | L | S |\n|--|--|--|--|\n| D1 | USB | \u2713 | ok |\n",
        dict(hl_map, Delta="Delta"), hl_extras)
    check("counts: a new mapped heading raises the counts and is appended after the README's existing order",
          lines[0] == "validated devices: 4" and lines[1] == "brands: 5 (Five)"
          and lines[3].endswith("Beta, Gamma (Alpha plugin), and Delta."))
    lines = readme_headline_counts_summary(
        hl_readme, hl_supported + "### Delta\n\n| Model Series | C | L | S |\n|--|--|--|--|\n| D1 | USB | \u2713 | ok |\n",
        hl_map, hl_extras)
    check("counts: a heading with no map entry is named, not silently dropped from the list",
          lines[1] == "brands: 4 (Four)" and any("unmapped heading '### Delta'" in x for x in lines))
    lines = readme_headline_counts_summary(hl_readme, hl_supported, dict(hl_map, Epsilon="Epsilon"), hl_extras)
    check("counts: a map entry whose heading is gone is named, and its brand is not suggested (review of #694)",
          lines[1] == "brands: 4 (Four)" and "Epsilon" not in lines[3]
          and any(x.startswith("stale map entry '### Epsilon'") for x in lines))
    lines = readme_headline_counts_summary("- **Other.** text\n", hl_supported, hl_map, hl_extras)
    check("counts: with no README headline the list is the map's own order and the current line is reported missing",
          lines[2] == "brand list: Alpha, Beta, DSLRs (via libgphoto2), Gamma (Alpha plugin)"
          and lines[4] == "README.md now: (headline not found)")
    # issue #690: parse failures name the real cause.
    f = _readme_headline_findings(sub(hl_readme, "Four brands", "Twenty One brands"), hl_supported, hl_map, hl_extras)
    check("readme headline: a multi-word brand count reaches the number-word message, not 'headline not found'",
          len(f) == 1 and "'Twenty One' is not a number word" in f[0])
    check("readme headline: a dangling hyphen ('twenty-') and a spaced compound are rejected, not rounded",
          _number_word_to_int("twenty-") is None and _number_word_to_int("twenty one") is None
          and _number_word_to_int("twenty") == 20)
    comma_map = dict(hl_map, GPhoto="Canon, Nikon and Sony DSLRs")
    comma_readme = sub(hl_readme, "DSLRs (via libgphoto2)", "Canon, Nikon and Sony DSLRs")
    check("readme headline: a declared brand item containing a comma is one item, not two unaccounted ones",
          _readme_headline_findings(comma_readme, hl_supported, comma_map, hl_extras) == [])
    check("readme headline: a declared comma item as the LAST entry (after 'and') is still consumed whole",
          _split_readme_brand_list("Alpha, and Canon, Nikon and Sony DSLRs", {"Alpha", "Canon, Nikon and Sony DSLRs"})
          == ["Alpha", "Canon, Nikon and Sony DSLRs"])
    f = _readme_headline_findings(comma_readme, hl_supported, hl_map, hl_extras)
    check("readme headline: an UNDECLARED comma item splits and each half is reported as unaccounted",
          any("'Canon' matches no" in x for x in f) and any("'Nikon and Sony DSLRs' matches no" in x for x in f))
    lines = readme_headline_counts_summary(comma_readme, hl_supported, comma_map, hl_extras)
    check("counts: the summary keeps a declared comma item whole in the paste-ready headline",
          lines[1] == "brands: 4 (Four)" and "Canon, Nikon and Sony DSLRs" in lines[3])

    # check 5 (blocking get_connected() lists) over a literal header and kinds.
    bl_header = ("// The blocking telescopes () list is empty on purpose.\n"
                 "// The wrapper-backed switch drivers () list is empty too.\n")
    bl_kinds = {"bisque_telescope_driver.cpp": "lock-free", "ioptron_telescope_driver.cpp": "lock-free"}
    check("blocking list: an empty telescopes list with every telescope lock-free reports nothing",
          blocking_list_findings(bl_header, bl_kinds) == [])
    f = blocking_list_findings(bl_header, dict(bl_kinds, **{"bisque_telescope_driver.cpp": "driver-mutex"}))
    check("blocking list: a re-locked Bisque getter against an empty list is MISSING FROM THE BLOCKING LIST",
          len(f) == 1 and f[0].startswith("MISSING FROM THE BLOCKING LIST") and "'Bisque'" in f[0])
    f = blocking_list_findings(bl_header.replace("telescopes ()", "telescopes (Bisque and iOptron)"), bl_kinds)
    check("blocking list: Bisque and iOptron named while lock-free is STALE BLOCKING-LIST ENTRY for each",
          all(sum(("'%s'" % n) in x and x.startswith("STALE BLOCKING-LIST ENTRY") for x in f) >= 1
              for n in ("Bisque", "iOptron")))
    f = blocking_list_findings(bl_header.replace("The blocking telescopes (", "The telescopes ("), bl_kinds)
    check("blocking list: a header without the telescopes span is BLOCKING-LIST SPAN NOT FOUND",
          len(f) == 1 and f[0].startswith("BLOCKING-LIST SPAN NOT FOUND"))

    # check 4: VERSION vs README badge, with the ~betaN suffix.
    b_readme = "#### [5.0.0~beta2] - 2026-11-01 &middot; [Changelog](CHANGELOG.md)\n"
    check("version badge: a beta VERSION and its beta badge agree",
          _version_badge_findings("5.0.0~beta2\n", b_readme) == [])
    check("version badge: a beta badge against the base VERSION is flagged",
          len(_version_badge_findings("5.0.0\n", b_readme)) == 1)
    check("version badge: a stable VERSION and badge still agree",
          _version_badge_findings("4.2.0\n", b_readme.replace("5.0.0~beta2", "4.2.0")) == [])
    check("version badge: a missing badge line is flagged",
          len(_version_badge_findings("4.2.0", "# no badge\n")) == 1)
    b0_readme = b_readme.replace("5.0.0~beta2", "5.0.0~beta0")
    check("version badge: ~beta0 is rejected even when VERSION and badge agree",
          _version_badge_findings("5.0.0~beta0\n", b0_readme) == ["VERSION (5.0.0~beta0) is not X.Y.Z or X.Y.Z~betaN (betas count from 1)"])
    check("version badge: a badge that is not X.Y.Z is not a badge",
          len(_version_badge_findings("4.2.0\n", b_readme.replace("5.0.0~beta2", "4.2"))) == 1)
    check("version badge: the form is changelog_fragments.VERSION_RE itself",
          VERSION_FORM_RE is changelog_fragments.VERSION_RE and README_BADGE_RE.search(b_readme) is not None)
    check("version badge: ~beta10 is accepted",
          _version_badge_findings("5.0.0~beta10\n", b_readme.replace("5.0.0~beta2", "5.0.0~beta10")) == [])
    check("updated date: a ~beta0 badge is not read as a release date",
          len(_updated_date_findings("# S\n\n## Updated 2026-11-01\n", b0_readme)) == 1)
    check("updated date: a beta badge still yields its release date",
          _updated_date_findings("# S\n\n## Updated 2026-11-01\n", b_readme) == [])

    # check 16: SUPPORTED-DRIVERS.md Updated date vs README badge date (issue #692).
    ud_supported = "# Supported\n\n## Updated 2026-09-27\nintro\n"
    ud_readme = "#### [4.1.0] - 2026-09-27 &middot; [Changelog](CHANGELOG.md)\n"
    check("updated date: an Updated line equal to the release date is clean",
          _updated_date_findings(ud_supported, ud_readme) == [])
    check("updated date: an Updated line after the release date is clean",
          _updated_date_findings(ud_supported.replace("09-27", "10-02"), ud_readme) == [])
    f = _updated_date_findings(ud_supported.replace("09-27", "09-24"), ud_readme)
    check("updated date: an Updated line behind the release date is flagged with both dates",
          len(f) == 1 and "Updated 2026-09-24" in f[0] and "released 2026-09-27" in f[0])
    f = _updated_date_findings(ud_supported.replace("2026-09-27", "2126-09-27"), ud_readme)
    check("updated date: a mistyped year far ahead of the release date is flagged",
          len(f) == 1 and "more than %d days after" % MAX_UPDATED_DAYS_AHEAD in f[0])
    check("updated date: a year ahead exactly is still clean (the bound is loose by design)",
          _updated_date_findings(ud_supported.replace("2026-09-27", "2027-09-27"), ud_readme) == [])
    f = _updated_date_findings(ud_supported.replace("2026-09-27", "2026-13-40"), ud_readme)
    check("updated date: a non-date Updated value is flagged",
          len(f) == 1 and "not a YYYY-MM-DD date" in f[0])
    f = _updated_date_findings("# Supported\nintro\n", ud_readme)
    check("updated date: a missing Updated line is flagged",
          len(f) == 1 and "could not find the '## Updated" in f[0])
    f = _updated_date_findings(ud_supported, "# no badge\n")
    check("updated date: a missing README badge is flagged rather than passing vacuously",
          len(f) == 1 and "release date in the README.md version badge" in f[0])

    # check 17: documented AlpacaError values vs alpaca_errors.h (issue #682).
    # attempt() so a helper that is missing or has the wrong signature is a
    # FAIL line, not a crash that hides every check after it.
    ec_header = (
        "namespace alpacacore {\n"
        "namespace AlpacaError {\n"
        "    constexpr int Success = 0;\n"
        "    constexpr int PropertyNotImplemented = 0x400; // 1024\n"
        "    constexpr int MethodNotImplemented = 0x400;   // 1024\n"
        "    constexpr int InvalidValue = 0x401;           // 1025\n"
        "    constexpr int InvalidOperation = 0x40B;       // 1035\n"
        "    constexpr int ActionNotImplemented = 0x40C;   // 1036\n"
        "}\n"
        "constexpr int NotAnAlpacaError = 0x999;\n"
        "}\n"
    )
    ec_agents = (
        "| Throw | When |\n"
        "|---|---|\n"
        "| `ActionNotImplemented` | a name not in `SupportedActions` (0x40C, ASCOM's exception). |\n"
        "Throw `MethodNotImplemented` (or `PropertyNotImplemented` for a property, both 0x400).\n"
        "`InvalidOperation` (0x40B) is for a supported member in the wrong state.\n"
        "Use Celestron MC_AUX_GUIDE 0x26 for guiding.\n"
    )
    ec_build = (
        "Error codes: InvalidValue `0x401`, InvalidOperation `0x40B`.\n"
        "Assert `AlpacaError::ActionNotImplemented` (0x40C).\n"
    )
    ec_docs = {"AGENTS.md": ec_agents, ".claude/commands/driver-build.md": ec_build}

    def ec(header=ec_header, docs=None, min_pins=1):
        return attempt("error codes", lambda: _error_code_findings(header, docs or ec_docs, min_pins))

    f = ec()
    check("error codes: the documented values that match the enum are clean", f == [])
    f = ec(min_pins=100)
    check("error codes: the pin floor fires when the extractor finds too few pins",
          f is not None and len(f) == 1 and "found only" in f[0])
    f = ec(docs=dict(ec_docs, **{"AGENTS.md": sub(ec_agents, "(0x40B)", "(0x40A)")}))
    check("error codes: a wrong '<Name> (0x...)' value names the file, the line and the expected value",
          f is not None and len(f) == 1 and "AGENTS.md:5" in f[0] and "InvalidOperation" in f[0]
          and "0x40A" in f[0] and "0x40B" in f[0])
    f = ec(docs=dict(ec_docs, **{"AGENTS.md": sub(ec_agents, "`InvalidOperation` (0x40B)", "`InvalidOperate` (0x40B)")}))
    check("error codes: an unknown '<Name> (0x...)' name names the file and the line and says it is not in the enum",
          f is not None and len(f) == 1 and "AGENTS.md:5" in f[0] and "InvalidOperate" in f[0]
          and "not an AlpacaError" in f[0])
    f = ec(docs=dict(ec_docs, **{".claude/commands/driver-build.md": sub(ec_build, "InvalidValue `0x401`", "InvalidValue `0x402`")}))
    check("error codes: a wrong 'Name `0x...`' list value is reported with the expected value",
          f is not None and len(f) == 1 and ".claude/commands/driver-build.md:1" in f[0] and "0x401" in f[0])
    f = ec(docs=dict(ec_docs, **{"AGENTS.md": sub(ec_agents, "both 0x400", "both 0x401")}))
    check("error codes: a wrong shared '(or `B` ..., both 0x...)' value is reported for both names",
          f is not None and len(f) == 2 and any("MethodNotImplemented" in x for x in f)
          and any("PropertyNotImplemented" in x for x in f))
    f = ec(docs=dict(ec_docs, **{"AGENTS.md": sub(ec_agents, "(0x40C, ASCOM's", "(0x40D, ASCOM's")}))
    check("error codes: a wrong value in an exception-table row is pinned to the row's name, not a name in the cell",
          f is not None and len(f) == 1 and "AGENTS.md:3" in f[0] and "ActionNotImplemented" in f[0]
          and "0x40C" in f[0])
    f = ec(docs=dict(ec_docs, **{".claude/commands/driver-build.md": sub(ec_build, "(0x40C)", "(0x400)")}))
    check("error codes: an 'AlpacaError::' qualified name is pinned like a bare one",
          f is not None and len(f) == 1 and "ActionNotImplemented" in f[0])
    f = ec(header=ec_header.replace("constexpr int NotAnAlpacaError = 0x999;\n", ""),
           docs=dict(ec_docs, **{"AGENTS.md": ec_agents + "`NotAnAlpacaError` (0x999)\n"}))
    check("error codes: a constant outside namespace AlpacaError is not part of the enum",
          f is not None and len(f) == 1 and "NotAnAlpacaError" in f[0])
    f = ec(header="namespace AlpacaError {\n}\n")
    check("error codes: an enum the parser cannot read is a finding, not a pass",
          f is not None and any("no AlpacaError constants" in x for x in f))

    # check 18: the ci-gates CI job roster vs ci.yml (issue #702).
    roster_ci = (
        "on:\n"
        "  pull_request:\n"
        "  push:\n"
        "    branches:\n"
        "      - main\n"
        "jobs:\n"
        "  build-test:\n"
        "    name: Build\n"
        "    steps:\n"
        "      - run: echo\n"
        "  format:\n"
        "    runs-on: x\n"
        "  docs-drift:\n"
        "    runs-on: x\n"
    )
    roster_agents = (
        "# Agent Instructions\n\n"
        "## Continuous Integration and Pre-flight\n\n"
        "Related decision: none.\n\n"
        "- CI (`.github/workflows/ci.yml`) runs on every PR. Its jobs, by id: `build-test` (vendors OFF),\n"
        "  `format` (clang-format over `[stress]` changed lines), and `docs-drift`.\n"
        "- **Layering gate** (`layering`, a later bullet) is not the roster.\n"
    )

    def roster(ci_text=roster_ci, agents_text=roster_agents):
        return attempt("ci roster", lambda: _ci_roster_findings(ci_text, agents_text))

    f = roster()
    check("ci roster: a bullet naming exactly the ci.yml job ids is clean", f == [])
    f = roster(agents_text=sub(roster_agents, "`build-test` (vendors OFF),\n", "\n"))
    check("ci roster: a real ci.yml job missing from the bullet is reported by id",
          f is not None and len(f) == 1 and "'build-test'" in f[0] and "missing" in f[0])
    f = roster(agents_text=sub(roster_agents, "`format` (clang-format", "`clang-format` (clang-format"))
    check("ci roster: a bullet naming a job id ci.yml does not define is reported (and the real id as missing)",
          f is not None and len(f) == 2 and any("'clang-format'" in x and "does not exist" in x for x in f)
          and any("'format'" in x and "missing" in x for x in f))
    f = roster(agents_text=sub(roster_agents, "and `docs-drift`", "`docs-drift` and `docs-drift`"))
    check("ci roster: a job id named twice is reported", f is not None and len(f) == 1 and "twice" in f[0])
    f = roster(ci_text=roster_ci + "  zizmor:\n    runs-on: x\n")
    check("ci roster: a job added to ci.yml without a bullet entry is reported",
          f is not None and len(f) == 1 and "'zizmor'" in f[0])
    f = roster(agents_text=roster_agents.replace("- CI (", "- Continuous integration ("))
    check("ci roster: a missing roster bullet is a finding, not a pass",
          f is not None and len(f) == 1 and "could not find" in f[0])
    f = roster(ci_text=roster_ci.replace("jobs:\n", "workflows:\n"))
    check("ci roster: a ci.yml with no jobs: block is a finding, not a pass",
          f is not None and len(f) == 1 and "no job ids" in f[0])

    zwo = "AlpacaCore/external/ZWO/lib/armv8/"
    blob_a, blob_b = "a" * 40, "b" * 40
    so_clean = [
        ("100755", blob_a, zwo + "libASICamera2.so.1.41"),
        ("120000", blob_b, zwo + "libASICamera2.so"),
        ("100644", blob_a, zwo + "libASICamera2.a"),
    ]

    def dup_so(entries):
        return attempt("vendored so", lambda: _duplicate_so_findings(entries))

    f = dup_so(so_clean)
    check("vendored so: a versioned file plus a symlink is clean", f == [])
    f = dup_so(so_clean[:1] + [("100755", blob_a, zwo + "libASICamera2.so")])
    check("vendored so: the unversioned name committed as a copy is reported, naming both and the fix",
          f is not None and len(f) == 1 and "libASICamera2.so," in f[0] and "libASICamera2.so.1.41" in f[0]
          and "keep libASICamera2.so.1.41" in f[0])
    f = dup_so(so_clean + [("100755", blob_a, "AlpacaCore/external/ZWO/lib/x64/libASICamera2.so.1.41")])
    check("vendored so: the same blob in two directories is not a duplicate", f == [])
    f = dup_so(so_clean + [("100644", _GIT_EMPTY_BLOB, zwo + "libusb.so"),
                           ("100644", _GIT_EMPTY_BLOB, zwo + "libusb.so.0")])
    check("vendored so: two empty placeholders are not reported", f == [])
    f = dup_so([("100755", blob_a, "AlpacaCore/src/libx.so"), ("100755", blob_a, "AlpacaCore/src/libx.so.1")])
    check("vendored so: an index with no vendored .so is a finding, not a pass",
          f is not None and len(f) == 1 and "stale" in f[0])

    cache_wf = (
        "jobs:\n"
        "  asan:\n"
        "    steps:\n"
        "      - uses: actions/cache/restore@x\n"
        "        with:\n"
        "          key: ccache-asan-${{ github.sha }}\n"
        "          restore-keys: ccache-asan-\n"
        "  tsan:\n"
        "    steps:\n"
        "      - uses: actions/cache/restore@x\n"
        "        with:\n"
        "          key: ccache-sanitizers-tsan-${{ github.sha }}\n"
        "          restore-keys: |\n"
        "            ccache-sanitizers-tsan-\n"
        "      - uses: actions/cache/save@x\n"
        "        with:\n"
        "          key: ccache-sanitizers-tsan-${{ github.sha }}\n"
        "  tidy:\n"
        "    steps:\n"
        "      - uses: actions/cache/restore@x\n"
        "        with:\n"
        "          key: ccache-asan-${{ github.sha }}\n"
        "          restore-keys: ccache-asan-\n"
        "  cpp:\n"
        "    steps:\n"
        "      - uses: actions/cache/restore@x\n"
        "        with:\n"
        "          key: cppcheck-${{ runner.os }}-${{ hashFiles('x') }}\n"
        "          restore-keys: cppcheck-${{ runner.os }}-\n"
    )

    def cache_prefixes(text, other=None):
        wf = {"ci.yml": text}
        if other is not None:
            wf["nightly.yml"] = other
        return attempt("cache prefixes", lambda: _cache_prefix_findings(wf))

    f = cache_prefixes(cache_wf)
    check("cache prefixes: distinct prefixes, a shared equal prefix and expression keys are clean", f == [])
    m = cache_wf.replace("ccache-asan-", "ccache-sanitizers-")
    f = cache_prefixes(m)
    check("cache prefixes: the #1355 shape (ASan prefix starts the TSan keys) is reported with both prefixes",
          f is not None and len(f) == 2 and all("'ccache-sanitizers-'" in x and "'ccache-sanitizers-tsan-'" in x
                                                for x in f))
    f = cache_prefixes(cache_wf.replace("            ccache-sanitizers-tsan-\n", "            ccache-\n"))
    check("cache prefixes: a block-style restore-keys entry is read",
          f is not None and len(f) == 2 and all("ci.yml:13 restores by the prefix 'ccache-'" in x for x in f))
    f = cache_prefixes(cache_wf, other="      - with:\n          key: ccache-asan-nightly-${{ github.sha }}\n")
    check("cache prefixes: a key in another workflow file counts",
          f is not None and len(f) == 2 and all("nightly.yml:2" in x for x in f))
    f = cache_prefixes("jobs:\n  a:\n    steps:\n      - run: echo\n")
    check("cache prefixes: no key at all is a finding, not a pass", f is not None and len(f) == 1 and "stale" in f[0])

    # check 24: review job time budget.
    rwf = "jobs:\n  review:\n    runs-on: x\n    timeout-minutes: 90\n    steps:\n      - run: |\n          deadline=$(( $(date +%s) + 45 * 60 ))\n"
    rgates = ("waits up to 45 minutes for them. `claude-review` (90 min:\n  up to 45 waiting for CI, then up to 45 for the review) "
              "is outlasted by the 95-minute verdict poll in")
    rskill = lambda n: "waits up to 45 minutes for CI; a 95-minute budget; DEADLINE=$(( x + ${BUDGET:-%d} ))" % n
    rskills = {REVIEW_SKILLS[0]: rskill(5700), REVIEW_SKILLS[1]: rskill(5700)}

    def budget(wf=rwf, gates=rgates, skills=rskills):
        return attempt("review budget", lambda: _review_budget_findings(wf, gates, skills))

    check("review budget: agreeing figures are clean", budget() == [])
    f = budget(wf=rwf.replace("timeout-minutes: 90", "timeout-minutes: 75"))
    check("review budget: a changed job timeout is reported against ci-gates",
          f is not None and any("90 minutes for the job timeout" in x for x in f))
    f = budget(wf=rwf.replace("45 * 60", "50 * 60"))
    check("review budget: a changed wait deadline is reported", f is not None and any("for the wait" in x for x in f))
    f = budget(gates=rgates.replace("95-minute", "80-minute"))
    check("review budget: a stale ci-gates poll figure is reported", f is not None and any("disagrees" in x for x in f))
    f = budget(skills={REVIEW_SKILLS[0]: rskill(5700), REVIEW_SKILLS[1]: rskill(4800)})
    check("review budget: one skill left on the old BUDGET default is reported",
          f is not None and any("disagrees" in x and "80 minutes" in x for x in f))
    f = budget(skills={REVIEW_SKILLS[0]: rskill(5700).replace("a 95-minute", "a 80-minute"), REVIEW_SKILLS[1]: rskill(5700)})
    check("review budget: stale N-minute prose in a skill is reported", f is not None and any("80 minutes" in x for x in f))
    f = budget(skills={REVIEW_SKILLS[0]: rskill(5700).replace("up to 45", "up to 40"), REVIEW_SKILLS[1]: rskill(5700)})
    check("review budget: a skill's wait prose is checked", f is not None and any("40 minutes for the wait" in x for x in f))
    f = budget(wf=rwf.replace("timeout-minutes: 90", "timeout-minutes: 120"))
    check("review budget: a poll budget not above the job timeout is reported",
          f is not None and any("must be greater" in x for x in f))
    f = budget(wf="jobs:\n  other:\n    timeout-minutes: 5\n")
    check("review budget: an unreadable workflow is a finding, not a pass", f is not None and len(f) == 1 and "stale" in f[0])

    # check 21: AGENTS.md size budget.
    check("agents size: a file at the budget is clean", _agents_md_size_findings(AGENTS_MD_MAX_BYTES) == [])
    f = _agents_md_size_findings(AGENTS_MD_MAX_BYTES + 1)
    check("agents size: one byte over the budget is reported with the size",
          len(f) == 1 and str(AGENTS_MD_MAX_BYTES + 1) in f[0])

    # checks 22 and 23: decision record shape and Adoption rosters (literal fixtures).
    good_record = (
        "# Example\n\nStatus: accepted\n\n## Context\n\nc\n\n## Decision\n\nd\n\n## Rules\n\nintro\n\n"
        "1. Do a thing. Scope: `src/a.cpp`. Checked by: review.\n"
        "2. Do another. Scope: `src/*.cpp`, `docs/x.md`. Checked by: `tests/t.cpp`.\n\n"
        "## Alternatives rejected\n\na\n\n## Consequences\n\nq\n\n## Links\n\nl\n")
    good_index = "- [Example](0001-example.md)\n- [Second](0002-second.md)\n"
    tracked_fixture = ["src/a.cpp", "src/b.cpp", "docs/x.md"]

    fixture_baseline = {"0001-example.md": 1, "0002-second.md": 1}

    def records(first=good_record, second=good_record, index=good_index, extra=None,
                baseline=fixture_baseline, reverse_from=99):
        recs = {"0001-example.md": first, "0002-second.md": second}
        recs.update(extra or {})
        return _decision_record_findings(recs, index, tracked_fixture, baseline, reverse_from)

    check("decision records: the baseline fixture is clean", records() == [])
    retired_record = good_record.replace("Status: accepted", "Status: retired (#1234)").replace(
        "## Rules\n\nintro\n\n1. Do a thing. Scope: `src/a.cpp`. Checked by: review.\n"
        "2. Do another. Scope: `src/*.cpp`, `docs/x.md`. Checked by: `tests/t.cpp`.\n\n", "")
    retired_index = "- [Example](0001-example.md) (retired)\n- [Second](0002-second.md)\n"
    check("decision records: a retired record with no Rules and a suffixed index line is clean",
          records(first=retired_record, index=retired_index) == [])
    f = records(first=good_record.replace("Status: accepted", "Status: retired (#1234)"), index=retired_index)
    check("decision records: a retired record that keeps its Rules is reported",
          any("retired record drops its `## Rules`" in x for x in f))
    f = records(first=good_record.replace("Status: accepted", "Status: retired"))
    check("decision records: retired without the retiring issue or PR is reported", any("line 3" in x for x in f))
    f = records(first=retired_record)
    check("decision records: a retired record whose index line lacks the suffix is reported",
          any("0001-example.md lacks the `(retired)` suffix" in x for x in f))
    f = records(index=retired_index)
    check("decision records: an accepted record whose index line carries the suffix is reported",
          any("0001-example.md carries the `(retired)` suffix" in x for x in f))
    f = records(first=good_record.replace("Checked by: review.", "Checked by: review (no sleep in the getter)."),
                baseline={"0002-second.md": 1})
    check("decision records: `review (<evidence>)` counts as evidence, not a bare review", f == [])
    f = records(baseline={"0002-second.md": 1})
    check("decision records: a bare review above the ratchet is reported",
          any("1 rules say a bare `Checked by: review` but the ratchet allows 0" in x for x in f))
    f = records(first=good_record.replace("Checked by: review.", "Checked by: review (evidence)."))
    check("decision records: a stale ratchet figure is reported so the baseline only falls",
          any("0 bare `Checked by: review` rules remain but DECISION_BARE_REVIEW_BASELINE allows 1" in x for x in f))
    f = records(first=good_record.replace("1. Do a thing.", "1. Do a thing unless asked."))
    check("decision records: an exception word in a rule is reported", any("rule 1 says `unless`" in x for x in f))
    f = records(first=good_record.replace("1. Do a thing. Scope: `src/a.cpp`. Checked by: review.",
                                          "1. Do a thing until told. Scope: `src/a.cpp`. Checked by: review. <!-- allowed -->"))
    check("decision records: the allowed marker admits a bounded rule", f == [])
    f = records(first=good_record.replace("Scope: `src/a.cpp`", "Scope: `**`"))
    check("decision records: scope `**` outside the catalogue is reported",
          any("scope `**` is allowed only in" in x for x in f))
    f = records(first=good_record.replace("Scope: `src/a.cpp`", "Scope: `**`"),
                extra={"0013-designs-to-avoid.md": good_record.replace("Scope: `src/a.cpp`", "Scope: `**`")})
    check("decision records: scope `**` inside the catalogue is not the finding",
          not any("0013-designs-to-avoid.md: rule 1 scope `**`" in x for x in f))
    f = records(reverse_from=2)
    check("decision records: a record from the cut-over number on without How to reverse is reported",
          any("0002-second.md: a record from 0002 on needs a `## How to reverse`" in x for x in f)
          and not any("0001-example.md: a record from" in x for x in f))
    f = records(second=good_record.replace("## Links", "## How to reverse\n\nr\n\n## Links"), reverse_from=2)
    check("decision records: How to reverse satisfies the cut-over", f == [])
    f = records(first=good_record.replace("Status: accepted", "Status: superseded by 0002").replace(
        "## Rules\n\nintro\n\n1. Do a thing. Scope: `src/a.cpp`. Checked by: review.\n"
        "2. Do another. Scope: `src/*.cpp`, `docs/x.md`. Checked by: `tests/t.cpp`.\n\n", ""),
        baseline={"0002-second.md": 1})
    check("decision records: a superseding record below the cut-over without How to reverse is reported",
          any("0002-second.md: supersedes 0001-example.md and needs a `## How to reverse`" in x for x in f))
    f = records(first=good_record.replace("## Rules\n\nintro\n\n1. Do a thing. Scope: `src/a.cpp`. Checked by: review.\n"
                                          "2. Do another. Scope: `src/*.cpp`, `docs/x.md`. Checked by: `tests/t.cpp`.\n\n", ""))
    check("decision records: a missing Rules section is reported", any("missing section `## Rules`" in x for x in f))
    f = records(first=good_record.replace("Status: accepted", "Status: approved"))
    check("decision records: an unknown status is reported", any("line 3" in x for x in f))
    f = records(first=good_record.replace("Status: accepted", "Status: superseded by 0009"))
    check("decision records: a supersede target that does not exist is reported", any("0009, which does not exist" in x for x in f))
    f = records(first=good_record.replace("Status: accepted", "Status: superseded by 0002").replace(
        "## Rules\n\nintro\n\n1. Do a thing. Scope: `src/a.cpp`. Checked by: review.\n"
        "2. Do another. Scope: `src/*.cpp`, `docs/x.md`. Checked by: `tests/t.cpp`.\n\n", ""),
        second=good_record.replace("## Links", "## How to reverse\n\nr\n\n## Links"), baseline={"0002-second.md": 1})
    check("decision records: a superseded record needs no Rules section", f == [])
    f = records(first=good_record.replace(" Checked by: review.", "."))
    check("decision records: a rule with no `Checked by:` is reported", any("rule line does not end" in x for x in f))
    f = records(first=good_record.replace("Scope: `src/a.cpp`", "Scope: `src/missing.cpp`"))
    check("decision records: a scope that matches no tracked file is reported", any("matches no tracked file" in x for x in f))
    f = records(first=good_record.replace("2. Do another.", "3. Do another."))
    check("decision records: a gap in rule numbers is reported", any("out of sequence" in x for x in f))
    f = records(first=good_record.replace("## Consequences", "## Notes"))
    check("decision records: an unknown section is reported", any("unknown section `## Notes`" in x for x in f))
    f = records(first=good_record.replace("## Alternatives rejected\n\na\n\n## Consequences\n\nq\n\n",
                                          "## Consequences\n\nq\n\n## Alternatives rejected\n\na\n\n"))
    check("decision records: sections out of order are reported", any("out of order" in x for x in f))
    f = records(index="- [Example](0001-example.md)\n")
    check("decision records: a record missing from the README index is reported", any("not indexed: 0002-second.md" in x for x in f))
    f = records(extra={"0004-gap.md": good_record}, index=good_index + "- [Gap](0004-gap.md)\n")
    check("decision records: a gap in record numbers is reported", any("no gap or repeat" in x for x in f))

    adoption_body = (
        "\nScan: `src/*_driver.cpp`\nSymbol: `Clock`\n\n| Driver | Source | State | Issue | Notes |\n"
        "| --- | --- | --- | --- | --- |\n"
        "| A | `src/a_driver.cpp` | adopted | #1 | |\n"
        "| B | `src/b_driver.cpp` | partial | #2 | |\n"
        "| C | `src/c_driver.cpp` | planned | — | |\n"
        "| D | — | planned | — | |\n")
    adoption_files = {"src/a_driver.cpp": "Clock c;", "src/b_driver.cpp": "util::Clock& x;",
                      "src/c_driver.cpp": "nothing", "src/z_helper.cpp": "Clock c;"}

    def adoption(body=adoption_body, files=None):
        return _adoption_findings("rec", body, adoption_files if files is None else files)

    check("adoption: the baseline fixture is clean", adoption() == [])
    f = adoption(files=dict(adoption_files, **{"src/e_driver.cpp": "Clock c;"}))
    check("adoption: a scanned file that names the symbol with no row is reported", any("src/e_driver.cpp" in x and "no Adoption row" in x for x in f))
    f = adoption(files=dict(adoption_files, **{"src/a_driver.cpp": "nothing"}))
    check("adoption: an adopted row whose source lacks the symbol is reported", any("row A is adopted" in x for x in f))
    f = adoption(files=dict(adoption_files, **{"src/c_driver.cpp": "Clock c;"}))
    check("adoption: a planned row whose source already uses the symbol is reported", any("row C is planned" in x for x in f))
    f = adoption(body=adoption_body.replace("| partial | #2 |", "| partial | — |"))
    check("adoption: a partial row with no issue is reported", any("row B is partial" in x for x in f))
    f = adoption(body=adoption_body.replace("`src/c_driver.cpp`", "`src/gone_driver.cpp`"))
    check("adoption: a source that is not tracked is reported", any("not a tracked AlpacaCore/ or AlpacaHTTP/" in x for x in f))
    f = adoption(body=adoption_body.replace("| adopted |", "| done |"))
    check("adoption: an unknown state is reported", any("state 'done'" in x for x in f))
    f = adoption(body=adoption_body.replace("Scan: `src/*_driver.cpp`\n", ""))
    check("adoption: a missing Scan line is reported", any("no `Scan:" in x for x in f))
    f = adoption(body=adoption_body.replace("| D | — | adopted", "| D | — | adopted").replace("| D | — | planned", "| D | — | adopted"))
    check("adoption: only a planned row may omit its source", any("row D names no source" in x for x in f))
    per_row = ("\nScan: `t/locked_*.h`\n\n| Driver | Symbol | Source | State | Issue |\n| --- | --- | --- | --- | --- |\n"
               "| X | `XSDK` | `t/locked_x.h` | adopted | #1 |\n")
    f = _adoption_findings("rec", per_row, {"t/locked_x.h": "class L : public XSDK", "t/locked_y.h": "class M : public YSDK"})
    check("adoption: with per-row symbols, every scanned file needs a row", any("t/locked_y.h" in x for x in f))
    f = _adoption_findings("rec", per_row, {"t/locked_x.h": "class L : public XSDK"})
    check("adoption: a per-row symbol fixture with every file listed is clean", f == [])
    both = per_row.replace("\n\n| Driver", "\nSymbol: `XSDK`\n\n| Driver")
    f = _adoption_findings("rec", both, {"t/locked_x.h": "class L : public XSDK", "t/locked_y.h": "class M : public YSDK"})
    check("adoption: with a Symbol line, a Symbol column overrides per row and an unrelated scanned file needs no row",
          f == [])
    mixed = ("\nScan: `src/*_driver.cpp`\nSymbol: `Clock`\n\n| Driver | Symbol | Source | State | Issue |\n"
             "| --- | --- | --- | --- | --- |\n| A |  | `src/a_driver.cpp` | adopted | #1 |\n"
             "| B | `Other` | `src/b_driver.cpp` | adopted | #2 |\n")
    mixed_files = {"src/a_driver.cpp": "Clock c;", "src/b_driver.cpp": "Other o;", "src/c_driver.cpp": "nothing"}
    check("adoption: a blank Symbol cell uses the Symbol line, and a file naming neither needs no row",
          _adoption_findings("rec", mixed, mixed_files) == [])
    f = _adoption_findings("rec", mixed, dict(mixed_files, **{"src/a_driver.cpp": "Other o;"}))
    check("adoption: a blank Symbol cell is checked against the Symbol line", any("row A is adopted" in x for x in f))
    polled = ("\nScan: `src/*_driver.cpp`\nSymbol: `StateSnapshot`\n\n| Driver | Symbol | Source | State | Issue |\n"
              "| --- | --- | --- | --- | --- |\n| P | `PolledState` | `src/p_driver.cpp` | adopted | #1 |\n"
              "| S | `StateSnapshot` | `src/s_driver.cpp` | adopted | #2 |\n")
    polled_files = {"src/p_driver.cpp": "util::PolledState<F> state_;", "src/s_driver.cpp": "util::StateSnapshot<F> snap_;"}
    check("adoption: a PolledState row and a StateSnapshot row each hold to their own symbol",
          _adoption_findings("rec", polled, polled_files) == [])
    f = _adoption_findings("rec", polled, dict(polled_files, **{"src/p_driver.cpp": "util::StateSnapshot<F> snap_;"}))
    check("adoption: a PolledState row whose source names only StateSnapshot is reported",
          any("row P is adopted but no source names `PolledState`" in x for x in f))
    f = adoption(files=dict(adoption_files, **{"src/a_driver.cpp": "// Clock is not used here\nint x;"}))
    check("adoption: a symbol named only in a comment does not count as adoption", any("row A is adopted" in x for x in f))
    f = adoption(files=dict(adoption_files, **{"src/c_driver.cpp": "/* Clock, one day */ int x;"}))
    check("adoption: a planned row whose source names the symbol only in a comment is clean", f == [])

    # check 25: README authored-line badge (issue #1465).
    badge = authored_badge_line(150000)
    ab_readme = "# T\n\n" + badge + "\n"
    check("authored badge: the generated line is what the extractor reads",
          AUTHORED_BADGE_RE.search(ab_readme) is not None and "150k%20lines" in badge)
    check("authored badge: a badge equal to the count is clean", _authored_badge_findings(ab_readme, 150000) == [])
    check("authored badge: an ordinary PR's churn inside the tolerance is clean",
          _authored_badge_findings(ab_readme, 150000 + 3000) == [] and _authored_badge_findings(ab_readme, 150000 - 3000) == [])
    f = _authored_badge_findings(ab_readme, 170000)
    check("authored badge: a count beyond the tolerance is flagged with the paste-ready line",
          len(f) == 1 and "says 150k" in f[0] and "170k%20lines" in f[0])
    f = _authored_badge_findings(ab_readme, 130000)
    check("authored badge: a count far below the badge is flagged too", len(f) == 1 and "130k%20lines" in f[0])
    f = _authored_badge_findings("# T\n", 150000)
    check("authored badge: a missing badge is a finding, not a pass", len(f) == 1 and "could not find" in f[0])
    f = _authored_badge_findings(ab_readme.replace("Authored%20code", "Code"), 150000)
    check("authored badge: a renamed badge label is not found", len(f) == 1 and "could not find" in f[0])
    f = _authored_badge_findings(ab_readme, 10)
    check("authored badge: a near-zero count is a floor finding", len(f) == 1 and "floor" in f[0])
    check("authored paths: first-party code counts",
          all(is_authored_path(p) for p in ("AlpacaCore/src/a.cpp", "AlpacaHTTP/web/app.js", "scripts/x.py",
                                            "AlpacaHTTP/tests/t.cpp", "AlpacaCore/include/alpacacore/a.h")))
    check("authored paths: vendored SDKs and the bundled libgphoto2 do not count",
          not is_authored_path("AlpacaCore/external/ZWO/inc/ASICamera2.h")
          and not is_authored_path("AlpacaCore/external/libgphoto2/src/x.c"))
    check("authored paths: docs, data and files outside the roots do not count",
          not any(is_authored_path(p) for p in ("README.md", "docs/x.py", "AlpacaCore/CMakeLists.txt",
                                                "AlpacaCore/conformu/a.txt", ".github/scripts/c.py")))
    check("authored lines: blank lines and excluded files are not counted",
          count_authored_lines({"scripts/a.py": "a\n\n  \nb\n", "AlpacaCore/external/x.h": "x\ny\n",
                                "README.md": "z\n"}) == 2)

    from check_instruction_structure import self_test as instruction_self_test
    instruction_self_test()

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
    if "--badge-date" in sys.argv:
        # The README badge's release date, by the same regex check 16 reads it
        # with; scripts/build_deb.sh dates a beta's changelog stanza from it.
        b = README_BADGE_DATE_RE.search(read("README.md"))
        if not b or _iso_date(b.group("date")) is None:
            print("could not find a dated version badge line in README.md", file=sys.stderr)
            sys.exit(1)
        print(b.group("date"))
        sys.exit(0)
    if "--counts" in sys.argv:
        sys.exit(print_counts())
    if "--badge" in sys.argv:
        sys.exit(print_badge())
    sys.exit(main())
