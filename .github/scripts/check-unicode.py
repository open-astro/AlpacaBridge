#!/usr/bin/env python3
"""Scan tracked text files for dangerous/invisible Unicode characters.

Guards against "Trojan Source" attacks (CVE-2021-42574), where bidirectional
control characters or invisible code points make source render differently from
how the compiler reads it.

Run from the repo root:  python3 .github/scripts/check-unicode.py
Regression guard for the scanner itself (no repo state needed):
                         python3 .github/scripts/check-unicode.py --self-test
Exits non-zero (and prints file:line:col U+XXXX) if any forbidden code point is
found. Intentional control characters in tests should be written as escapes
(e.g. "\\u200E") rather than literal characters so the source stays clean.

Detection does NOT depend on a file decoding cleanly as UTF-8, and it does not
silently pass anything it fails to inspect -- both are bypasses a security
control must not have:

  * Genuine binaries are skipped (a NUL byte, and no UTF-16/UTF-32 BOM).
  * UTF-8 and BOM-declared UTF-16 / UTF-32 text are decoded and scanned for
    precise file:line:col reporting.
  * Any remaining undecodable *text* file is flagged (a non-UTF-8 text file is
    anomalous in a UTF-8 tree) and byte-scanned for the forbidden code points
    in every encoding that can carry them.
  * A tracked file that cannot be read at all (broken symlink, bad perms) is a
    hard finding -- never skipped silently. Directory targets of tracked
    symlinks (e.g. macOS .framework internals) are not files and are skipped.

Known accepted limitation: a UTF-16/UTF-32 text file with NO byte-order mark is
indistinguishable from binary by the NUL heuristic and is skipped. Such a file
is not valid source for this tree's toolchains (which read UTF-8), so it is not
a realistic vector here.
"""

import subprocess
import sys


# Forbidden code points keyed by integer value, so this scanner is itself pure
# ASCII and never trips over its own table. Limited to characters that are
# unambiguously suspect in a source tree: bidi controls plus zero-width /
# invisible spaces. ZWJ/ZWNJ (U+200C/U+200D) are intentionally omitted because
# they appear in legitimate emoji sequences in Markdown.
FORBIDDEN = {
    # Bidirectional formatting controls (the core Trojan Source vector)
    0x202A: "LEFT-TO-RIGHT EMBEDDING",
    0x202B: "RIGHT-TO-LEFT EMBEDDING",
    0x202C: "POP DIRECTIONAL FORMATTING",
    0x202D: "LEFT-TO-RIGHT OVERRIDE",
    0x202E: "RIGHT-TO-LEFT OVERRIDE",
    0x2066: "LEFT-TO-RIGHT ISOLATE",
    0x2067: "RIGHT-TO-LEFT ISOLATE",
    0x2068: "FIRST STRONG ISOLATE",
    0x2069: "POP DIRECTIONAL ISOLATE",
    0x200E: "LEFT-TO-RIGHT MARK",
    0x200F: "RIGHT-TO-LEFT MARK",
    0x061C: "ARABIC LETTER MARK",
    # Invisible / zero-width spaces
    0x200B: "ZERO WIDTH SPACE",
    0x2060: "WORD JOINER",
    0xFEFF: "ZERO WIDTH NO-BREAK SPACE (BOM)",
}

# Encodings a hidden character could realistically reach a toolchain through.
# Every forbidden code point is > U+00FF, so single-byte legacy encodings
# (Latin-1, Windows-1252, ...) physically cannot represent them -- the Unicode
# transformation formats are the only byte-level vectors. Both endiannesses of
# UTF-16/UTF-32 are covered.
BYTE_ENCODINGS = ("utf-8", "utf-16-le", "utf-16-be", "utf-32-le", "utf-32-be")


def _build_forbidden_bytes():
    """{byte sequence: [(codepoint, encoding), ...]} for the raw-byte backstop.

    A list of hits per sequence (not a single value) so that if two code points
    ever encode to identical bytes the table stays correct instead of silently
    dropping one. No collision exists today; this keeps it safe as FORBIDDEN
    grows. Wrapped in a function so the loop variables don't leak to module
    scope.
    """
    table = {}
    for cp in FORBIDDEN:
        for enc in BYTE_ENCODINGS:
            table.setdefault(chr(cp).encode(enc), []).append((cp, enc))
    return table


FORBIDDEN_BYTES = _build_forbidden_bytes()

# Paths under these prefixes are skipped.
#
# The vendored SDK trees under AlpacaCore/external/ are third-party camera
# vendor drops we don't author and can't clean up; some ship BOMs and non-UTF-8
# demo sources. They're excluded so the scan covers only code we own. Authored
# source (AlpacaCore/, AlpacaHTTP/, scripts, docs) is always scanned. Add a
# prefix here only if an upstream import legitimately needs an exception.
EXCLUDE_PREFIXES = (
    "AlpacaCore/external/",
)

# Byte-order marks. UTF-32 must be tested before UTF-16: the UTF-32-LE BOM
# (FF FE 00 00) starts with the UTF-16-LE BOM (FF FE), so a naive UTF-16 check
# would misdecode UTF-32-LE as UTF-16 garbage.
_UTF32_BOMS = (b"\xff\xfe\x00\x00", b"\x00\x00\xfe\xff")
_UTF16_BOMS = (b"\xff\xfe", b"\xfe\xff")


def tracked_files():
    out = subprocess.run(
        ["git", "ls-files", "-z"],
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",  # decode paths as UTF-8 regardless of runner locale
    ).stdout
    for path in out.split("\0"):
        if path and not path.startswith(EXCLUDE_PREFIXES):
            yield path


def looks_binary(data):
    """A NUL byte means binary -- unless a UTF-16/UTF-32 BOM marks it as text."""
    if not data:
        return False
    if data.startswith(_UTF32_BOMS) or data.startswith(_UTF16_BOMS):
        return False
    return b"\x00" in data


def bom_text_encoding(data):
    """Return the Python codec for a BOM-declared UTF-16/UTF-32 file, else None.

    The plain "utf-16" / "utf-32" codecs consume the BOM and auto-select the
    endianness from it. UTF-32 is checked first (see _UTF32_BOMS).
    """
    if data.startswith(_UTF32_BOMS):
        return "utf-32"
    if data.startswith(_UTF16_BOMS):
        return "utf-16"
    return None


def scan_text(path, text, findings):
    """Report forbidden code points in decoded text with 1-based line:col."""
    line = 1
    col = 0
    for ch in text:
        if ch == "\n":
            line += 1
            col = 0
            continue
        if ch == "\r":
            # CRLF: the \r is not a column -- let the following \n advance the
            # line so reported columns stay correct on Windows-style endings.
            continue
        col += 1
        name = FORBIDDEN.get(ord(ch))
        if name is not None:
            findings.append("%s:%d:%d: U+%04X %s" % (path, line, col, ord(ch), name))


def main():
    findings = []
    scanned = 0
    for path in tracked_files():
        scanned += 1
        try:
            with open(path, "rb") as fh:
                data = fh.read()
        except IsADirectoryError:
            # Tracked symlink that points at a directory (e.g. the macOS
            # .framework internals under thirdparty/) -- not a scannable file.
            continue
        except OSError as e:
            # A tracked file we cannot read (broken symlink, bad perms) is a
            # hard finding -- a security scanner must not pass files it never
            # inspected, so this fails the job rather than being skipped.
            findings.append("%s: unreadable tracked file (%s)" % (path, e))
            continue

        if looks_binary(data):
            continue

        # UTF-8 is the tree's canonical encoding; decode for precise locations.
        try:
            scan_text(path, data.decode("utf-8"), findings)
            continue
        except UnicodeDecodeError:
            pass

        # Not valid UTF-8 but not binary. Honour a declared UTF-16/UTF-32 BOM.
        enc = bom_text_encoding(data)
        if enc is not None:
            try:
                scan_text(path, data.decode(enc), findings)
                continue
            except UnicodeDecodeError:
                pass

        # Undecodable text: do NOT silently skip (that was the old bypass).
        # Flag the anomaly and byte-scan for forbidden sequences as a backstop.
        findings.append("%s: non-UTF-8 text file (cannot verify cleanly; treat as suspect)" % path)
        for seq, hits in FORBIDDEN_BYTES.items():
            if seq in data:
                for cp, enc in hits:
                    findings.append("%s: contains %s-encoded U+%04X %s" % (path, enc, cp, FORBIDDEN[cp]))

    # A scan that listed nothing is a broken gate, not a clean tree: if
    # tracked_files() ever stops yielding (a changed git invocation, a wrong
    # working directory), this must fail rather than print the OK line.
    # `scanned` counts files LISTED, not files whose text was inspected: a
    # tree of nothing but looks_binary() files would pass this floor. The
    # floor targets an empty `git ls-files`, which is the failure seen.
    if scanned == 0:
        findings.append("no tracked files were scanned -- the file list is empty, "
                        "so nothing was checked (run from the repo root)")

    if findings:
        print("Forbidden Unicode characters found:\n")
        for f in findings:
            print("  " + f)
        print("\n%d finding(s). See CVE-2021-42574 (Trojan Source)." % len(findings))
        return 1

    print("Unicode scan OK -- no forbidden characters in %d file(s)." % scanned)
    return 0


def self_test():
    """Regression guard for the scanner, run with --self-test.

    The gate's failure mode is silence: a regression in scan_text(),
    looks_binary() or bom_text_encoding() stops rejecting the characters
    the scan exists to reject, and a tracked_files() that yields nothing
    reads as a clean tree (issue #449). So this pins three layers:

      * the FORBIDDEN table itself (every code point the gate is documented
        to reject, so deleting a row fails here) and each pure helper;
      * tracked_files() against a REAL temporary git repository, because
        the -z splitting, the UTF-8 path decoding and EXCLUDE_PREFIXES are
        all decided by what git prints;
      * main() end to end over fixture files with tracked_files() patched,
        so the wiring (binary skip, BOM decode, non-UTF-8 backstop,
        unreadable-file finding, the empty-scan floor) is covered too.
    """
    import os
    import tempfile

    checks = []

    def check(name, condition):
        checks.append((name, condition))

    # --- the table: the documented set, spelled out so a deleted row fails ---
    expected_forbidden = [
        0x202A, 0x202B, 0x202C, 0x202D, 0x202E,          # embeddings / overrides
        0x2066, 0x2067, 0x2068, 0x2069,                  # isolates
        0x200E, 0x200F, 0x061C,                          # marks
        0x200B, 0x2060, 0xFEFF,                          # invisible spaces
    ]
    check("FORBIDDEN carries every documented code point",
          sorted(FORBIDDEN) == sorted(expected_forbidden))
    check("FORBIDDEN omits ZWJ/ZWNJ (legitimate in emoji sequences)",
          0x200C not in FORBIDDEN and 0x200D not in FORBIDDEN)
    check("FORBIDDEN_BYTES covers every code point in every byte encoding",
          sorted({(cp, enc) for hits in FORBIDDEN_BYTES.values() for cp, enc in hits})
          == sorted((cp, enc) for cp in expected_forbidden for enc in BYTE_ENCODINGS))

    # --- scan_text(): every code point fires, with 1-based line:col ---------
    for cp in expected_forbidden:
        findings = []
        scan_text("f", "ab\ncd%sef\n" % chr(cp), findings)
        check("scan_text reports U+%04X at line 2 col 3" % cp,
              findings == ["f:2:3: U+%04X %s" % (cp, FORBIDDEN.get(cp, "<not in FORBIDDEN>"))])
    findings = []
    scan_text("f", "plain ascii\nand caf\u00e9 and \u4e2d\u6587 and \U0001F600\n", findings)
    check("scan_text is quiet on ordinary non-ASCII text", findings == [])
    findings = []
    scan_text("f", "a\r\nb\u202ec\r\n", findings)
    check("scan_text reports the right line:col on CRLF endings",
          findings == ["f:2:2: U+202E RIGHT-TO-LEFT OVERRIDE"])
    findings = []
    scan_text("f", "\u200b\u2060", findings)
    check("scan_text reports two characters on one line separately",
          findings == ["f:1:1: U+200B ZERO WIDTH SPACE", "f:1:2: U+2060 WORD JOINER"])
    findings = []
    scan_text("f", "emoji \U0001F468\u200d\U0001F469 ok\n", findings)
    check("scan_text passes a ZWJ emoji sequence", findings == [])

    # --- looks_binary() / bom_text_encoding() ------------------------------
    check("looks_binary: empty file is not binary", looks_binary(b"") is False)
    check("looks_binary: plain text is not binary", looks_binary(b"hello\n") is False)
    check("looks_binary: a NUL byte means binary", looks_binary(b"he\x00llo") is True)
    for bom, codec in ((b"\xff\xfe", "utf-16"), (b"\xfe\xff", "utf-16"),
                       (b"\xff\xfe\x00\x00", "utf-32"), (b"\x00\x00\xfe\xff", "utf-32")):
        data = bom + b"a\x00b\x00"
        check("looks_binary: %s BOM %r marks NUL-bearing data as text" % (codec, bom),
              looks_binary(data) is False)
        check("bom_text_encoding: %r -> %s" % (bom, codec),
              bom_text_encoding(data) == codec)
    check("bom_text_encoding: UTF-32-LE BOM is not misread as UTF-16-LE",
          bom_text_encoding(b"\xff\xfe\x00\x00rest") == "utf-32")
    check("bom_text_encoding: no BOM -> None", bom_text_encoding(b"\xef\xbb\xbfabc") is None)

    # --- tracked_files() over a real git repository -------------------------
    import subprocess as sp
    real_cwd = os.getcwd()
    with tempfile.TemporaryDirectory() as tmp:
        repo = os.path.join(tmp, "repo")
        os.mkdir(repo)
        env = dict(os.environ,
                   GIT_AUTHOR_NAME="self-test", GIT_AUTHOR_EMAIL="self-test@example.invalid",
                   GIT_COMMITTER_NAME="self-test", GIT_COMMITTER_EMAIL="self-test@example.invalid",
                   GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_SYSTEM="/dev/null")

        def git(*args):
            sp.run(["git", "-C", repo] + list(args), check=True,
                   capture_output=True, text=True, env=env)

        def write(rel, data):
            path = os.path.join(repo, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as fh:
                fh.write(data)
            return path

        git("init", "-q")
        write("src/a.cpp", b"int a;\n")
        write("docs/caf\u00e9 notes.md", b"# ok\n")
        write("AlpacaCore/external/Vendor/sdk.h", b"\xef\xbb\xbfvendored\n")
        write("untracked.txt", b"never added\n")
        git("add", "src", "docs", "AlpacaCore")
        git("commit", "-q", "-m", "fixture")
        os.chdir(repo)
        try:
            listed = sorted(tracked_files())
        finally:
            os.chdir(real_cwd)
        check("tracked_files lists tracked paths, decoded as UTF-8, and nothing untracked",
              listed == ["docs/caf\u00e9 notes.md", "src/a.cpp"])
        check("tracked_files drops EXCLUDE_PREFIXES",
              not any(p.startswith("AlpacaCore/external/") for p in listed))

    # --- main() end to end over fixture files with tracked_files() patched --
    real_tracked_files = globals()["tracked_files"]
    with tempfile.TemporaryDirectory() as tmp:
        def fixture(name, data):
            path = os.path.join(tmp, name)
            with open(path, "wb") as fh:
                fh.write(data)
            return path

        def run_main_with(paths):
            globals()["tracked_files"] = lambda: iter(paths)
            try:
                return main()
            finally:
                globals()["tracked_files"] = real_tracked_files

        clean = fixture("clean.txt", "ascii and caf\u00e9\n".encode("utf-8"))
        check("main() passes a clean UTF-8 tree", run_main_with([clean]) == 0)
        check("main() FAILS when no file was scanned (empty file list)",
              run_main_with([]) == 1)
        rlo = fixture("rlo.cpp", "// safe\u202e evil\n".encode("utf-8"))
        check("main() FAILS on a UTF-8 file with a bidi override",
              run_main_with([clean, rlo]) == 1)
        binary = fixture("blob.bin", b"\x00\x01" + "\u202e".encode("utf-8"))
        check("main() skips a binary file even when it carries the bytes",
              run_main_with([binary]) == 0)
        # The "utf-16"/"utf-32" codecs write the BOM themselves; an explicit
        # U+FEFF in the text would be a second, embedded one and a finding.
        u16 = fixture("bom16.txt", "hello \u200b world\n".encode("utf-16"))
        check("main() FAILS on a BOM-declared UTF-16 file with a zero-width space",
              run_main_with([u16]) == 1)
        u16_clean = fixture("bom16-clean.txt", "hello world\n".encode("utf-16"))
        check("main() passes a clean BOM-declared UTF-16 file",
              run_main_with([u16_clean]) == 0)
        u32 = fixture("bom32.txt", "hi \u200e there\n".encode("utf-32"))
        check("main() FAILS on a BOM-declared UTF-32 file with a bidi mark",
              run_main_with([u32]) == 1)
        # Not valid UTF-8, no NUL, no BOM: flagged as suspect even when clean.
        latin1 = fixture("latin1.txt", b"caf\xe9\n")
        check("main() FAILS on a non-UTF-8 text file (anomaly, treated as suspect)",
              run_main_with([latin1]) == 1)
        # The byte backstop: a UTF-16-BE encoded override hidden in an
        # undecodable file must be named, not just the anomaly.
        hidden = fixture("hidden.txt", b"caf\xe9 " + "\u202e".encode("utf-16-be") + b"\n")
        import io
        import contextlib
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = run_main_with([hidden])
        check("main() names a UTF-16-BE-encoded override in an undecodable file",
              rc == 1 and "contains utf-16-be-encoded U+202E" in out.getvalue())
        dangling = os.path.join(tmp, "dangling")
        os.symlink(os.path.join(tmp, "does-not-exist"), dangling)
        check("main() FAILS on an unreadable tracked file (dangling symlink)",
              run_main_with([dangling]) == 1)
        dir_link = os.path.join(tmp, "dirlink")
        os.symlink(tmp, dir_link)
        check("main() skips a tracked symlink to a directory",
              run_main_with([clean, dir_link]) == 0)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            run_main_with([rlo])
        check("main() reports file:line:col U+XXXX",
              "rlo.cpp:1:8: U+202E RIGHT-TO-LEFT OVERRIDE" in out.getvalue())

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
