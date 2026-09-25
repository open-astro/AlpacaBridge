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

Checks:
  1. Every ALPACACORE_ENABLE_* CMake option is documented in the
     docs/development.md build-options table.
  2. The zizmor pinned version + sha256 are identical between ci.yml and
     ci_preflight.sh.
  3. The cppcheck --suppress list is identical between ci.yml and
     ci_preflight.sh (AGENTS.md: "Keep the cppcheck --suppress list
     identical between ci.yml and ci_preflight.sh").
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
     shared mutex through locked() -- except cancel_exposure(), which must
     take its own cancel_mutex_ and must NOT go through locked() (issue #339:
     production's cancel skips the per-handle mutex so it can interrupt a
     download blocked on the same handle) -- and the forward sweep in
     test_qhy_fake_sdk.cpp drives all of them.
  7. Every relative path referenced in AGENTS.md, CONTEXT.md, scoped instructions,
     docs/agents/ agent-skills config, .claude/skills/ Claude skills, and
     docs/failures/ and docs/decisions/
     inline code spans (`` `AlpacaCore/...` ``, `` `scripts/...` ``,
     `` `docs/...` ``, etc.) that looks like a real repo path actually exists.
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
"""

import glob
import hashlib
import re
import subprocess
import sys
from pathlib import Path

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

def check_version_matches_readme():
    failures = []
    version = read("VERSION").strip()
    readme = read("README.md")

    m = re.search(r"^####\s*\[([0-9.]+)\]\s*-\s*[0-9-]+\s*&middot;\s*\[Changelog\]", readme, re.MULTILINE)
    if not m:
        failures.append("could not find the version badge line in README.md")
        return failures

    badge_version = m.group(1)
    if badge_version != version:
        failures.append(
            "VERSION (%s) does not match the README badge version (%s)"
            % (version, badge_version)
        )
    return failures


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
# lock-free majority is not named anywhere, by design. The four
# wrapper-backed switch drivers (iOptron iMate PowerBox, ToupTek StellaVita,
# ASIAIR, ASIAIR Plus) are the exception: they keep their entries although
# they answer get_connected() lock-free since #382, because the entry is what
# lets the gate report MISSING FROM THE BLOCKING LIST if one of their wrappers
# ever re-locks is_open(). Do not remove them on the STALE BLOCKING-LIST ENTRY
# advice.
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
    "celestron_telescope_driver.cpp": ("Celestron", "telescopes"),
    "ioptron_telescope_driver.cpp": ("iOptron", "telescopes"),
    "onstep_telescope_driver.cpp": ("OnStep", "telescopes"),
    "skywatcher_telescope_driver.cpp": ("Sky-Watcher", "telescopes"),
    "ioptron_switch_driver.cpp": ("iOptron iMate PowerBox", "switches"),
    "touptek_switch_driver.cpp": ("ToupTek StellaVita", "switches"),
    "zwo_asiair_switch_driver.cpp": ("ZWO ASIAIR", "switches"),
    "zwo_asiair_plus_switch_driver.cpp": ("ASIAIR Plus", "switches"),
}

# Where each list lives in async_connectable.h, as (start marker, end marker).
# Both must be found or the check fails loudly: a header rewrite that moves
# them must not silently turn this gate into a no-op.
BLOCKING_LIST_SPANS = {
    "telescopes": ("create that hazard are the", "telescopes."),
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
    if which == "telescopes":
        body = span.split("create that hazard are the", 1)[1]
        body = body.rsplit("telescopes.", 1)[0]
    else:
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
    as a method signature. No raw string literals exist in either header."""
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


def check_blocking_get_connected_list():
    kinds, failures = classify_get_connected_bodies()
    if not kinds:
        return ["No get_connected() overrides found under AlpacaCore/src/vendors -- the scan is broken."]

    header = read("AlpacaCore/include/alpacacore/async_connectable.h")
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

    # cancel_exposure is the one documented exception (open-astro#339): the real
    # QHYSDKWrapper::cancel_exposure() deliberately skips the per-handle call
    # mutex so it can interrupt a GetQHYCCDSingleFrame already blocked on the
    # same handle, and routing it through the shared mutex here inverted that
    # invariant. It still takes A mutex -- its own -- so it is not an unlocked
    # forward and the fake stays non-racy; it just does not queue behind the
    # call it exists to interrupt.
    CANCEL_EXEMPT = "cancel_exposure"
    for name in sorted(set(locked_methods) & interface_methods):
        body = locked_methods[name]
        if name == CANCEL_EXEMPT:
            # Mechanical, not a substring: the body must take the lock_guard on
            # cancel_mutex_ AND must not go through locked(). A body that does
            # both (shared mutex plus a mention of cancel_mutex_) is the
            # inverted shape #339 fixed, and a substring test passed it.
            # `lock_guard<std::mutex>` or CTAD `lock_guard` both count; any
            # mention of the shared `mutex_` (locked() or a hand-rolled guard)
            # is the inverted shape, so it is rejected by name, not by idiom.
            takes_own = re.search(r"lock_guard\s*(?:<\s*std::mutex\s*>)?\s*\w+\s*\(\s*cancel_mutex_\s*\)", body)
            touches_shared = "locked(" in body or re.search(r"(?<![\w])mutex_\b", body)
            if not takes_own or touches_shared:
                failures.append(
                    "LockedQHYSDK::cancel_exposure() must take cancel_mutex_ -- its own lock, separate "
                    "from the shared one, per open-astro#339. Through the shared mutex it queues behind "
                    "the in-flight call it exists to interrupt; with no mutex at all it becomes the one "
                    "racy forward in the decorator.")
            continue
        if "locked(" not in body:
            failures.append(
                "UNLOCKED FORWARD: LockedQHYSDK::%s() does not go through locked(). The decorator exists "
                "only to take the mutex -- an unlocked forward makes the fake racy under a [stress] storm "
                "and produces a ThreadSanitizer report naming the FAKE, which is the confusion the "
                "decorator was built to prevent." % name)

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
MIN_AGENTS_MD_PATH_REFS = 40
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
        # Markdown anchors / fragments (`docs/x.md#section`), glob patterns,
        # and template placeholders (`AlpacaCore/src/vendors/<vendor>/...`)
        # aren't real filesystem paths.
        if "#" in path or "*" in path or "<" in path or ">" in path:
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
    "fake_pty_write.h": "bounded pty-master write and PtyPair, shared by the pty-backed fakes",
    "fake_raw_decoder.h": "fake RawDecoder seam for the gphoto camera, used together with fake_gphoto_sdk.h",
}


def _fake_roster_findings(header_text, disk_fakes, helpers=None):
    helpers = HELPER_FAKES if helpers is None else helpers
    failures = []
    m = ROSTER_ARRAY_RE.search(_strip_comments(header_text))
    rows = ROSTER_ROW_RE.findall(m.group(1)) if m else []
    if not rows:
        return ["AlpacaCore/tests/contract_sweep.h has no kFakeConnectableRoster rows (or the array moved): "
                "the roster cannot be pinned to the fakes on disk"]
    rostered = {fake for _, _, fake in rows}
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

    roster_hdr = ("inline constexpr FakeRosterRow kFakeConnectableRoster[] = {\n"
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
    check("fake roster: a missing roster array is a finding, not a silent pass",
          len(_fake_roster_findings("// nothing here\n", roster_disk, roster_helpers)) == 1)
    check("fake roster: a row inside a comment is not a row",
          len(_fake_roster_findings("// {\"a\", \"b\", \"fake_x.h\"},\n" + roster_hdr, roster_disk, roster_helpers)) == 0)

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
    sys.exit(main())
