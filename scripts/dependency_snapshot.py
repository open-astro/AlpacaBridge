#!/usr/bin/env python3
"""Build a GitHub Dependency Submission API snapshot for the C++ build.

GitHub's dependency graph only parses manifests it knows (here: the workflow
action pins). This script reports what actually ships: the Debian packages in
debian/control, the CMake FetchContent libraries and the vendored SDKs in
AlpacaCore/external/ (versions from scripts/dependency_sdk_versions.json).

  dependency_snapshot.py [--sha S] [--ref R] [--output FILE]   write the snapshot
  dependency_snapshot.py --check                               drift check
  dependency_snapshot.py --self-test

Rules: Build-Depends become scope "development", Depends become "runtime".
For an alternative group ``a | b`` the FIRST alternative is reported. A version
is put in a deb purl only for an exact ``(= X)`` pin; ``${...}`` substitutions
and ``debhelper-compat`` are skipped. ``${shlibs:Depends}`` is resolved only at
package build time and is out of scope. deb and generic purls raise no
Dependabot alerts; they document the inventory.
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import tempfile
import urllib.parse
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TABLE = "scripts/dependency_sdk_versions.json"
EXTERNAL = "AlpacaCore/external"
DEB_QUALIFIERS = "arch=arm64&distro=trixie"
SKIP = {"debhelper-compat"}
DETECTOR = {"name": "alpacabridge-dependency-snapshot", "version": "1",
            "url": "https://github.com/open-astro/AlpacaBridge/blob/main/scripts/dependency_snapshot.py"}


def parse_relations(value: str) -> list[tuple[str, str | None]]:
    """Return (package, exact version or None) per comma group, first alternative."""
    out = []
    for group in value.replace("\n", " ").split(","):
        group = group.strip()
        if not group:
            continue
        first = group.split("|")[0].strip()
        # "${...}" substitutions fail the name pattern below and are skipped.
        m = re.match(r"^([a-z0-9][a-z0-9+.\-]*)\s*(?:\[[^\]]*\])?\s*(?:\(\s*([<>=]+)\s*([^)\s]+)\s*\))?", first)
        if not m or m.group(1) in SKIP:
            continue
        out.append((m.group(1), m.group(3) if m.group(2) == "=" else None))
    return out


def parse_control(text: str) -> dict[str, list[tuple[str, str | None]]]:
    """Return {"development": Build-Depends, "runtime": Depends of the binary stanza}."""
    fields: dict[str, dict[str, str]] = {}
    for stanza in re.split(r"\n\s*\n", text.strip()):
        key = "source" if stanza.startswith("Source:") else "binary"
        cur = None
        for line in stanza.splitlines():
            if line[:1] in " \t" and cur:
                fields.setdefault(key, {})[cur] += " " + line.strip()
            elif ":" in line:
                cur, _, val = line.partition(":")
                fields.setdefault(key, {})[cur] = val.strip()
    return {"development": parse_relations(fields.get("source", {}).get("Build-Depends", "")),
            "runtime": parse_relations(fields.get("binary", {}).get("Depends", ""))}


def parse_fetchcontent(text: str) -> list[tuple[str, str]]:
    """Return (owner/repo, tag) per FetchContent_Declare of a github.com repository."""
    out = []
    for block in re.findall(r"FetchContent_Declare\((.*?)\)", text, re.S):
        repo = re.search(r"GIT_REPOSITORY\s+https://github\.com/([\w.\-]+/[\w.\-]+?)(?:\.git)?\s", block + " ")
        tag = re.search(r"GIT_TAG\s+(\S+)", block)
        if repo and tag:
            out.append((repo.group(1), tag.group(1)))
    return out


def deb_purl(name: str, version: str | None) -> str:
    name = urllib.parse.quote(name, safe="")  # purl: "+" in g++ is %2B
    return f"pkg:deb/debian/{name}{'@' + version if version else ''}?{DEB_QUALIFIERS}"


def manifest(path: str, entries: list[tuple[str, str]]) -> dict:
    """entries: (purl, scope). Skips duplicates."""
    resolved = {}
    for purl, scope in entries:
        resolved.setdefault(purl, {"package_url": purl, "relationship": "direct", "scope": scope})
    return {"name": path, "file": {"source_location": path}, "resolved": resolved}


def build_snapshot(root: Path, sha: str, ref: str, job_id: str, scanned: str) -> dict:
    manifests = {}
    control = parse_control((root / "debian/control").read_text(encoding="utf-8"))
    entries = [(deb_purl(n, v), scope) for scope, rels in control.items() for n, v in rels]
    manifests["debian/control"] = manifest("debian/control", entries)
    cm = "AlpacaHTTP/CMakeLists.txt"
    fetched = parse_fetchcontent((root / cm).read_text(encoding="utf-8"))
    manifests[cm] = manifest(cm, [(f"pkg:github/{repo}@{tag}", "runtime") for repo, tag in fetched])
    table = json.loads((root / TABLE).read_text(encoding="utf-8"))
    sdk = []
    for e in table["sdks"]:
        ver = e.get("version")
        sdk.append((f"pkg:generic/{e['name']}{'@' + ver if ver else ''}", "runtime"))
    manifests[TABLE] = manifest(TABLE, sdk)
    return {"version": 0, "job": {"correlator": "dependency-submission", "id": job_id},
            "sha": sha, "ref": ref, "detector": DETECTOR, "scanned": scanned,
            "manifests": manifests}


def check_drift(root: Path) -> list[str]:
    table = json.loads((root / TABLE).read_text(encoding="utf-8"))
    sdks = [e["path"] for e in table["sdks"]]
    docs = list(table["doc_only"])
    listed = sdks + docs
    errors = []
    for p in listed:
        if not (root / p).is_dir():
            errors.append(f"{TABLE}: stale entry {p} (directory gone): remove it from 'sdks' or 'doc_only'")

    def walk(rel: str) -> None:
        for child in sorted((root / rel).iterdir()):
            if not child.is_dir():
                continue
            c = f"{rel}/{child.name}"
            if c in listed:
                continue
            if any(p.startswith(c + "/") for p in listed):
                walk(c)
                continue
            errors.append(f"{c} is not in {TABLE}: add it to 'sdks' (name, version, purl source) "
                          f"if it holds an SDK or library, or to 'doc_only' if it holds only documents or udev rules")

    if (root / EXTERNAL).is_dir():
        walk(EXTERNAL)
    return errors


def git_out(*args: str) -> str:
    return subprocess.run(["git", "-C", str(ROOT), *args], check=True, capture_output=True, text=True).stdout.strip()


def self_test() -> int:
    ctl = ("Source: x\nBuild-Depends: debhelper-compat (= 13),\n cmake (>= 3.20),\n g++,\n"
           " libgpiod-dev (>= 2.0),\n libfoo (= 1.2-3),\n libcurl4-openssl-dev | libcurl4-gnutls-dev\n"
           "Standards-Version: 4\n\nPackage: x\nArchitecture: arm64\n"
           "Depends: ${shlibs:Depends}, ${misc:Depends},\n libusb-1.0-0,\n adduser\nRecommends: polkitd\n")
    c = parse_control(ctl)
    assert c["development"] == [("cmake", None), ("g++", None), ("libgpiod-dev", None),
                                ("libfoo", "1.2-3"), ("libcurl4-openssl-dev", None)], c
    assert c["runtime"] == [("libusb-1.0-0", None), ("adduser", None)], c
    assert deb_purl("libfoo", "1.2-3") == "pkg:deb/debian/libfoo@1.2-3?arch=arm64&distro=trixie"
    cm = "FetchContent_Declare(\n nlohmann_json\n GIT_REPOSITORY https://github.com/nlohmann/json.git\n GIT_TAG v3.11.3\n)\n"
    assert deb_purl("g++", None).startswith("pkg:deb/debian/g%2B%2B?")
    assert parse_fetchcontent(cm) == [("nlohmann/json", "v3.11.3")]
    with tempfile.TemporaryDirectory() as d:
        r = Path(d)
        (r / "debian").mkdir()
        (r / "debian/control").write_text(ctl)
        (r / "AlpacaHTTP").mkdir()
        (r / "AlpacaHTTP/CMakeLists.txt").write_text(cm)
        (r / "scripts").mkdir()
        for p in ("A/sdk-1", "B", "Q/deep"):
            (r / EXTERNAL / p).mkdir(parents=True)
        (r / TABLE).write_text(json.dumps({"sdks": [
            {"path": f"{EXTERNAL}/A/sdk-1", "name": "a-sdk", "version": "1"},
            {"path": f"{EXTERNAL}/Q/deep", "name": "q-sdk", "version": None}],
            "doc_only": [f"{EXTERNAL}/B"]}))
        assert check_drift(r) == [], check_drift(r)
        snap = build_snapshot(r, "a" * 40, "refs/heads/main", "1", "2026-01-01T00:00:00Z")
        for k in ("version", "job", "sha", "ref", "detector", "scanned", "manifests"):
            assert k in snap, k
        assert snap["version"] == 0 and {"correlator", "id"} <= set(snap["job"])
        assert {"name", "version", "url"} <= set(snap["detector"])
        for m in snap["manifests"].values():
            assert m["name"] and m["file"]["source_location"]
            for dep in m["resolved"].values():
                assert dep["relationship"] == "direct" and dep["scope"] in ("runtime", "development")
        assert "pkg:generic/q-sdk" in snap["manifests"][TABLE]["resolved"]
        assert "pkg:generic/a-sdk@1" in snap["manifests"][TABLE]["resolved"]
        assert "pkg:github/nlohmann/json@v3.11.3" in snap["manifests"]["AlpacaHTTP/CMakeLists.txt"]["resolved"]
        assert snap["manifests"]["debian/control"]["resolved"][deb_purl("cmake", None)]["scope"] == "development"
        assert snap["manifests"]["debian/control"]["resolved"][deb_purl("adduser", None)]["scope"] == "runtime"
        (r / EXTERNAL / "New").mkdir()
        errs = check_drift(r)
        assert len(errs) == 1 and f"{EXTERNAL}/New is not in {TABLE}" in errs[0], errs
        (r / EXTERNAL / "New").rmdir()
        (r / EXTERNAL / "B").rmdir()
        errs = check_drift(r)
        assert len(errs) == 1 and "stale entry" in errs[0], errs
    print("dependency_snapshot self-test: OK")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--sha")
    ap.add_argument("--ref")
    ap.add_argument("--output")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if a.check:
        errs = check_drift(ROOT)
        for e in errs:
            print(f"ERROR: {e}")
        print("dependency table: " + ("DRIFT" if errs else "OK"))
        return 1 if errs else 0
    sha = a.sha or os.environ.get("GITHUB_SHA") or git_out("rev-parse", "HEAD")
    ref = a.ref or os.environ.get("GITHUB_REF") or "refs/heads/main"
    job_id = os.environ.get("GITHUB_RUN_ID", "local")
    scanned = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    text = json.dumps(build_snapshot(ROOT, sha, ref, job_id, scanned), indent=2) + "\n"
    if a.output:
        Path(a.output).write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
