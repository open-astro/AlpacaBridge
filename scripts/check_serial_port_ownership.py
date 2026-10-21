#!/usr/bin/env python3
"""Every serial port a driver opens goes through util::SerialLink (issue #1202).

Many vendors ship the same CH340 adapter, so one vendor's auto-detect probe
matches every other vendor's device. Before #1202 only some wrappers took part
in the in-use registry and fewer took an exclusive lock, so a probe opened a
connected device's live port at its own baud rate (a Gemini flat panel scan
took the Gemini power hub down until restart), and a reader that relied on
VTIME parked forever in n_tty_read once another program retuned the port to
VMIN=1 (a WandererRotator and an OnStep mount hung Disconnect).

util::SerialLink (AlpacaCore/include/alpacacore/util/serial_link.h) claims the
port in the registry, takes TIOCEXCL, keeps the fd non-blocking and records
the line settings so a retune is put back. A wrapper that opens a tty by hand
compiles and passes its tests while skipping all of that, so this scan fails
on the hand-rolled shapes:

  * O_NOCTTY            -- a raw tty open()
  * tcsetattr(          -- line settings applied outside SerialLink
                           (SerialLink::reconfigure() changes them)
  * TIOCEXCL / TIOCNXCL -- exclusive lock handled outside SerialLink
  * c_cc[VTIME] / [VMIN] -- a read timeout that does nothing on SerialLink's
                           non-blocking fd; reads are bounded by tty_read()
  * mark_serial_port_*  -- the pre-#1202 registry calls, which let a caller
                           mark without refusing and forget to release

Scanned: AlpacaCore/src and AlpacaCore/include, except the two files that
implement the helper. Comments are stripped first, string literals are not.
AlpacaHTTP is not scanned on purpose: it opens no tty, and the layering
rules (no vendor SDK or device I/O in AlpacaHTTP) keep it that way.

Regex regression guard (no repo state needed):
    python3 scripts/check_serial_port_ownership.py --self-test
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCAN_DIRS = [ROOT / "AlpacaCore" / "src", ROOT / "AlpacaCore" / "include"]
SOURCE_GLOBS = ("*.cpp", "*.h", "*.hpp")
HELPER_FILES = {
    "AlpacaCore/include/alpacacore/util/serial_link.h",
    "AlpacaCore/include/alpacacore/util/serial_port_registry.h",
}
# path -> reason. A file listed here may use the forbidden shapes; say why.
ALLOWLIST: dict[str, str] = {}

RULES = [
    (re.compile(r"\bO_NOCTTY\b"), "raw tty open(); open the port with util::SerialLink::open()"),
    (re.compile(r"\btcsetattr\s*\("), "tcsetattr() outside SerialLink; use SerialLink::open()/reconfigure()"),
    (re.compile(r"\bTIOC(?:EXCL|NXCL)\b"), "exclusive lock outside SerialLink; SerialLink::open() takes it"),
    (
        re.compile(r"\bc_cc\s*\[\s*V(?:TIME|MIN)\s*\]"),
        "VTIME/VMIN read timeout; SerialLink's fd is non-blocking, bound reads with util::tty_read()",
    ),
    (
        re.compile(r"\b(?:try_)?mark_serial_port_(?:open|closed)\b"),
        "pre-#1202 registry call; SerialLink holds an RAII util::SerialPortClaim",
    ),
]

COMMENT_RE = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)


def strip_comments(text: str) -> str:
    """Blank out comments, keeping line numbers. String literals are kept."""
    return COMMENT_RE.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def scan_text(text: str) -> list[tuple[int, str, str]]:
    """(line, matched text, reason) for every forbidden shape in @p text."""
    code = strip_comments(text)
    findings = []
    for pattern, reason in RULES:
        for m in pattern.finditer(code):
            line = code.count("\n", 0, m.start()) + 1
            findings.append((line, m.group(0), reason))
    return sorted(findings)


def main() -> int:
    failures = 0
    scanned = 0
    for base in SCAN_DIRS:
        for glob in SOURCE_GLOBS:
            for path in sorted(base.rglob(glob)):
                rel = path.relative_to(ROOT).as_posix()
                if any(part.startswith("build") for part in path.relative_to(ROOT).parts):
                    continue
                if rel in HELPER_FILES or rel in ALLOWLIST:
                    continue
                scanned += 1
                for line, text, reason in scan_text(path.read_text(encoding="utf-8", errors="replace")):
                    print(f"{rel}:{line}: {text}: {reason}")
                    failures += 1
    for rel in ALLOWLIST:
        if not (ROOT / rel).exists():
            print(f"{rel}: stale ALLOWLIST entry (file does not exist)")
            failures += 1
    if failures:
        print(f"\nSerial port ownership check FAILED: {failures} finding(s). See issue #1202, decision 0017 "
              "and .github/instructions/serial-autodetect.instructions.md 'Serial / socket I/O'.")
        return 1
    print(f"Serial port ownership check OK ({scanned} file(s) scanned).")
    return 0


def self_test() -> int:
    cases = [
        ("raw open", 'int fd = open(p.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);', 1),
        ("tcsetattr", "if (tcsetattr(fd, TCSANOW, &tty) != 0) return;", 1),
        ("tcsetattr spaced", "tcsetattr (fd, TCSANOW, &tty);", 1),
        ("excl", "(void)ioctl(fd, TIOCEXCL);", 1),
        ("nxcl", "ioctl(fd, TIOCNXCL);", 1),
        ("vtime", "tty.c_cc[VTIME] = 5;", 1),
        ("vmin spaced", "tty.c_cc[ VMIN ] = 0;", 1),
        ("old registry", "util::mark_serial_port_open(path);", 1),
        ("old try registry", "if (!try_mark_serial_port_open(k)) {}", 1),
        ("old close registry", "mark_serial_port_closed(k);", 1),
        ("line comment", "// a bare read() relied on c_cc[VTIME] and tcsetattr(fd...)", 0),
        ("block comment", "/* O_NOCTTY\n TIOCEXCL */ int x = 0;", 0),
        ("helper use", "link_ = util::SerialLink::open(p, util::SerialClaimKind::Link, kOwner, cfg, &err);", 0),
        ("reconfigure", "link_.reconfigure([](termios& t) { cfsetospeed(&t, B9600); });", 0),
        ("tcgetattr ok", "tcgetattr(fd, &tty);", 0),
        ("tty_read ok", "util::tty_read(fd, &ch, 1, 500);", 0),
        ("two on a line", "tty.c_cc[VMIN] = 0; tty.c_cc[VTIME] = 1;", 2),
    ]
    failures = 0
    for label, text, expected in cases:
        got = len(scan_text(text))
        if got != expected:
            print(f"  self-test: {label}: expected {expected} finding(s), got {got}")
            failures += 1
    if failures:
        print(f"\nself-test FAILED ({failures} case(s)).")
        return 1
    print(f"self-test OK ({len(cases)} cases).")
    return 0


if __name__ == "__main__":
    if "--self-test" in sys.argv[1:]:
        sys.exit(self_test())
    sys.exit(main())
