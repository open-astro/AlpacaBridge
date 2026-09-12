#!/usr/bin/env python3
"""Every AsyncConnectable driver must expose its connect-failure reason.

Issue #358 gave AsyncConnectable a stored reason for a failed connect, and the
router reports it to the client instead of a bare "Connection failed". The
router reads it through the AlpacaDriver::get_last_connect_error() virtual,
which each driver supplies with the ALPACA_EXPOSE_CONNECT_ERROR() macro.

It cannot be a dynamic_cast to AsyncConnectable: every driver mixes that base
in as `protected` (the documented style, which keeps start_connection_task()
out of the public API), and a cross-cast only traverses PUBLIC base paths. Such
a cast compiles, always returns nullptr, and silently drops every reason -- the
exact bug this gate exists to keep from coming back, found in review of the PR
that introduced the feature.

A driver that inherits AsyncConnectable and omits the macro compiles and tests
green while its operators are told nothing but "Connection failed", so nothing
except this gate would catch it.

Regex regression guard (no repo state needed):
    python3 scripts/check_connect_error_hook.py --self-test
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DRIVER_DIRS = [ROOT / "AlpacaCore" / "src", ROOT / "AlpacaCore" / "include"]
SOURCE_GLOBS = ("*.cpp", "*.h", "*.hpp")

# `class Foo : public XDriver, protected alpacacore::AsyncConnectable {`
# Matches the mixin regardless of access specifier on purpose: a driver that
# inherits it `public` would be invisible to a `protected`-only pattern and
# would silently report nothing. The access specifier is reported separately,
# since `public` is itself worth flagging -- it exposes start_connection_task()
# and friends on the driver's public API, which the documented style forbids.
#
# Leading whitespace is allowed so a class declared inside a namespace block
# is not skipped, and headers are scanned as well as .cpp files: every shape
# this gate does not match is one that "compiles and tests green while
# reporting nothing", the exact failure it exists to prevent.
CLASS_RE = re.compile(
    r"^[ \t]*class\s+(?P<name>\w+)\b[^;{]*?\b(?P<access>public|protected|private)?\s*(?:alpacacore::)?"
    r"AsyncConnectable\b[^;{]*\{",
    re.MULTILINE,
)
MACRO = "ALPACA_EXPOSE_CONNECT_ERROR()"


def class_body(text: str, open_brace_index: int) -> str:
    """Return the text of the class body starting at its opening brace.

    Counts braces without stripping strings, chars or comments. An unbalanced
    brace inside a literal would truncate or overrun the scanned region; no
    file in the tree does that today, and the failure mode of an overrun (a
    class passing because a LATER class in the same file carries the macro) is
    why this is worth knowing rather than silently relying on.
    """
    depth = 0
    for i in range(open_brace_index, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace_index : i + 1]
    return text[open_brace_index:]


def scan_text(text: str, label: str) -> tuple[list[str], int]:
    """Findings and match count for one file's text. Shared with --self-test."""
    findings: list[str] = []
    checked = 0
    for match in CLASS_RE.finditer(text):
        checked += 1
        body = class_body(text, text.index("{", match.start()))
        line = text[: match.start()].count("\n") + 1
        access = match.group("access") or "private"
        if MACRO not in body:
            findings.append(
                f"  {label}:{line}: class {match.group('name')} inherits "
                f"AsyncConnectable but does not use {MACRO}, so a failed connect reports "
                f'only "Connection failed" and the driver\'s own reason is lost (issue #358). '
                f"Add it in a public section."
            )
        elif access != "protected":
            findings.append(
                f"  {label}:{line}: class {match.group('name')} inherits "
                f"AsyncConnectable as `{access}`, not `protected`. The documented style is "
                f"protected, which keeps start_connection_task() and stop_connection_thread() "
                f"off the driver's public API."
            )
    return findings, checked


def main() -> int:
    findings: list[str] = []
    checked = 0

    paths = sorted(
        {p for directory in DRIVER_DIRS if directory.is_dir() for g in SOURCE_GLOBS for p in directory.rglob(g)}
    )
    for path in paths:
        text = path.read_text(encoding="utf-8", errors="replace")
        if "AsyncConnectable" not in text:
            continue
        file_findings, file_checked = scan_text(text, str(path.relative_to(ROOT)))
        findings.extend(file_findings)
        checked += file_checked

    if checked == 0:
        print("check_connect_error_hook: matched no drivers at all -- the pattern has drifted.")
        return 1

    if findings:
        print("Connect-error hook check failed:\n")
        print("\n".join(findings))
        print(f"\n{len(findings)} finding(s).")
        return 1

    print(f"Connect-error hook OK -- {checked} AsyncConnectable driver(s) expose their reason.")
    return 0


def self_test() -> int:
    """Regression guard for CLASS_RE, run with --self-test.

    The gate's own failure mode is silence: a regex that stops matching a
    shape reports nothing and passes. `checked == 0` only fires when ALL the
    drivers stop matching, so one missed shape is otherwise invisible. These
    synthetic snippets pin each shape the tree actually uses, plus the two
    that a naive pattern would drop (a class indented inside a namespace, and
    one declared in a header).
    """
    cases: list[tuple[str, str, int, bool]] = [
        # (label, snippet, expected match count, expected to be clean)
        (
            "protected mixin with the macro",
            "class FooDriver : public TelescopeDriver, protected alpacacore::AsyncConnectable {\n"
            "public:\n    ALPACA_EXPOSE_CONNECT_ERROR()\n};\n",
            1,
            True,
        ),
        (
            "protected mixin without the macro",
            "class FooDriver : public TelescopeDriver, protected alpacacore::AsyncConnectable {\n"
            "public:\n    void f() {}\n};\n",
            1,
            False,
        ),
        (
            "public mixin is flagged even with the macro",
            "class FooDriver : public TelescopeDriver, public alpacacore::AsyncConnectable {\n"
            "public:\n    ALPACA_EXPOSE_CONNECT_ERROR()\n};\n",
            1,
            False,
        ),
        (
            "unqualified base name",
            "class FooDriver : public TelescopeDriver, protected AsyncConnectable {\n"
            "public:\n    ALPACA_EXPOSE_CONNECT_ERROR()\n};\n",
            1,
            True,
        ),
        (
            "indented inside a namespace block",
            "namespace v {\n"
            "    class FooDriver : public TelescopeDriver, protected alpacacore::AsyncConnectable {\n"
            "    public:\n    };\n"
            "}\n",
            1,
            False,
        ),
        (
            "a class that does not inherit the mixin is not matched",
            "class Unrelated : public TelescopeDriver {\npublic:\n};\n",
            0,
            True,
        ),
    ]

    failures = 0
    for label, snippet, expected_matches, expect_clean in cases:
        findings, checked = scan_text(snippet, "synthetic.cpp")
        if checked != expected_matches:
            print(f"  self-test: {label}: matched {checked}, expected {expected_matches}")
            failures += 1
        elif expect_clean and findings:
            print(f"  self-test: {label}: expected no finding, got {findings[0].strip()}")
            failures += 1
        elif not expect_clean and not findings:
            print(f"  self-test: {label}: expected a finding, got none")
            failures += 1

    if failures:
        print(f"\nCLASS_RE self-test FAILED ({failures} case(s)).")
        return 1
    print(f"CLASS_RE self-test OK -- {len(cases)} case(s).")
    return 0


if __name__ == "__main__":
    if "--self-test" in sys.argv[1:]:
        sys.exit(self_test())
    sys.exit(main())
