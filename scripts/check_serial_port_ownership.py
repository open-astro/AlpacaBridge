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
  * read( / write(      -- a raw read or write on a link's fd (`x.fd()`,
                           `x->fd()` or `serial_fd_`); reads and writes go
                           through util::tty_read()/tty_write_all() (decision
                           0017 rule 9, issue #1440). Sky-Watcher and the
                           Celestron CGX keep poll()-bounded loops behind
                           their test seams and are allowed this rule only.
                           Sockets and non-tty fds are not matched.

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
RAW_IO = "raw-io"
# path -> {rule id: reason}. A file listed here may use that rule's forbidden
# shape and no other; say why. An entry whose file is gone, or whose rule no
# longer matches in it, is stale and fails.
ALLOWLIST: dict[str, dict[str, str]] = {
    "AlpacaCore/src/vendors/skywatcher/skywatcher_protocol_wrapper.cpp": {
        RAW_IO: "poll()-bounded loops on the non-blocking fd, behind serial_read_/serial_write_ test seams (decision 0017 rule 9)",
    },
    "AlpacaCore/src/vendors/celestron/celestron_cgx_protocol_wrapper.cpp": {
        RAW_IO: "poll()-bounded loops on the non-blocking fd, behind test seams (decision 0017 rule 9)",
    },
}

# (rule id, pattern, reason)
RULES = [
    ("o-noctty", re.compile(r"\bO_NOCTTY\b"), "raw tty open(); open the port with util::SerialLink::open()"),
    (
        "tcsetattr",
        re.compile(r"\btcsetattr\s*\("),
        "tcsetattr() outside SerialLink; use SerialLink::open()/reconfigure()",
    ),
    ("tiocexcl", re.compile(r"\bTIOC(?:EXCL|NXCL)\b"), "exclusive lock outside SerialLink; SerialLink::open() takes it"),
    (
        "vtime-vmin",
        re.compile(r"\bc_cc\s*\[\s*V(?:TIME|MIN)\s*\]"),
        "VTIME/VMIN read timeout; SerialLink's fd is non-blocking, bound reads with util::tty_read()",
    ),
    (
        "mark-registry",
        re.compile(r"\b(?:try_)?mark_serial_port_(?:open|closed)\b"),
        "pre-#1202 registry call; SerialLink holds an RAII util::SerialPortClaim",
    ),
    (
        RAW_IO,
        re.compile(
            r"(?<![\w.>:])(?:::)?(?:read|write)\s*\(\s*"
            r"(?:(?:[\w:]+(?:\(\))?(?:\.|->))*fd\(\)|serial_fd_)\s*,"
        ),
        "raw read()/write() on a link's fd; use util::tty_read()/util::tty_write_all()",
    ),
]

COMMENT_RE = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)


def strip_comments(text: str) -> str:
    """Blank out comments, keeping line numbers. String literals are kept."""
    return COMMENT_RE.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def scan_text(text: str, allowed: frozenset[str] = frozenset()) -> list[tuple[int, str, str]]:
    """(line, matched text, reason) for every forbidden shape in @p text.

    @p allowed names rule ids the file may break; see ALLOWLIST.
    """
    return [(line, matched, reason) for rule_id, line, matched, reason in scan_rules(text) if rule_id not in allowed]


def scan_rules(text: str) -> list[tuple[str, int, str, str]]:
    """(rule id, line, matched text, reason) for every forbidden shape."""
    code = strip_comments(text)
    findings = []
    for rule_id, pattern, reason in RULES:
        for m in pattern.finditer(code):
            line = code.count("\n", 0, m.start()) + 1
            findings.append((rule_id, line, m.group(0), reason))
    return sorted(findings, key=lambda f: (f[1], f[2]))


def check_files(files: dict[str, str], allowlist: dict[str, dict[str, str]]) -> list[str]:
    """Finding lines for the scanned @p files (path -> text), honouring @p allowlist.

    An allowlist entry whose file is not in @p files, or whose rule never matches in it, is stale.
    """
    out = []
    used: set[tuple[str, str]] = set()
    for rel, text in files.items():
        allowed = allowlist.get(rel, {})
        for rule_id, line, matched, reason in scan_rules(text):
            if rule_id in allowed:
                used.add((rel, rule_id))
                continue
            out.append(f"{rel}:{line}: {matched}: {reason}")
    for rel, rules in allowlist.items():
        if rel not in files:
            out.append(f"{rel}: stale ALLOWLIST entry (file does not exist)")
            continue
        for rule_id in rules:
            if (rel, rule_id) not in used:
                out.append(f"{rel}: stale ALLOWLIST entry (rule {rule_id} no longer matches in it)")
    return out


def main() -> int:
    files: dict[str, str] = {}
    for base in SCAN_DIRS:
        for glob in SOURCE_GLOBS:
            for path in sorted(base.rglob(glob)):
                rel = path.relative_to(ROOT).as_posix()
                if any(part.startswith("build") for part in path.relative_to(ROOT).parts):
                    continue
                if rel in HELPER_FILES:
                    continue
                files[rel] = path.read_text(encoding="utf-8", errors="replace")
    scanned = len(files)
    findings = check_files(files, ALLOWLIST)
    failures = len(findings)
    for f in findings:
        print(f)
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
        ("raw read link", "n = ::read(link_.fd(), buf, 1);", 1),
        ("raw read arrow", "n = read(link_->fd(), buf, 1);", 1),
        ("raw write member", "n = ::write(serial_fd_, p, len);", 1),
        ("raw read this arrow", "n = ::read(this->link_.fd(), buf, 1);", 1),
        ("tty_read link ok", "util::tty_read(link_.fd(), &ch, 1, 500);", 0),
        ("tty_write_all ok", "util::tty_write_all(link_.fd(), d, n);", 0),
        ("write_all ok", "util::write_all(fd, d, n);", 0),
        ("socket read ok", "ssize_t r = read(fd, &ch, 1);", 0),
        ("member read ok", "n = stream.read(link_.fd(), 1);", 0),
        ("std read ok", "n = std::read(link_.fd(), 1);", 0),
        ("serial_read_ seam ok", "r = serial_read_(serial_fd_, &ch, 1);", 0),
        ("two on a line", "tty.c_cc[VMIN] = 0; tty.c_cc[VTIME] = 1;", 2),
    ]
    failures = 0
    for label, text, expected in cases:
        got = len(scan_text(text))
        if got != expected:
            print(f"  self-test: {label}: expected {expected} finding(s), got {got}")
            failures += 1
    seam = "r = serial_read_ ? serial_read_(serial_fd_, &ch, 1) : ::read(serial_fd_, &ch, 1);"
    for label, text, allowed, expected in [
        ("seam shape allowed for raw-io", seam, frozenset({RAW_IO}), 0),
        ("seam shape flagged elsewhere", seam, frozenset(), 1),
        ("raw-io allowance keeps other rules", seam + " tcsetattr(fd, 0, &t);", frozenset({RAW_IO}), 1),
    ]:
        got = len(scan_text(text, allowed))
        if got != expected:
            print(f"  self-test: {label}: expected {expected} finding(s), got {got}")
            failures += 1
    allow = {"a.cpp": {RAW_IO: "why"}}
    raw = "n = ::read(link_.fd(), buf, 1);"
    for label, files, expected in [
        ("allowance used gives no finding", {"a.cpp": raw}, 0),
        ("allowance never matching is stale", {"a.cpp": "int x = 0;"}, 1),
        ("allowance for a missing file is stale", {}, 1),
        ("no allowance for another file", {"a.cpp": raw, "b.cpp": raw}, 1),
    ]:
        got = len(check_files(files, allow))
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
