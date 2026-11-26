# Copyright (c) 2025-2026 Joey Troy and contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Check the vendored binaries under AlpacaCore/external/ against SHA256SUMS (issue #2633).

Every regular file git tracks under AlpacaCore/external/ and treats as binary
(vendor .so/.a files, fxload, camera firmware, source tarballs) must have a line
in AlpacaCore/external/SHA256SUMS, and every line must name a tracked binary
whose content still has that sha256. The manifest uses the `sha256sum` format
(`<hash>  <path>`, paths relative to AlpacaCore/external/), so
`cd AlpacaCore/external && sha256sum -c SHA256SUMS` also checks it.

Run from the repo root:   python3 scripts/check_vendor_binaries.py
After an SDK bump:        python3 scripts/check_vendor_binaries.py --write
"""

import argparse
import hashlib
from pathlib import Path
import subprocess
import sys

EXTERNAL = "AlpacaCore/external"
MANIFEST = "SHA256SUMS"


def tracked_binaries(root):
    """Paths (relative to EXTERNAL) of tracked regular files git sees as binary."""
    out = subprocess.run(
        ["git", "ls-files", "--eol", "-s", "-z", "--", EXTERNAL],
        cwd=root, check=True, capture_output=True, text=True,
    ).stdout
    paths = []
    for entry in filter(None, out.split("\0")):
        # "<mode> <object> <stage>\t<i/eol> <w/eol> <attr>\t<path>"
        stage, eol, path = entry.split("\t", 2)
        if stage.startswith("100") and eol.startswith("i/-text"):
            paths.append(path[len(EXTERNAL) + 1:])
    return sorted(paths)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_manifest(text):
    entries = {}
    for n, line in enumerate(text.splitlines(), 1):
        digest, sep, path = line.partition("  ")
        if not sep or len(digest) != 64 or not path:
            raise ValueError(f"{MANIFEST}:{n}: not '<sha256>  <path>': {line!r}")
        entries[path] = digest
    return entries


def problems(root):
    root = Path(root)
    base = root / EXTERNAL
    try:
        listed = read_manifest((base / MANIFEST).read_text(encoding="utf-8"))
    except OSError as e:
        return [f"{MANIFEST}: {e.strerror}"]
    except ValueError as e:
        return [str(e)]
    found = []
    binaries = tracked_binaries(root)
    for path in binaries:
        if path not in listed:
            found.append(f"{path}: tracked binary missing from {MANIFEST}")
        elif sha256(base / path) != listed[path]:
            found.append(f"{path}: sha256 differs from {MANIFEST}")
    for path in sorted(set(listed) - set(binaries)):
        found.append(f"{path}: listed in {MANIFEST} but not a tracked binary")
    return found


def write(root):
    base = Path(root) / EXTERNAL
    lines = [f"{sha256(base / p)}  {p}\n" for p in tracked_binaries(root)]
    (base / MANIFEST).write_text("".join(lines), encoding="utf-8", newline="\n")
    return len(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--write", action="store_true", help=f"regenerate {EXTERNAL}/{MANIFEST}")
    a = ap.parse_args(argv)
    if a.write:
        print(f"Wrote {write('.')} entries to {EXTERNAL}/{MANIFEST}")
        return 0
    found = problems(".")
    for p in found:
        print(f"::error::{EXTERNAL}/{p}")
    if found:
        print(f"Run `python3 scripts/check_vendor_binaries.py --write` only after checking the new files against the vendor's download.")
        return 1
    print(f"{EXTERNAL}/{MANIFEST}: every vendored binary matches")
    return 0


if __name__ == "__main__":
    sys.exit(main())
