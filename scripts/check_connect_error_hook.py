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
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DRIVER_DIRS = [ROOT / "AlpacaCore" / "src"]

# `class Foo : public XDriver, protected alpacacore::AsyncConnectable {`
CLASS_RE = re.compile(
    r"^class\s+(?P<name>\w+)\b[^;{]*?\bprotected\s+(?:alpacacore::)?AsyncConnectable\b[^;{]*\{",
    re.MULTILINE,
)
MACRO = "ALPACA_EXPOSE_CONNECT_ERROR()"


def class_body(text: str, open_brace_index: int) -> str:
    """Return the text of the class body starting at its opening brace."""
    depth = 0
    for i in range(open_brace_index, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace_index : i + 1]
    return text[open_brace_index:]


def main() -> int:
    findings: list[str] = []
    checked = 0

    for directory in DRIVER_DIRS:
        for path in sorted(directory.rglob("*.cpp")):
            text = path.read_text(encoding="utf-8", errors="replace")
            if "AsyncConnectable" not in text:
                continue
            for match in CLASS_RE.finditer(text):
                checked += 1
                body = class_body(text, text.index("{", match.start()))
                if MACRO not in body:
                    line = text[: match.start()].count("\n") + 1
                    findings.append(
                        f"  {path.relative_to(ROOT)}:{line}: class {match.group('name')} inherits "
                        f"AsyncConnectable but does not use {MACRO}, so a failed connect reports "
                        f'only "Connection failed" and the driver\'s own reason is lost (issue #358). '
                        f"Add it in a public section."
                    )

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


if __name__ == "__main__":
    sys.exit(main())
