#!/usr/bin/env python3
"""Build a GitHub Dependency Submission API snapshot for the C++ build.

GitHub's dependency graph parses only the manifests it knows (here: the workflow
action pins), and stops listing a workflow's pins once a submitted snapshot names that
workflow file. This script reports what actually ships: the Debian packages in
debian/control, the CMake FetchContent libraries and the vendored SDKs in
AlpacaCore/external/ (versions from scripts/dependency_sdk_versions.json), and
the system packages the daemon needs that Depends does not name (RUNTIME_SYSTEM),
and, with --deb, a built package's resolved Depends and Recommends,
plus, as development dependencies, the tools .github/workflows/ci.yml installs outside debian/control (cppcheck,
zizmor, and the CI_APT_TOOLS such as Catch2), the ConformU build .claude/commands/conformu.md pins,
every workflow job's container image (pkg:docker), and the action pins of each
workflow file the snapshot names (GitHub stops listing them itself once it does).

  dependency_snapshot.py [--sha S] [--ref R] [--deb DEB] [--debian-policy F] [--ubuntu-policy F] [--output FILE]
                                                               write the snapshot
  dependency_snapshot.py --apt-packages debian|ubuntu [--deb DEB]  package names to pass to apt-cache policy
  dependency_snapshot.py --newest-deb DIR                      path of the newest nightly .deb in DIR
  dependency_snapshot.py --check                               drift check
  dependency_snapshot.py --spdx DEB [--output FILE]            SPDX SBOM for a built .deb
  dependency_snapshot.py --pin                                 rewrite the SDK sha256 pins
  dependency_snapshot.py --self-test

Rules: Build-Depends become scope "development", Depends and Recommends become "runtime".
For an alternative group ``a | b`` the FIRST alternative is reported. A version
is put in a deb purl for an exact ``(= X)`` pin, else the apt candidate from
``--debian-policy``/``--ubuntu-policy`` (``apt-cache policy`` output) when one is
given; with neither the purl carries no version. ``${...}`` substitutions
and ``debhelper-compat`` are skipped. ``${shlibs:Depends}`` is resolved only at
package build time, so debian/control alone cannot report it; ``--deb`` reads it
from a built package (dependency-submission.yml passes the latest nightly .deb).
deb and generic purls raise no
Dependabot alerts; they document the inventory.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
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
VENDORS = "AlpacaCore/src/vendors"
DEB_QUALIFIERS = "arch=arm64&distro=trixie"
# Package directories holding vendor binaries and firmware copied from AlpacaCore/external/.
SHIPPED_VENDOR_DIRS = ("usr/lib/alpacabridge", "usr/lib/firmware")
# Subtrees of those directories built at package time from a vendored source tarball (libgphoto2,
# scripts/build_bundled_gphoto.sh): no file in them is byte-identical to a tracked file, so the
# file-level match below cannot apply. The SDK entry (the tarball) is still CONTAINS-ed by the package.
BUILT_FROM_VENDORED_SOURCE = ("usr/lib/alpacabridge/libgphoto2/",)
SKIP = {"debhelper-compat"}
CI = ".github/workflows/ci.yml"
# (ci.yml step name, purl template): tools CI downloads at a pinned version.
CI_TOOL_STEPS = (("Build cppcheck 2.17.x from source", "pkg:github/danmar/cppcheck@{ver}"),
                 ("Install zizmor", "pkg:github/zizmorcore/zizmor@v{ver}"))
# The same ver= pins in other workflows: {workflow path: ((step name, purl template), ...)}.
WORKFLOW_TOOL_STEPS = {".github/workflows/osv-scan.yml": (("Install OSV-Scanner",
                                                          "pkg:github/google/osv-scanner@v{ver}"),)}
# Unpinned tools ci.yml installs from the runner's apt (Ubuntu 24.04, not trixie).
CI_APT_TOOLS = ("catch2", "clang-format", "clang-tidy", "gcovr", "nodejs", "shellcheck")
CI_APT_QUALIFIERS = "arch=arm64&distro=ubuntu-24.04"
# ConformU validates every driver on the rig; no workflow installs it. /conformu installs a
# maintainer beta (no GitHub release yet) from the project's tools/ mirror and names the build
# it must match.
CONFORMU_DOC = ".claude/commands/conformu.md"
CONFORMU_BUILD_RE = re.compile(r"`(\d+\.\d+\.\d+)\.\d+, Build time:")
CONFORMU_BETA_URL = "download.ascom-standards.org/beta/"
DETECTOR = {"name": "alpacabridge-dependency-snapshot", "version": "1",
            "url": "https://github.com/open-astro/AlpacaBridge/blob/main/scripts/dependency_snapshot.py"}
# Packages the installed daemon needs that debian/control's Depends does not name, each with the
# evidence for it: (package, file glob, regex). A package is reported while its regex matches a
# file the glob finds; --check fails once it matches none, so a dropped need is never kept silently.
# INSTALLED_UDEV_RULES in place of a glob means the udev rules files debian/rules installs.
INSTALLED_UDEV_RULES = "<the udev rules debian/rules installs>"
RUNTIME_MANIFEST = "runtime system packages (not in Depends)"
RUNTIME_SYSTEM = (
    ("systemd", "debian/alpacabridge.service", r"^ExecStart=/usr/bin/alpacabridge$"),
    ("dbus", "AlpacaHTTP/src/util/software_update.cpp", r"\bsd_bus_open_system\("),
    ("udev", "debian/rules", r"/usr/lib/udev/rules\.d"),
    ("apt", "debian/alpacabridge-software-update", r"^apt-get "),
    ("dpkg", "debian/alpacabridge-software-update", r"\bdpkg-query\b"),
    ("bash", "debian/alpacabridge-software-update", r"^#!/bin/bash"),
    # The vendor udev rules the package installs run /bin/sh, /bin/echo and /bin/sleep.
    ("dash", INSTALLED_UDEV_RULES, r'RUN\+="/bin/sh '),
    ("coreutils", INSTALLED_UDEV_RULES, r'RUN\+="[^"]*/bin/(?:echo|sleep)\b'),
)
# --deb: a built package's resolved Depends and Recommends, as one more manifest of the main snapshot.
# Its source_location is debian/rules (dh_shlibdeps resolves ${shlibs:Depends} there), which no other
# manifest names: two manifests on one location may replace each other in the dependency graph.
DEB_MANIFEST = "alpacabridge .deb (resolved Depends and Recommends)"
DEB_SOURCE = "debian/rules"


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
    """Return {"development": Build-Depends, "runtime": Depends, "recommends": Recommends of the binary stanza}."""
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
            "runtime": parse_relations(fields.get("binary", {}).get("Depends", "")),
            "recommends": parse_relations(fields.get("binary", {}).get("Recommends", ""))}


def parse_fetchcontent(text: str) -> list[tuple[str, str]]:
    """Return (owner/repo, tag) per FetchContent_Declare of a github.com repository."""
    out = []
    for block in re.findall(r"FetchContent_Declare\((.*?)\)", text, re.S):
        repo = re.search(r"GIT_REPOSITORY\s+https://github\.com/([\w.\-]+/[\w.\-]+?)(?:\.git)?\s", block + " ")
        tag = re.search(r"GIT_TAG\s+(\S+)", block)
        if repo and tag:
            out.append((repo.group(1), tag.group(1)))
    return out


def _step_block(text: str, step: str) -> str | None:
    """The body of the workflow step named ``step``, up to the next ``- name:``."""
    m = re.search(rf"^\s*- name: [\"']?{re.escape(step)}[\"']?\s*$(.*?)(?=^\s*- name:|\Z)", text, re.M | re.S)
    return m.group(1) if m else None


def parse_step_pins(text: str, path: str, steps) -> tuple[list[str], list[str]]:
    """Return (purls, errors) for the ``ver=`` pin inside each named workflow step."""
    purls, errors = [], []
    for step, fmt in steps:
        block = _step_block(text, step)
        ver = re.search(r"^\s*ver=([0-9][0-9.]*)\s*$", block, re.M) if block is not None else None
        if ver:
            purls.append(fmt.format(ver=ver.group(1)))
        else:
            errors.append(f"{path}: no 'ver=' pin found in the step '{step}': update CI_TOOL_STEPS "
                          "or WORKFLOW_TOOL_STEPS in scripts/dependency_snapshot.py to match the workflow")
    return purls, errors


def parse_ci_tools(text: str) -> tuple[list[str], list[str]]:
    """Return (purls, errors) for the tools ci.yml installs outside debian/control.

    cppcheck and zizmor are pinned by a ``ver=`` line in their install step; the
    CI_APT_TOOLS come unpinned from the runner's apt (Ubuntu 24.04), so their purls
    carry no version.
    """
    purls, errors = parse_step_pins(text, CI, CI_TOOL_STEPS)
    installed = parse_apt_installs(text)
    for tool in CI_APT_TOOLS:
        if tool in installed:
            purls.append(f"pkg:deb/ubuntu/{tool}?{CI_APT_QUALIFIERS}")
        else:
            errors.append(f"{CI}: no apt-get install of {tool} found: update CI_APT_TOOLS "
                          "in scripts/dependency_snapshot.py to match the workflow")
    return purls, errors


def parse_apt_installs(text: str) -> set[str]:
    """Package names on every ``apt-get install`` line and its ``\\`` continuation lines."""
    names: set[str] = set()
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if "apt-get install" not in line or line.lstrip().startswith("#"):
            continue
        words = line.split("apt-get install", 1)[1].split()
        while line.rstrip().endswith("\\") and i + 1 < len(lines):
            i += 1
            line = lines[i]
            words += line.split()
        for w in words:
            # The command ends at a shell separator: "install foo && echo bar" installs only foo,
            # and so does "install foo; echo bar".
            end = w in ("&&", "||", "|", ";") or w.endswith(";")
            w = w.rstrip(";")
            if w and w not in ("&&", "||", "|", "\\") and not w.startswith("-") \
                    and not any(c in w for c in "<>$="):
                names.add(w)
            if end:
                break
    return names


def parse_conformu(text: str) -> tuple[list[str], list[str]]:
    """Return (purls, errors) for the ConformU build /conformu pins.

    The version carries ``-beta`` only while the doc installs from the ASCOM beta download
    (``CONFORMU_BETA_URL``); a pin on a GitHub release reports the bare version.
    """
    builds = {m.group(0)[1:-len(", Build time:")]: m for m in CONFORMU_BUILD_RE.finditer(text)}
    if not builds:
        return [], [f"{CONFORMU_DOC}: no pinned ConformU build ('`X.Y.Z.N, Build time:`') found: "
                    "if ConformU is now a GitHub release, update parse_conformu in scripts/dependency_snapshot.py"]
    if len(builds) > 1:
        # Taking the first would report whichever build the prose names first, silently.
        return [], [f"{CONFORMU_DOC}: more than one pinned ConformU build ({', '.join(sorted(builds))}); "
                    "keep the '`X.Y.Z.N, Build time:`' form for the standard build only"]
    m = next(iter(builds.values()))
    suffix = "-beta" if CONFORMU_BETA_URL in text else ""
    return [f"pkg:github/ASCOMInitiative/ConformU@{m.group(1)}{suffix}"], []


def parse_containers(text: str) -> list[str]:
    """Return a docker purl per job ``container:`` image (string or mapping form) in a workflow.

    In the mapping form ``image:`` may follow other keys (``credentials:``, ``options:``). An
    image on another registry keeps that registry in the ``repository_url`` qualifier, as the
    purl spec requires; ``${{ ... }}`` expressions are skipped.
    """
    images = []
    lines = text.splitlines()
    for i, line in enumerate(lines):
        m = re.match(r"^(\s*)container:\s*([^#]*?)\s*(?:#.*)?$", line)
        if not m:
            continue
        if m.group(2):
            images.append(m.group(2))
            continue
        indent = len(m.group(1))
        for sub in lines[i + 1:]:
            if sub.strip() and len(sub) - len(sub.lstrip()) <= indent:
                break
            im = re.match(r"^\s*image:\s*([^\s#]+)", sub)
            if im:
                images.append(im.group(1))
                break
    out = []
    for image in images:
        if image.startswith("${{"):
            continue
        if "@" in image:
            name, ver = image.split("@", 1)
        elif ":" in image.rsplit("/", 1)[-1]:
            name, ver = image.rsplit(":", 1)
        else:
            name, ver = image, "latest"
        qualifier = ""
        first, _, rest = name.partition("/")
        if rest and ("." in first or ":" in first or first == "localhost"):
            name, qualifier = rest, "?repository_url=" + urllib.parse.quote(first, safe="")
        purl = f"pkg:docker/{name}@{urllib.parse.quote(ver, safe='')}{qualifier}"
        if purl not in out:
            out.append(purl)
    return out


def parse_uses(text: str) -> list[str]:
    """A githubactions purl per remote ``uses:`` pin in a workflow; ``./`` paths and ``docker://`` are skipped."""
    out = []
    for m in re.finditer(r"^\s*(?:-\s+)?uses:\s*[\"']?([^\s\"'#]+)", text, re.M):
        ref = m.group(1)
        if ref.startswith(("./", "docker://")) or "@" not in ref:
            continue
        purl = f"pkg:githubactions/{ref}"
        if purl not in out:
            out.append(purl)
    return out


def scan_udev_rules(root: Path) -> tuple[list[Path], list[str]]:
    """(vendored udev rules files debian/rules installs, errors), its make variables expanded.

    Only ``:=`` and ``=`` assignments are expanded. A ``.rules`` path that uses a variable set any other
    way (``?=``, ``+=``, ``$(shell ...)``), or that still holds a ``$(VAR)`` after expansion, is an error:
    skipping it would drop that file's evidence without a message. The one exception is an unresolved
    ``$(VAR)`` after an absolute prefix that cannot lead into AlpacaCore/external/ (the libgphoto2
    stage path): that is not vendor evidence and is skipped.
    """
    rules = root / "debian/rules"
    if not rules.is_file():
        return [], []
    text = rules.read_text(encoding="utf-8")
    values: dict[str, str] = {}
    unsupported: dict[str, str] = {}  # variable -> the form that makes it unexpandable
    for m in re.finditer(r"^([A-Z_][A-Z0-9_]*)\s*([:?+]?=)\s*(.*?)\s*$", text, re.M):
        name, op, value = m.groups()
        if op in ("?=", "+="):
            unsupported[name] = op
        elif "$(shell" in value:
            unsupported[name] = "$(shell ...)"
        else:
            values[name] = value
    for name in unsupported:
        values.pop(name, None)
    values["CURDIR"] = str(root)

    def expand(word: str) -> str:
        for _ in range(10):  # bounded: a self-referencing variable cannot loop forever
            new = re.sub(r"\$\(([A-Z_][A-Z0-9_]*)\)", lambda m: values.get(m.group(1), m.group(0)), word)
            if new == word:
                break
            word = new
        return word

    out: list[Path] = []
    errors: list[str] = []
    for word in re.findall(r"\S+\.rules\b", text):
        used = set(re.findall(r"\$\(([A-Z_][A-Z0-9_]*)\)", word))
        bad = sorted(v for v in used if v in unsupported)
        expanded = expand(word)
        if bad:
            errors.append(f"debian/rules: {word} uses {', '.join(f'{v} ({unsupported[v]})' for v in bad)}, "
                          "which scan_udev_rules() cannot expand: assign it with := or =")
            continue
        if "$(" in expanded or "${" in expanded:
            text_prefix = re.split(r"\$[({]", expanded, maxsplit=1)[0]
            prefix, vendored = Path(text_prefix), str(root / EXTERNAL)
            if (prefix.is_absolute() and not prefix.is_relative_to(root / EXTERNAL)
                    and not vendored.startswith(text_prefix)):
                continue  # the text so far cannot lead into the vendored tree: not vendor evidence
            errors.append(f"debian/rules: {word} still holds an unresolved variable after expansion "
                          f"({expanded}): define it with := or = in debian/rules")
            continue
        path = Path(expanded)
        if path.is_absolute() and path.is_relative_to(root / EXTERNAL) and path.is_file() and path not in out:
            out.append(path)
    return out, sorted(set(errors))


def installed_udev_rules(root: Path) -> list[Path]:
    return scan_udev_rules(root)[0]


def newest_nightly_deb(paths: list[str]) -> tuple[str | None, list[str]]:
    """(newest path by its ``nightlyYYYYMMDDHHMM`` stamp, warnings). A file without a stamp is never chosen."""
    stamped, warnings = [], []
    for p in paths:
        m = re.search(r"nightly(\d{12})(?!\d)", Path(p).name)
        if m:
            stamped.append((m.group(1), p))
        else:
            warnings.append(f"{p}: no nightlyYYYYMMDDHHMM stamp, ignored")
    return (max(stamped)[1] if stamped else None), warnings


def newest_deb_in_dir(directory: Path) -> tuple[int, str | None, list[str]]:
    """(exit code, newest stamped .deb, warnings): 1 when .deb files exist and none has a stamp."""
    debs = sorted(str(p) for p in directory.glob("*.deb"))
    newest, warnings = newest_nightly_deb(debs)
    return (1 if debs and newest is None else 0), newest, warnings


def parse_runtime_system(root: Path) -> tuple[list[str], list[str]]:
    """Return (package names, errors) for RUNTIME_SYSTEM: a package whose evidence is gone is an error."""
    names, errors = [], list(scan_udev_rules(root)[1])
    for name, pattern, rx in RUNTIME_SYSTEM:
        files = installed_udev_rules(root) if pattern == INSTALLED_UDEV_RULES else sorted(root.glob(pattern))
        if any(re.search(rx, f.read_text(encoding="utf-8", errors="replace"), re.M)
               for f in files if f.is_file()):
            names.append(name)
        else:
            errors.append(f"{pattern}: nothing matches {rx!r}, the evidence that the daemon needs {name}: "
                          "update RUNTIME_SYSTEM in scripts/dependency_snapshot.py, or drop the package "
                          "if nothing needs it any more")
    return names, errors


def parse_apt_policy(text: str) -> dict[str, str]:
    """{package: candidate version} from `LC_ALL=C apt-cache policy PKG...`; no candidate, no entry."""
    out: dict[str, str] = {}
    pkg = None
    for line in text.splitlines():
        if line and line[0] not in " \t" and line.endswith(":"):
            pkg = line[:-1]
        elif pkg and line.strip().startswith("Candidate:"):
            ver = line.split(":", 1)[1].strip()
            if ver and ver != "(none)":
                out[pkg] = ver
    return out


def deb_relations(deb: dict[str, str]) -> list[tuple[str, str | None]]:
    """(package, exact version or None) for a built package's Depends and Recommends (dpkg-deb -f)."""
    return [rel for field in ("Depends", "Recommends") for rel in parse_relations(deb.get(field, ""))]


def apt_packages(root: Path, distro: str, deb: dict[str, str] | None = None) -> list[str]:
    """The unpinned apt packages the snapshot reports for distro ("debian" or "ubuntu")."""
    if distro == "ubuntu":
        return list(CI_APT_TOOLS)
    control = parse_control((root / "debian/control").read_text(encoding="utf-8"))
    names = [n for rels in control.values() for n, _ in rels] + [n for n, _, _ in RUNTIME_SYSTEM]
    names += [n for n, _ in deb_relations(deb or {})]
    return list(dict.fromkeys(names))


def deb_purl(name: str, version: str | None) -> str:
    name = urllib.parse.quote(name, safe="")  # purl: "+" in g++ is %2B
    return f"pkg:deb/debian/{name}{'@' + version if version else ''}?{DEB_QUALIFIERS}"


def versioned_ubuntu_purl(purl: str, cand: dict[str, str]) -> str:
    """Insert the candidate version into an unversioned pkg:deb/ubuntu purl; others pass through."""
    head, sep, quals = purl.partition("?")
    name = head.removeprefix("pkg:deb/ubuntu/")
    if name == head or "@" in name or name not in cand:
        return purl
    return f"{head}@{cand[name]}{sep}{quals}"


def manifest(path: str, entries: list[tuple[str, str]]) -> dict:
    """entries: (purl, scope). Skips duplicates."""
    resolved = {}
    for purl, scope in entries:
        resolved.setdefault(purl, {"package_url": purl, "relationship": "direct", "scope": scope})
    return {"name": path, "file": {"source_location": path}, "resolved": resolved}


def build_snapshot(root: Path, sha: str, ref: str, job_id: str, scanned: str,
                   candidates: dict[str, dict[str, str]] | None = None, deb: dict[str, str] | None = None) -> dict:
    """candidates: {"debian"|"ubuntu": parse_apt_policy()} versions the unpinned deb entries.
    deb: a built package's control fields (dpkg-deb -f), whose resolved Depends and Recommends become
    the DEB_MANIFEST manifest."""
    candidates = candidates or {}
    deb_cand, ubuntu_cand = candidates.get("debian", {}), candidates.get("ubuntu", {})
    manifests = {}
    control = parse_control((root / "debian/control").read_text(encoding="utf-8"))
    entries = [(deb_purl(n, v or deb_cand.get(n)), "development" if key == "development" else "runtime")
               for key, rels in control.items() for n, v in rels]
    manifests["debian/control"] = manifest("debian/control", entries)
    names, _ = parse_runtime_system(root)
    manifests[RUNTIME_MANIFEST] = {**manifest("scripts/dependency_snapshot.py",
                                              [(deb_purl(n, deb_cand.get(n)), "runtime") for n in names]),
                                   "name": RUNTIME_MANIFEST}
    if deb:
        manifests[DEB_MANIFEST] = {**manifest(DEB_SOURCE, [(deb_purl(n, v or deb_cand.get(n)), "runtime")
                                                           for n, v in deb_relations(deb)]),
                                   "name": DEB_MANIFEST}
    cm = "AlpacaHTTP/CMakeLists.txt"
    fetched = parse_fetchcontent((root / cm).read_text(encoding="utf-8"))
    manifests[cm] = manifest(cm, [(f"pkg:github/{repo}@{tag}", "runtime") for repo, tag in fetched])
    table = json.loads((root / TABLE).read_text(encoding="utf-8"))
    sdk = []
    for e in table["sdks"]:
        ver = e.get("version")
        sdk.append((f"pkg:generic/{e['name']}{'@' + ver if ver else ''}", "runtime"))
    manifests[TABLE] = manifest(TABLE, sdk)
    if (root / CI).is_file():
        tools, _ = parse_ci_tools((root / CI).read_text(encoding="utf-8"))
        tools = [versioned_ubuntu_purl(p, ubuntu_cand) for p in tools]
        manifests[CI] = manifest(CI, [(p, "development") for p in tools])
    for path, steps in WORKFLOW_TOOL_STEPS.items():
        if (root / path).is_file():
            pins, _ = parse_step_pins((root / path).read_text(encoding="utf-8"), path, steps)
            manifests[path] = manifest(path, [(p, "development") for p in pins])
    if (root / CONFORMU_DOC).is_file():
        cu, _ = parse_conformu((root / CONFORMU_DOC).read_text(encoding="utf-8"))
        manifests[CONFORMU_DOC] = manifest(CONFORMU_DOC, [(p, "development") for p in cu])
    # Job container images: release.yml and nightly-build.yml build the published .deb in one.
    wf_dir = root / ".github/workflows"
    for wf in sorted(wf_dir.glob("*.yml")) if wf_dir.is_dir() else []:
        images = parse_containers(wf.read_text(encoding="utf-8"))
        if images:
            rel = wf.relative_to(root).as_posix()
            m = manifest(rel, [(p, "development") for p in images])
            if rel in manifests:  # ci.yml also carries the CI tools
                m["resolved"] = {**manifests[rel]["resolved"], **m["resolved"]}
            manifests[rel] = m
    # The dependency graph listed no action pins for the workflows this snapshot names, while it
    # listed them for every workflow it does not (2026-10-16), so each workflow manifest here
    # carries its own workflow's pins.
    for rel, m in manifests.items():
        if rel.startswith(".github/workflows/"):
            pins = parse_uses((root / rel).read_text(encoding="utf-8"))
            m["resolved"] = {**m["resolved"], **manifest(rel, [(p, "development") for p in pins])["resolved"]}
    return {"version": 0, "job": {"correlator": "dependency-submission", "id": job_id},
            "sha": sha, "ref": ref, "detector": DETECTOR, "scanned": scanned,
            "manifests": manifests}


def installed_packages() -> dict[str, str]:
    """{package: version} installed in this environment, from dpkg-query."""
    query = subprocess.run(["dpkg-query", "-W", "-f=${db:Status-Abbrev}\\t${Package}\\t${Version}\\n"],
                           check=True, capture_output=True, text=True).stdout
    return parse_installed(query)


def read_deb_fields(path: Path) -> dict[str, str] | None:
    """A built package's Depends and Recommends (dpkg-deb -f), or None with a warning when it cannot be
    read, so a truncated nightly download costs the deb manifest rather than the whole snapshot."""
    try:
        out = subprocess.run(["dpkg-deb", "-f", str(path), "Depends", "Recommends"],
                             check=True, capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        print(f"WARNING: cannot read {path} ({e}); leaving out the resolved Depends", file=sys.stderr)
        return None
    return parse_deb_fields(out)


def parse_deb_fields(text: str) -> dict[str, str]:
    """Fields of `dpkg-deb -f`, with continuation lines (leading space) folded into their field."""
    out: dict[str, str] = {}
    cur = None
    for line in text.splitlines():
        if line[:1] in " \t" and cur:
            out[cur] += " " + line.strip()
        elif ":" in line:
            cur, _, val = line.partition(":")
            out[cur] = val.strip()
    return out


def parse_installed(text: str) -> dict[str, str]:
    """{package: version} from `dpkg-query -W -f='${db:Status-Abbrev}\\t${Package}\\t${Version}\\n'`, installed rows only."""
    out = {}
    for line in text.splitlines():
        parts = line.split("\t")
        if len(parts) == 3 and parts[0].strip() == "ii":
            out[parts[1]] = parts[2]
    return out


def file_digests(data: bytes) -> dict[str, str]:
    return {"sha1": hashlib.sha1(data).hexdigest(), "sha256": hashlib.sha256(data).hexdigest()}


def tracked_files(root: Path, rel: str) -> list[tuple[str, bool]]:
    """(path, is_symlink) for every git-tracked file under rel, sorted. The pins and the
    --spdx match both use this set, so an untracked file can satisfy neither."""
    # safe.directory for this one read: the release jobs run as root in a container over a
    # checkout owned by the runner user, and git refuses such a repository by default.
    proc = subprocess.run(["git", "-c", f"safe.directory={root.resolve()}", "-C", str(root),
                           "ls-files", "-s", "-z", "--", rel], capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"git ls-files {rel} failed: {proc.stderr.strip()}")
    out = proc.stdout
    entries = []
    for entry in sorted(e for e in out.split("\0") if e):
        meta, path = entry.split("\t", 1)
        entries.append((path, meta.split()[0] == "120000"))
    return entries


def tree_digest(root: Path, rel: str) -> str:
    """SHA-256 over the git-tracked files under rel: path, then file bytes (or symlink target).
    A tracked file missing from the working tree hashes as missing, so it reads as drift."""
    h = hashlib.sha256()
    for path, is_link in tracked_files(root, rel):
        p = root / path
        if is_link and p.is_symlink():
            data = b"L" + os.readlink(p).encode()
        elif not is_link and p.is_file():
            data = b"F" + hashlib.sha256(p.read_bytes()).digest()
        else:
            data = b"M"
        h.update(path.encode() + b"\0" + data + b"\n")
    return h.hexdigest()


def pin_sdk_hashes(root: Path) -> None:
    """Rewrite every 'sdks' entry's sha256 pin, keeping the table's one-entry-per-line layout."""
    table = json.loads((root / TABLE).read_text(encoding="utf-8"))
    for e in table["sdks"]:
        e["sha256"] = tree_digest(root, e["path"])
    # Every top-level key survives, one list item or mapping entry per line, so --pin never
    # drops a section (documents, no_upstream, vendors) it does not know about.
    blocks = []
    for key, value in table.items():
        if isinstance(value, list):
            inner = ",\n".join(f"    {json.dumps(v)}" for v in value)
            blocks.append(f"  {json.dumps(key)}: [\n{inner}\n  ]" if value else f"  {json.dumps(key)}: []")
        elif isinstance(value, dict):
            inner = ",\n".join(f"    {json.dumps(k)}: {json.dumps(v)}" for k, v in value.items())
            blocks.append(f"  {json.dumps(key)}: {{\n{inner}\n  }}" if value else f"  {json.dumps(key)}: {{}}")
        else:
            blocks.append(f"  {json.dumps(key)}: {json.dumps(value)}")
    lines = ["{", ",\n".join(blocks), "}"]
    (root / TABLE).write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_spdx(root: Path, deb: dict[str, str], file_name: str, sha256: str,
               created: str, namespace: str, payload: dict[str, dict[str, str]] | None = None,
               installed: dict[str, str] | None = None) -> dict:
    """SPDX 2.3 SBOM for one built .deb: the same inventory as the snapshot, rooted at the package.

    deb: the package's control fields (dpkg-deb -f). Runtime dependencies come from its
    Depends, where ${shlibs:Depends} is already resolved; vendored SDKs and FetchContent
    libraries are CONTAINS; Build-Depends are BUILD_DEPENDENCY_OF. Both kinds of dependency are
    at the version in installed (dpkg-query in the build environment) when one is given.

    payload: {path inside the .deb: file_digests()} for the shipped vendor files. Each must be
    byte-identical to a file under a listed SDK's path, and is reported as a file CONTAINS-ed by
    that SDK; one that matches none raises ValueError.
    """
    payload = payload or {}
    installed = installed or {}
    packages: list[dict] = []
    ids: dict[str, str] = {}

    def add(purl: str, name: str, version: str | None) -> str:
        if purl in ids:
            return ids[purl]
        spdx_id = f"SPDXRef-Package-{len(packages)}"
        pkg = {"name": name, "SPDXID": spdx_id, "downloadLocation": "NOASSERTION", "filesAnalyzed": False,
               "externalRefs": [{"referenceCategory": "PACKAGE-MANAGER", "referenceType": "purl",
                                 "referenceLocator": purl}]}
        if version:
            pkg["versionInfo"] = version
        packages.append(pkg)
        ids[purl] = spdx_id
        return spdx_id

    root_id = add(deb_purl(deb["Package"], deb["Version"]), deb["Package"], deb["Version"])
    packages[0]["packageFileName"] = file_name
    packages[0]["checksums"] = [{"algorithm": "SHA256", "checksumValue": sha256}]
    rels = [("SPDXRef-DOCUMENT", "DESCRIBES", root_id)]
    for n, v in parse_relations(deb.get("Depends", "")):
        v = installed.get(n, v)
        rels.append((root_id, "DEPENDS_ON", add(deb_purl(n, v), n, v)))
    for repo, tag in parse_fetchcontent((root / "AlpacaHTTP/CMakeLists.txt").read_text(encoding="utf-8")):
        rels.append((root_id, "CONTAINS", add(f"pkg:github/{repo}@{tag}", repo, tag)))
    vendored: dict[str, str] = {}
    for e in json.loads((root / TABLE).read_text(encoding="utf-8"))["sdks"]:
        ver = e.get("version")
        sdk_id = add(f"pkg:generic/{e['name']}{'@' + ver if ver else ''}", e["name"], ver)
        rels.append((root_id, "CONTAINS", sdk_id))
        for path, is_link in tracked_files(root, e["path"]):
            p = root / path
            if not is_link and p.is_file():
                vendored.setdefault(hashlib.sha256(p.read_bytes()).hexdigest(), sdk_id)
    files = []
    unmatched = []
    for path in sorted(payload):
        if path.startswith(BUILT_FROM_VENDORED_SOURCE):
            continue
        d = payload[path]
        sdk_id = vendored.get(d["sha256"])
        if sdk_id is None:
            unmatched.append(path)
            continue
        file_id = f"SPDXRef-File-{len(files)}"
        files.append({"fileName": f"./{path}", "SPDXID": file_id,
                      "checksums": [{"algorithm": "SHA1", "checksumValue": d["sha1"]},
                                    {"algorithm": "SHA256", "checksumValue": d["sha256"]}]})
        rels.append((sdk_id, "CONTAINS", file_id))
    if unmatched:
        raise ValueError("shipped vendor files match no file under a 'sdks' path in "
                         f"{TABLE} (changed in packaging, or not vendored): " + ", ".join(unmatched))
    control = parse_control((root / "debian/control").read_text(encoding="utf-8"))
    for n, v in control["development"]:
        v = installed.get(n, v)
        rels.append((add(deb_purl(n, v), n, v), "BUILD_DEPENDENCY_OF", root_id))
    rels = list(dict.fromkeys(rels))
    return {"spdxVersion": "SPDX-2.3", "dataLicense": "CC0-1.0", "SPDXID": "SPDXRef-DOCUMENT",
            "name": file_name, "documentNamespace": namespace,
            "creationInfo": {"created": created,
                             "creators": [f"Tool: {DETECTOR['name']}-{DETECTOR['version']}"]},
            "packages": packages,
            "files": files,
            "relationships": [{"spdxElementId": a, "relationshipType": t, "relatedSpdxElement": b}
                              for a, t, b in rels]}


def check_drift(root: Path) -> list[str]:
    table = json.loads((root / TABLE).read_text(encoding="utf-8"))
    sdks = [e["path"] for e in table["sdks"]]
    docs = list(table["doc_only"])
    listed = sdks + docs
    errors = []
    for p in listed:
        if not (root / p).is_dir():
            errors.append(f"{TABLE}: stale entry {p} (directory gone): remove it from 'sdks' or 'doc_only'")
    for e in table["sdks"]:
        if not (root / e["path"]).is_dir():
            continue
        if not e.get("sha256"):
            errors.append(f"{TABLE}: {e['path']} has no sha256 pin: run python3 scripts/dependency_snapshot.py --pin")
        elif e["sha256"] != tree_digest(root, e["path"]):
            errors.append(f"{TABLE}: the vendored files under {e['path']} changed since their sha256 pin: "
                          "check 'version' and 'source', then run python3 scripts/dependency_snapshot.py --pin")

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
    if (root / CI).is_file():
        errors += parse_ci_tools((root / CI).read_text(encoding="utf-8"))[1]
    for path, steps in WORKFLOW_TOOL_STEPS.items():
        if (root / path).is_file():
            errors += parse_step_pins((root / path).read_text(encoding="utf-8"), path, steps)[1]
    errors += check_coverage(root, table)
    for d in table.get("documents", []):
        if d.get("path") and not (root / d["path"]).exists():
            errors.append(f"{TABLE}: document {d['name']!r} points at {d['path']}, which does not exist: "
                          "fix its 'path' or remove the entry")
    if (root / CONFORMU_DOC).is_file():
        errors += parse_conformu((root / CONFORMU_DOC).read_text(encoding="utf-8"))[1]
    errors += parse_runtime_system(root)[1]
    return errors


def check_coverage(root: Path, table: dict) -> list[str]:
    """The update-coverage rules (docs/development.md "Dependency update checks"): a new SDK,
    protocol folder, vendor driver or pinned workflow tool cannot arrive without an update check."""
    errors = []
    documents = table.get("documents", [])
    libraries = table.get("system_libraries", [])
    names = {e["name"] for e in table["sdks"]} | {d["name"] for d in documents} | {lib["name"] for lib in libraries if lib.get("name")}
    # Debian-provided libraries need an upstream to compare and the baseline Debian ships.
    for lib in libraries:
        missing = [k for k in ("name", "repo", "baseline") if not lib.get(k)]
        if missing:
            errors.append(f"{TABLE}: system_libraries entry {lib.get('name', '?')!r} has no {', '.join(missing)}: "
                          "give its GitHub repo (owner/name) and the version Debian trixie ships as baseline")
    # Rule 1: every SDK has an update source.
    for e in table["sdks"]:
        if not (e.get("updates") or {}).get("provider"):
            errors.append(f"{TABLE}: SDK {e['name']!r} has no 'updates' block: add where "
                          "scripts/dependency_updates.py finds its newest version (provider release-page or json)")
    # Rule 2: every docs-only folder holds a watched document, or says why there is none.
    exempt = {n["path"] for n in table.get("no_upstream", [])}
    for p in sorted(exempt - set(table["doc_only"])):
        errors.append(f"{TABLE}: no_upstream lists {p}, which is not a 'doc_only' directory: remove the stale entry")
    doc_paths = [d["path"] for d in documents if d.get("path")]
    for p in table["doc_only"]:
        if p not in exempt and not any(dp.startswith(p + "/") for dp in doc_paths):
            errors.append(f"{TABLE}: {p} has no watched document: add a 'documents' entry whose path is "
                          "under it, or a 'no_upstream' entry with the reason it has no upstream")
    # Rule 3: every vendor driver directory maps to what it is built from.
    vendors = table.get("vendors", {})
    vdir = root / VENDORS
    present = sorted(c.name for c in vdir.iterdir() if c.is_dir()) if vdir.is_dir() else []
    for v in present:
        m = vendors.get(v) or {}
        if m.get("no_upstream"):
            continue
        tracked = m.get("tracked") or []
        if not tracked:
            errors.append(f"{TABLE}: vendor {VENDORS}/{v} is not in 'vendors': list the SDKs or documents it "
                          "is built from under 'tracked', or give a 'no_upstream' reason")
        for n in tracked:
            if n not in names:
                errors.append(f"{TABLE}: vendors[{v!r}] tracks {n!r}, which is no SDK or document name")
    for v in vendors:
        if vdir.is_dir() and v not in present:
            errors.append(f"{TABLE}: vendors[{v!r}] has no {VENDORS}/{v} directory: remove the entry")
    # Rule 4: every ver= pin in a workflow belongs to a step the snapshot reads.
    wf_dir = root / ".github/workflows"
    for wf in sorted(wf_dir.glob("*.yml")) if wf_dir.is_dir() else []:
        rel = wf.relative_to(root).as_posix()
        tracked_steps = {step for step, _ in (CI_TOOL_STEPS if rel == CI else WORKFLOW_TOOL_STEPS.get(rel, ()))}
        step = None
        for line in wf.read_text(encoding="utf-8").splitlines():
            m = re.match(r"^\s*- name:\s*(.+?)\s*$", line)
            if m:
                step = m.group(1).strip("\"'")  # YAML quotes are not part of the name
            elif re.match(r"^\s*ver=", line) and step not in tracked_steps:
                errors.append(f"{rel}: the step {step!r} pins a tool with ver= that the dependency snapshot does "
                              "not read: add it to CI_TOOL_STEPS (ci.yml) or WORKFLOW_TOOL_STEPS in "
                              "scripts/dependency_snapshot.py")
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
    ci = ("jobs:\n  a:\n    steps:\n      - name: Install deps\n        run: |\n"
          "          sudo apt-get install -y \\\n            catch2 \\\n            gcovr  \\\n            cmake\n"
          "          sudo apt-get install -y --no-install-recommends clang-format\n"
          "          sudo apt-get install -y clang-tidy shellcheck nodejs\n"
          "          echo nodejs-is-not-a-package-here\n"
          "      - name: Build cppcheck 2.17.x from source\n        run: |\n"
          "          ver=2.17.1\n          sha256=00\n"
          "      - name: Install zizmor\n        run: |\n          ver=1.25.2\n"
          "      - name: Later\n        run: ver=9.9.9\n")
    tools, errs = parse_ci_tools(ci)
    apt = [f"pkg:deb/ubuntu/{t}?{CI_APT_QUALIFIERS}" for t in CI_APT_TOOLS]
    assert errs == [] and tools == ["pkg:github/danmar/cppcheck@2.17.1",
                                    "pkg:github/zizmorcore/zizmor@v1.25.2", *apt], (tools, errs)
    assert "cmake" in parse_apt_installs(ci) and "-y" not in parse_apt_installs(ci)
    assert "echo" not in parse_apt_installs(ci), "a continuation ends at a line without a backslash"
    # A commented-out install is not an install.
    assert parse_apt_installs("    # sudo apt-get install -y nodejs\n") == set()
    # The install command ends at a shell separator: a name after && or ; is not installed.
    for sep in (" && echo nodejs", "; echo nodejs", " | tee nodejs"):
        line = f"sudo apt-get install -y gcovr{sep}\n"
        assert parse_apt_installs(line) == {"gcovr"}, (sep, parse_apt_installs(line))
    # A reworded step or a dropped apt line is reported, never silently omitted.
    tools, errs = parse_ci_tools(ci.replace("Install zizmor", "Fetch zizmor").replace("catch2 ", ""))
    assert tools == ["pkg:github/danmar/cppcheck@2.17.1", *apt[1:]], tools
    assert len(errs) == 2 and "'Install zizmor'" in errs[0] and "catch2" in errs[1], errs
    # A tool named outside an apt-get install (here: an echo) is not an install.
    _, errs = parse_ci_tools(ci.replace(" shellcheck nodejs", " shellcheck"))
    assert len(errs) == 1 and "nodejs" in errs[0], errs
    # A step with no ver= pin does not borrow the next step's.
    _, errs = parse_ci_tools(ci.replace("          ver=2.17.1\n", ""))
    assert len(errs) == 1 and "cppcheck" in errs[0], errs
    cu_doc = ("curl -o cu.tar.xz 'https://github.com/ASCOMInitiative/ConformU/releases/download/v4.5.2/conformu.linux-arm64.tar.xz'\n"
              "line it prints against the build, `4.5.2.56139, Build time:\nFri 16 October 2026` -- ok\n")
    # Pinned to a GitHub release: the bare version, no -beta.
    assert parse_conformu(cu_doc) == (["pkg:github/ASCOMInitiative/ConformU@4.5.2"], [])
    # Still on the ASCOM beta download: a beta.
    beta = cu_doc.replace("github.com/ASCOMInitiative/ConformU/releases/download/v4.5.2/", "download.ascom-standards.org/beta/")
    assert parse_conformu(beta) == (["pkg:github/ASCOMInitiative/ConformU@4.5.2-beta"], [])
    # Two different pinned builds: the doc is ambiguous, so it is an error, not the first one.
    cu, errs = parse_conformu("older `4.5.1.54507, Build time: Thu` here\n" + cu_doc)
    assert cu == [] and len(errs) == 1 and "4.5.1.54507" in errs[0] and "4.5.2.56139" in errs[0], errs
    # The same build named twice is not ambiguous.
    assert parse_conformu(cu_doc + cu_doc)[0] == ["pkg:github/ASCOMInitiative/ConformU@4.5.2"]
    cu, errs = parse_conformu("go back to the ordinary release asset\n")
    assert cu == [] and len(errs) == 1 and CONFORMU_DOC in errs[0], errs
    wf = ("jobs:\n  a:\n    container: debian:trixie\n  b:\n    container: debian:trixie  # dup\n"
          "  c:\n    container:\n      credentials:\n        username: u\n      options: --cpus 1\n"
          "      image: ghcr.io/o/img@sha256:ab\n    steps: []\n"
          "  d:\n    container: ${{ matrix.c }}\n"
          "  e:\n    container: alpine\n"
          "  f:\n    container:\n      options: --privileged\n    steps:\n      - image: not-mine\n")
    # image: after credentials:/options: is found; a registry goes to repository_url; an image:
    # outside the container mapping (job f) is not the container's.
    assert parse_containers(wf) == ["pkg:docker/debian@trixie",
                                    "pkg:docker/o/img@sha256%3Aab?repository_url=ghcr.io",
                                    "pkg:docker/alpine@latest"], parse_containers(wf)
    assert parse_containers("    container: localhost:5000/team/img:1\n") == [
        "pkg:docker/team/img@1?repository_url=localhost%3A5000"], parse_containers("container: localhost:5000/team/img:1")
    with tempfile.TemporaryDirectory() as d:
        r = Path(d)
        (r / "debian").mkdir()
        (r / "debian/control").write_text(ctl)
        (r / "AlpacaHTTP").mkdir()
        (r / "AlpacaHTTP/CMakeLists.txt").write_text(cm)
        (r / "scripts").mkdir()
        for p in ("A/sdk-1", "B", "Q/deep"):
            (r / EXTERNAL / p).mkdir(parents=True)
        # One evidence line per RUNTIME_SYSTEM package, so the tree below is drift-free.
        evidence = {"debian/alpacabridge.service": "ExecStart=/usr/bin/alpacabridge\n",
                    "AlpacaHTTP/src/util/software_update.cpp": "int r = sd_bus_open_system(&bus.bus);\n",
                    "debian/rules": ("RULES_DIR := $(CURDIR)/AlpacaCore/external\n"
                                     "\tinstall -d $(STAGING)/usr/lib/udev/rules.d\n"
                                     "\tinstall -m 644 $(RULES_DIR)/x.rules \\\n"
                                     "\t  $(STAGING)/usr/lib/udev/rules.d/\n"),
                    "debian/alpacabridge-software-update": "#!/bin/bash\napt-get update\ndpkg-query -W x\n",
                    f"{EXTERNAL}/x.rules": "RUN+=\"/bin/sh -c '/bin/echo 256 >/sys/x'\"\nRUN+=\"/bin/sleep 5\"\n"}
        for p, text in evidence.items():
            (r / p).parent.mkdir(parents=True, exist_ok=True)
            (r / p).write_text(text)
        subprocess.run(["git", "-C", d, "init", "-q"], check=True)  # vendored files are git-tracked files
        up = {"provider": "release-page", "url": "https://example.test/", "pattern": r"sdk-(\d+)"}
        (r / TABLE).write_text(json.dumps({"sdks": [
            {"path": f"{EXTERNAL}/A/sdk-1", "name": "a-sdk", "version": "1", "updates": up},
            {"path": f"{EXTERNAL}/Q/deep", "name": "q-sdk", "version": None, "updates": up}],
            "doc_only": [f"{EXTERNAL}/B"],
            "no_upstream": [{"path": f"{EXTERNAL}/B", "reason": "test fixture"}]}))
        assert [e for e in check_drift(r) if "no sha256 pin" not in e] == [], check_drift(r)
        # --- coverage rules: a new SDK, protocol folder, vendor or pinned tool fails --check ---
        base = json.loads((r / TABLE).read_text())

        def drift_with(**changes):
            (r / TABLE).write_text(json.dumps({**base, **changes}))
            try:
                return [e for e in check_drift(r) if "no sha256 pin" not in e]
            finally:
                (r / TABLE).write_text(json.dumps(base))

        # Rule 1: every SDK has an update source.
        errs = drift_with(sdks=[{**base["sdks"][0], "updates": None}, base["sdks"][1]])
        assert len(errs) == 1 and "a-sdk" in errs[0] and "updates" in errs[0], errs
        # Rule 2: every docs-only folder has a watched document under it, or a no_upstream reason.
        errs = drift_with(no_upstream=[])
        assert len(errs) == 1 and f"{EXTERNAL}/B" in errs[0] and "documents" in errs[0], errs
        (r / EXTERNAL / "B/spec.md").write_text("spec")
        assert drift_with(no_upstream=[], documents=[{"name": "b-spec", "path": f"{EXTERNAL}/B/spec.md",
                                                      "check": {"provider": "hash", "url": "u"}, "baseline": "0"}]) == []
        (r / EXTERNAL / "B/spec.md").unlink()
        # A no_upstream exemption for a directory that is no longer docs-only is stale.
        errs = drift_with(no_upstream=base["no_upstream"] + [{"path": f"{EXTERNAL}/Gone", "reason": "old"}])
        assert len(errs) == 1 and f"{EXTERNAL}/Gone" in errs[0] and "no_upstream" in errs[0], errs
        # Rule 3: every vendor driver directory maps to SDKs or documents, or has a no_upstream reason.
        (r / VENDORS / "acme").mkdir(parents=True)
        errs = drift_with()
        assert len(errs) == 1 and "acme" in errs[0] and "vendors" in errs[0], errs
        assert drift_with(vendors={"acme": {"tracked": ["a-sdk"]}}) == []
        assert drift_with(vendors={"acme": {"no_upstream": "reads a user-configured feed"}}) == []
        # A Debian-provided library tracked against its upstream is a valid vendors name; an entry
        # without its upstream repository or baseline is not checkable and fails.
        lib = {"name": "libacme", "repo": "acme/libacme", "debian": "libacme", "baseline": "1.0"}
        assert drift_with(system_libraries=[lib], vendors={"acme": {"tracked": ["libacme"]}}) == []
        errs = drift_with(system_libraries=[{k: v for k, v in lib.items() if k != "repo"}],
                          vendors={"acme": {"tracked": ["libacme"]}})
        assert len(errs) == 1 and "libacme" in errs[0] and "repo" in errs[0], errs
        errs = drift_with(vendors={"acme": {"tracked": ["gone-sdk"]}})
        assert len(errs) == 1 and "gone-sdk" in errs[0], errs
        errs = drift_with(vendors={"acme": {"tracked": ["a-sdk"]}, "old": {"tracked": ["a-sdk"]}})
        assert len(errs) == 1 and "'old'" in errs[0], "a vendor key with no directory is stale"
        (r / VENDORS / "acme").rmdir()
        # Rule 4: every ver= pin in a workflow belongs to a step the snapshot reads.
        (r / ".github/workflows").mkdir(parents=True)
        (r / ".github/workflows/x.yml").write_text(
            "jobs:\n  a:\n    steps:\n      - name: Install newtool\n        run: |\n          ver=1.2.3\n")
        errs = drift_with()
        assert len(errs) == 1 and "x.yml" in errs[0] and "Install newtool" in errs[0], errs
        # A quoted step name is the same step: a tracked pin written `- name: "..."` still matches.
        osv_step = WORKFLOW_TOOL_STEPS[".github/workflows/osv-scan.yml"][0][0]
        (r / ".github/workflows/x.yml").unlink()
        (r / ".github/workflows/osv-scan.yml").write_text(
            f'jobs:\n  a:\n    steps:\n      - name: "{osv_step}"\n        run: |\n          ver=2.7.0\n')
        assert drift_with() == [], drift_with()
        (r / ".github/workflows/osv-scan.yml").rename(r / ".github/workflows/x.yml")
        (r / ".github/workflows/x.yml").unlink()
        (r / ".github/workflows").rmdir()
        (r / ".github").rmdir()
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
        assert CI not in snap["manifests"], "no ci.yml, no CI-tools manifest"
        (r / ".github/workflows").mkdir(parents=True)
        (r / CI).write_text(ci)
        snap = build_snapshot(r, "a" * 40, "refs/heads/main", "1", "2026-01-01T00:00:00Z")
        assert set(snap["manifests"][CI]["resolved"]) == set(parse_ci_tools(ci)[0])
        assert all(x["scope"] == "development" for x in snap["manifests"][CI]["resolved"].values())
        (r / ".github/workflows/rel.yml").write_text(wf)
        snap = build_snapshot(r, "a" * 40, "refs/heads/main", "1", "2026-01-01T00:00:00Z")
        assert "pkg:docker/debian@trixie" in snap["manifests"][".github/workflows/rel.yml"]["resolved"]
        assert ".github/workflows/ci.yml" in snap["manifests"], "a workflow with no container still has its own"
        # ci.yml with a container keeps its tool entries and gains the image (merge, not overwrite).
        (r / CI).write_text(ci + "  z:\n    container: debian:trixie\n")
        merged = build_snapshot(r, "a" * 40, "refs/heads/main", "1", "2026-01-01T00:00:00Z")["manifests"][CI]["resolved"]
        assert "pkg:docker/debian@trixie" in merged and "pkg:github/danmar/cppcheck@2.17.1" in merged, merged
        (r / CI).write_text(ci)
        (r / ".github/workflows/rel.yml").unlink()
        osv = "jobs:\n  s:\n    steps:\n      - name: Install OSV-Scanner\n        run: |\n          ver=2.7.0\n"
        osv_path = next(iter(WORKFLOW_TOOL_STEPS))
        (r / osv_path).write_text(osv)
        snap = build_snapshot(r, "a" * 40, "refs/heads/main", "1", "2026-01-01T00:00:00Z")
        assert list(snap["manifests"][osv_path]["resolved"]) == ["pkg:github/google/osv-scanner@v2.7.0"]
        (r / osv_path).write_text(osv.replace("ver=2.7.0", "ver=latest"))
        assert any(osv_path in e and "Install OSV-Scanner" in e for e in check_drift(r)), check_drift(r)
        (r / osv_path).unlink()
        (r / CI).write_text(ci.replace("ver=1.25.2", "ver=latest"))
        assert any("'Install zizmor'" in e for e in check_drift(r)), check_drift(r)
        (r / CI).unlink()
        (r / ".claude/commands").mkdir(parents=True)
        (r / CONFORMU_DOC).write_text(cu_doc)
        snap = build_snapshot(r, "a" * 40, "refs/heads/main", "1", "2026-01-01T00:00:00Z")
        assert snap["manifests"][CONFORMU_DOC]["resolved"]["pkg:github/ASCOMInitiative/ConformU@4.5.2"][
            "scope"] == "development"
        (r / CONFORMU_DOC).write_text("no pin\n")
        assert any(CONFORMU_DOC in e for e in check_drift(r)), check_drift(r)
        (r / CONFORMU_DOC).unlink()
        assert snap["manifests"]["debian/control"]["resolved"][deb_purl("cmake", None)]["scope"] == "development"
        assert snap["manifests"]["debian/control"]["resolved"][deb_purl("adduser", None)]["scope"] == "runtime"
        # apt-cache policy candidates version the unpinned deb entries; an exact (= X) pin wins and a
        # package with no candidate stays unversioned.
        policy = ("cmake:\n  Installed: (none)\n  Candidate: 3.31.6-2\n  Version table:\n"
                  "     3.31.6-2 500\n        500 http://deb.debian.org/debian trixie/main arm64 Packages\n"
                  "libfoo:\n  Installed: 1.2-3\n  Candidate: 9.9-9\n"
                  "g++:\n  Installed: (none)\n  Candidate: (none)\n"
                  "libusb-1.0-0:\n  Installed: (none)\n  Candidate: 2:1.0.28-1\n")
        assert parse_apt_policy(policy) == {"cmake": "3.31.6-2", "libfoo": "9.9-9",
                                            "libusb-1.0-0": "2:1.0.28-1"}, parse_apt_policy(policy)
        (r / CI).write_text(ci)
        cand = {"debian": parse_apt_policy(policy),
                "ubuntu": parse_apt_policy("catch2:\n  Installed: (none)\n  Candidate: 3.4.0-1build1\n")}
        snap = build_snapshot(r, "a" * 40, "refs/heads/main", "1", "2026-01-01T00:00:00Z", candidates=cand)
        dc = snap["manifests"]["debian/control"]["resolved"]
        assert deb_purl("cmake", "3.31.6-2") in dc and deb_purl("libusb-1.0-0", "2:1.0.28-1") in dc, dc
        assert deb_purl("libfoo", "1.2-3") in dc, "an exact pin is not replaced by the candidate"
        assert deb_purl("g++", None) in dc and deb_purl("adduser", None) in dc, dc
        ct = snap["manifests"][CI]["resolved"]
        assert f"pkg:deb/ubuntu/catch2@3.4.0-1build1?{CI_APT_QUALIFIERS}" in ct, ct
        assert f"pkg:deb/ubuntu/gcovr?{CI_APT_QUALIFIERS}" in ct, ct
        runtime_names = ["systemd", "dbus", "udev", "apt", "dpkg", "bash", "dash", "coreutils"]
        assert [n for n, _, _ in RUNTIME_SYSTEM] == runtime_names
        assert apt_packages(r, "debian") == ["cmake", "g++", "libgpiod-dev", "libfoo", "libcurl4-openssl-dev",
                                             "libusb-1.0-0", "adduser", "polkitd", *runtime_names], apt_packages(r, "debian")
        assert apt_packages(r, "ubuntu") == list(CI_APT_TOOLS)
        # Recommends are runtime, and so is every package the daemon needs that Depends does not name.
        assert dc[deb_purl("polkitd", None)]["scope"] == "runtime", dc
        rs = snap["manifests"][RUNTIME_MANIFEST]
        assert rs["file"]["source_location"] == "scripts/dependency_snapshot.py", rs
        assert set(rs["resolved"]) == {deb_purl(n, None) for n in runtime_names}, rs
        assert all(x["scope"] == "runtime" for x in rs["resolved"].values())
        # Evidence that goes away is a --check error naming the package, never a silent drop.

        def drift():
            return [e for e in check_drift(r) if "no sha256 pin" not in e]

        assert drift() == [], drift()
        (r / "debian/alpacabridge.service").write_text("ExecStart=/usr/bin/other\n")
        errs = drift()
        assert len(errs) == 1 and "systemd" in errs[0] and "debian/alpacabridge.service" in errs[0], errs
        gone = build_snapshot(r, "a" * 40, "refs/heads/main", "1", "2026-01-01T00:00:00Z")
        assert deb_purl("systemd", None) not in gone["manifests"][RUNTIME_MANIFEST]["resolved"]
        (r / "debian/alpacabridge.service").write_text(evidence["debian/alpacabridge.service"])
        (r / EXTERNAL / "x.rules").unlink()
        errs = drift()
        assert len(errs) == 2 and "dash" in errs[0] and "coreutils" in errs[1], errs
        # Only the rules debian/rules installs are evidence (make variables expanded): a vendor copy
        # the package never ships keeps nothing listed.
        assert installed_udev_rules(r) == [], installed_udev_rules(r)
        (r / EXTERNAL / "B/copy.rules").write_text(evidence[f"{EXTERNAL}/x.rules"])
        # A .rules file debian/rules installs from outside the vendored tree (a polkit rule, a staged
        # copy) is not vendor udev evidence either.
        (r / "debian/local.rules").write_text(evidence[f"{EXTERNAL}/x.rules"])
        (r / "debian/rules").write_text(evidence["debian/rules"] + "\tinstall -m 644 $(CURDIR)/debian/local.rules x/\n")
        assert installed_udev_rules(r) == [], installed_udev_rules(r)
        errs = drift()
        assert len(errs) == 2 and "dash" in errs[0] and "coreutils" in errs[1], errs
        (r / "debian/rules").write_text(evidence["debian/rules"])
        (r / "debian/local.rules").unlink()
        (r / EXTERNAL / "B/copy.rules").unlink()
        (r / EXTERNAL / "x.rules").write_text(evidence[f"{EXTERNAL}/x.rules"])
        assert installed_udev_rules(r) == [r / EXTERNAL / "x.rules"], installed_udev_rules(r)
        assert scan_udev_rules(r)[1] == [] and drift() == [], scan_udev_rules(r)
        # A .rules path built with a form the expansion cannot follow is an error, not a skipped path.
        good = evidence["debian/rules"]
        for form, rule_text in (
                ("?=", good.replace("RULES_DIR :=", "RULES_DIR ?=")),
                ("+=", good.replace("RULES_DIR :=", "RULES_DIR +=")),
                ("$(shell ...)", good.replace("$(CURDIR)/AlpacaCore/external", "$(shell echo x)")),
                ("unresolved", good.replace("$(RULES_DIR)/x.rules", "$(MISSING_DIR)/x.rules"))):
            (r / "debian/rules").write_text(rule_text)
            errs = scan_udev_rules(r)[1]
            assert len(errs) == 1 and "debian/rules" in errs[0] and "x.rules" in errs[0], (form, errs)
            if form != "unresolved":
                assert f"({form})" in errs[0], (form, errs)
            assert any("x.rules" in e for e in drift()), (form, drift())
        # An unresolved variable in a path already outside the vendored tree is not vendor evidence: no error.
        (r / "debian/rules").write_text(good + "\tinstall -m 644 $(CURDIR)/debian/$(UNDEFINED)/y.rules x/\n")
        assert scan_udev_rules(r)[1] == [], scan_udev_rules(r)
        # A prefix that is an ancestor of the vendored tree could still lead into it: error.
        for anc in ("$(CURDIR)/$(UNDEFINED)/x.rules", "$(CURDIR)/AlpacaCore/$(UNDEFINED)/x.rules"):
            (r / "debian/rules").write_text(good + f"\tinstall -m 644 {anc} x/\n")
            assert len(scan_udev_rules(r)[1]) == 1, (anc, scan_udev_rules(r))
        # Only the $(shell) rule catches a variable whose value is a shell call inside an otherwise known path.
        (r / "debian/rules").write_text(good + "SH := /opt/$(shell echo a)\n\tinstall -m 644 $(SH)/z.rules x/\n")
        assert any("($(shell ...))" in e for e in scan_udev_rules(r)[1]), scan_udev_rules(r)
        (r / "debian/rules").write_text(good)
        assert drift() == [], drift()
        # The pick of the newest nightly .deb orders by its stamp, never by the full name.
        old, new_ = "alpacabridge_5.0.9-nightly202610150300_arm64.deb", "alpacabridge_5.0.10-nightly202610160300_arm64.deb"
        assert sorted([new_, old])[-1] == old  # the full-name sort this replaces picks the wrong file
        assert newest_nightly_deb([old, new_]) == (new_, []), newest_nightly_deb([old, new_])
        assert newest_nightly_deb([new_, old])[0] == new_
        pick, warns = newest_nightly_deb(["zzz_arm64.deb", old])
        assert pick == old and len(warns) == 1 and "zzz_arm64.deb" in warns[0], (pick, warns)
        assert newest_nightly_deb(["zzz_arm64.deb"])[0] is None
        with tempfile.TemporaryDirectory() as nd:
            dd = Path(nd)
            assert newest_deb_in_dir(dd) == (0, None, []), newest_deb_in_dir(dd)
            (dd / "foo_arm64.deb").write_text("")
            assert newest_deb_in_dir(dd)[0] == 1, newest_deb_in_dir(dd)
            (dd / old).write_text("")
            assert newest_deb_in_dir(dd)[:2] == (0, str(dd / old)), newest_deb_in_dir(dd)
        # Every workflow manifest the snapshot submits also carries that workflow's action pins:
        # GitHub drops its own scan of a workflow file a submitted snapshot names.
        pin = "c" * 40
        assert parse_uses(f"      - uses: actions/checkout@{pin}  # v7.0.1\n"
                          "      - uses: ./.github/actions/local\n"
                          "      - uses: docker://alpine@sha256:ab\n"
                          "    uses: o/r/.github/workflows/w.yml@v1\n"
                          "      - uses: \"o/q@v2\"\n"
                          "      # - uses: o/commented@v3\n") == [
            f"pkg:githubactions/actions/checkout@{pin}",
            "pkg:githubactions/o/r/.github/workflows/w.yml@v1", "pkg:githubactions/o/q@v2"]
        (r / CI).write_text(ci + f"      - uses: actions/checkout@{pin}  # v7.0.1\n")
        (r / ".github/workflows/rel.yml").write_text(
            f"jobs:\n  a:\n    container: debian:trixie\n    steps:\n      - uses: o/upload-sarif@{pin}\n")
        (r / ".github/workflows/plain.yml").write_text(f"jobs:\n  a:\n    steps:\n      - uses: o/plain@{pin}\n")
        ms = build_snapshot(r, "a" * 40, "refs/heads/main", "1", "2026-01-01T00:00:00Z")["manifests"]
        assert ms[CI]["resolved"][f"pkg:githubactions/actions/checkout@{pin}"]["scope"] == "development", ms[CI]
        assert "pkg:github/danmar/cppcheck@2.17.1" in ms[CI]["resolved"], "the tools stay"
        assert f"pkg:githubactions/o/upload-sarif@{pin}" in ms[".github/workflows/rel.yml"]["resolved"]
        assert ".github/workflows/plain.yml" not in ms, "a workflow the snapshot does not name stays GitHub's"
        (r / ".github/workflows/rel.yml").unlink()
        (r / ".github/workflows/plain.yml").unlink()
        (r / CI).unlink()
        deb = {"Package": "alpacabridge", "Version": "5.0.0~beta2", "Architecture": "arm64",
               "Depends": "libc6 (>= 2.38), libgpiod3 (>= 2.1), libusb-1.0-0 (>= 2:1.0.16), adduser"}
        # --deb: a built package's Depends (shlibs resolved) and Recommends go in the MAIN snapshot, so
        # every push submits them with the current commit, in a manifest whose source_location no other
        # manifest uses, versioned from the same trixie apt candidates.
        built = {**deb, "Recommends": "network-manager, polkitd"}
        assert "libc6" in apt_packages(r, "debian", deb=built) and "libc6" not in apt_packages(r, "debian")
        ms = build_snapshot(r, "a" * 40, "refs/heads/main", "1", "2026-01-01T00:00:00Z",
                            candidates={"debian": {"libc6": "2.41-12"}}, deb=built)["manifests"]
        dm = ms[DEB_MANIFEST]
        assert dm["name"] == DEB_MANIFEST and dm["file"]["source_location"] == DEB_SOURCE, dm
        assert [m["file"]["source_location"] for m in ms.values()].count(DEB_SOURCE) == 1, ms
        assert set(dm["resolved"]) == {deb_purl("libc6", "2.41-12"), deb_purl("libgpiod3", None),
                                       deb_purl("libusb-1.0-0", None), deb_purl("adduser", None),
                                       deb_purl("network-manager", None), deb_purl("polkitd", None)}, dm
        assert all(x["scope"] == "runtime" and x["relationship"] == "direct" for x in dm["resolved"].values())
        # A .deb that cannot be read (a truncated download; no dpkg-deb here) is a warning, not a
        # failure: the snapshot is then submitted without the deb manifest, as with no nightly at all.
        (r / "broken.deb").write_bytes(b"not an ar archive")
        assert read_deb_fields(r / "broken.deb") is None
        nodeb = build_snapshot(r, "a" * 40, "refs/heads/main", "1", "2026-01-01T00:00:00Z")
        assert DEB_MANIFEST not in nodeb["manifests"] and nodeb["job"]["correlator"] == "dependency-submission"
        doc = build_spdx(r, deb, "x.deb", "f" * 64, "2026-01-01T00:00:00Z", "https://example.test/sbom")
        assert doc["spdxVersion"] == "SPDX-2.3" and doc["SPDXID"] == "SPDXRef-DOCUMENT"
        assert doc["dataLicense"] == "CC0-1.0" and doc["documentNamespace"] == "https://example.test/sbom"
        pkgs = {p["SPDXID"]: p for p in doc["packages"]}
        assert len(pkgs) == len(doc["packages"]), "SPDXIDs must be unique"
        purl = {p["externalRefs"][0]["referenceLocator"]: p["SPDXID"] for p in doc["packages"]}
        root_id = purl["pkg:deb/debian/alpacabridge@5.0.0~beta2?arch=arm64&distro=trixie"]
        assert pkgs[root_id]["checksums"] == [{"algorithm": "SHA256", "checksumValue": "f" * 64}]
        assert pkgs[root_id]["packageFileName"] == "x.deb"
        rels = {(x["spdxElementId"], x["relationshipType"], x["relatedSpdxElement"]) for x in doc["relationships"]}
        assert ("SPDXRef-DOCUMENT", "DESCRIBES", root_id) in rels
        # Runtime: the BUILT package's Depends (shlibs resolved), not debian/control's.
        for name in ("libc6", "libgpiod3", "libusb-1.0-0", "adduser"):
            assert (root_id, "DEPENDS_ON", purl[deb_purl(name, None)]) in rels, name
        assert deb_purl("libusb-1.0-0", None) in purl and deb_purl("libc6", None) in purl
        # Vendored SDKs and FetchContent libraries ship inside the package.
        for p in ("pkg:generic/a-sdk@1", "pkg:generic/q-sdk", "pkg:github/nlohmann/json@v3.11.3"):
            assert (root_id, "CONTAINS", purl[p]) in rels, p
        # Build-Depends are build-only.
        assert (purl[deb_purl("cmake", None)], "BUILD_DEPENDENCY_OF", root_id) in rels
        assert (purl[deb_purl("libfoo", "1.2-3")], "BUILD_DEPENDENCY_OF", root_id) in rels
        assert len(rels) == len(doc["relationships"]), "no duplicate relationships"
        # Runtime dependencies carry the version installed in the build environment; one that is
        # not installed keeps no version (a >= constraint is never recorded as a version).
        doc_rt = build_spdx(r, deb, "x.deb", "f" * 64, "t", "n",
                            installed={"libc6": "2.41-12", "libusb-1.0-0": "2:1.0.27-2"})
        rt = {p["name"]: p for p in doc_rt["packages"]}
        assert rt["libc6"]["versionInfo"] == "2.41-12", rt["libc6"]
        assert rt["libusb-1.0-0"]["versionInfo"] == "2:1.0.27-2", rt["libusb-1.0-0"]
        assert "versionInfo" not in rt["adduser"], "a runtime dep not installed stays unversioned"
        rt_purls = {p["externalRefs"][0]["referenceLocator"] for p in doc_rt["packages"]}
        assert deb_purl("libc6", "2.41-12") in rt_purls and deb_purl("libc6", None) not in rt_purls
        assert "versionInfo" not in rt["libgpiod3"], "a >= constraint is never recorded as a version"
        assert deb_purl("libgpiod3", None) in rt_purls
        rt_rels = {(x["spdxElementId"], x["relationshipType"], x["relatedSpdxElement"]) for x in doc_rt["relationships"]}
        rt_root = next(p["SPDXID"] for p in doc_rt["packages"] if p["name"] == "alpacabridge")
        assert (rt_root, "DEPENDS_ON", rt["libc6"]["SPDXID"]) in rt_rels
        # An alternative "a | b" is recorded as its first branch; installing only b changes nothing.
        alt = dict(deb, Depends="liba | libb")
        for inst, want in (({"libb": "1"}, None), ({"liba": "2"}, "2")):
            dalt = build_spdx(r, alt, "x.deb", "f" * 64, "t", "n", installed=inst)
            names = {p["name"]: p for p in dalt["packages"]}
            assert "liba" in names and "libb" not in names, names.keys()
            assert names["liba"].get("versionInfo") == want, (inst, names["liba"])
        for p in doc["packages"]:
            assert {"name", "SPDXID", "downloadLocation", "filesAnalyzed"} <= set(p), p
        # --- Vendored file pins and the shipped-file gate ---
        lib, fw = b"A-lib-bytes", b"Q-firmware"
        (r / EXTERNAL / "A/sdk-1/lib").mkdir()
        (r / EXTERNAL / "A/sdk-1/lib/liba.so.1.0").write_bytes(lib)
        (r / EXTERNAL / "A/sdk-1/lib/liba.so").symlink_to("liba.so.1.0")
        (r / EXTERNAL / "Q/deep/qhy.hex").write_bytes(fw)
        subprocess.run(["git", "-C", d, "init", "-q"], check=True)
        subprocess.run(["git", "-C", d, "add", "-A"], check=True)
        errs = check_drift(r)
        assert len(errs) == 2 and all("no sha256 pin" in e for e in errs), errs
        t = json.loads((r / TABLE).read_text())
        t["documents"] = [{"name": "b-doc", "path": f"{EXTERNAL}/B/doc.md",
                           "check": {"provider": "hash", "url": "https://example.test/b.pdf"}, "baseline": "00"}]
        (r / TABLE).write_text(json.dumps(t))
        (r / EXTERNAL / "B/doc.md").write_text("doc")
        pin_sdk_hashes(r)
        assert check_drift(r) == [], check_drift(r)
        kept = json.loads((r / TABLE).read_text())
        assert kept["documents"] == t["documents"] and kept["no_upstream"] == t["no_upstream"], \
            "--pin keeps every top-level section"
        (r / EXTERNAL / "B/doc.md").unlink()
        errs = check_drift(r)
        assert len(errs) == 1 and "b-doc" in errs[0] and f"{EXTERNAL}/B/doc.md" in errs[0], errs
        t = json.loads((r / TABLE).read_text())
        del t["documents"]
        (r / TABLE).write_text(json.dumps(t))
        pinned = t["sdks"][0]["sha256"]
        assert re.fullmatch(r"[0-9a-f]{64}", pinned) and pinned == tree_digest(r, f"{EXTERNAL}/A/sdk-1")
        (r / EXTERNAL / "A/sdk-1/lib/liba.so.1.0").write_bytes(b"swapped")
        errs = check_drift(r)
        assert len(errs) == 1 and f"{EXTERNAL}/A/sdk-1" in errs[0] and "--pin" in errs[0], errs
        (r / EXTERNAL / "A/sdk-1/lib/liba.so.1.0").write_bytes(lib)
        assert check_drift(r) == [], "restoring the bytes restores the pin"
        (r / EXTERNAL / "A/sdk-1/lib/liba.so").unlink()
        (r / EXTERNAL / "A/sdk-1/lib/liba.so").symlink_to("other")
        assert len(check_drift(r)) == 1, "a retargeted symlink changes the digest"
        (r / EXTERNAL / "A/sdk-1/lib/liba.so").unlink()
        (r / EXTERNAL / "A/sdk-1/lib/liba.so").symlink_to("liba.so.1.0")
        payload = {"usr/lib/alpacabridge/liba.so.1.0": file_digests(lib),
                   "usr/lib/firmware/qhy/qhy.hex": file_digests(fw)}
        installed = {"cmake": "3.31.6-2", "libfoo": "1.2-3", "libc6": "2.41-12"}
        doc = build_spdx(r, deb, "x.deb", "f" * 64, "2026-01-01T00:00:00Z", "https://example.test/sbom",
                         payload=payload, installed=installed)
        purl = {p["externalRefs"][0]["referenceLocator"]: p["SPDXID"] for p in doc["packages"]}
        pkgs = {p["SPDXID"]: p for p in doc["packages"]}
        files = {f["fileName"]: f for f in doc["files"]}
        assert set(files) == {"./usr/lib/alpacabridge/liba.so.1.0", "./usr/lib/firmware/qhy/qhy.hex"}, files
        f_lib = files["./usr/lib/alpacabridge/liba.so.1.0"]
        assert {"algorithm": "SHA256", "checksumValue": hashlib.sha256(lib).hexdigest()} in f_lib["checksums"]
        assert {"algorithm": "SHA1", "checksumValue": hashlib.sha1(lib).hexdigest()} in f_lib["checksums"]
        rels = {(x["spdxElementId"], x["relationshipType"], x["relatedSpdxElement"]) for x in doc["relationships"]}
        assert (purl["pkg:generic/a-sdk@1"], "CONTAINS", f_lib["SPDXID"]) in rels
        assert (purl["pkg:generic/q-sdk"], "CONTAINS", files["./usr/lib/firmware/qhy/qhy.hex"]["SPDXID"]) in rels
        # Build-Depends carry the version installed in the build environment.
        cmake = pkgs[purl[deb_purl("cmake", "3.31.6-2")]]
        assert cmake["versionInfo"] == "3.31.6-2"
        assert (purl[deb_purl("cmake", "3.31.6-2")], "BUILD_DEPENDENCY_OF", purl[
            "pkg:deb/debian/alpacabridge@5.0.0~beta2?arch=arm64&distro=trixie"]) in rels
        assert deb_purl("g++", None) in purl, "a build dep not installed keeps no version"
        # dpkg-deb -f output: a folded field keeps its continuation lines.
        fields = parse_deb_fields("Package: alpacabridge\nVersion: 5.0.0\n"
                                  "Depends: libc6 (>= 2.38),\n libgpiod3 (>= 2.1),\n adduser\nArchitecture: arm64\n")
        assert fields["Depends"] == "libc6 (>= 2.38), libgpiod3 (>= 2.1), adduser", fields
        assert fields["Package"] == "alpacabridge" and fields["Architecture"] == "arm64", fields
        # dpkg-query: only installed ("ii") rows give a version.
        inst = parse_installed("ii \tcmake\t3.31.6-2\nrc \tlibold\t1.0\nii \tg++\t4:14.2.0-1\n")
        assert inst == {"cmake": "3.31.6-2", "g++": "4:14.2.0-1"}, inst
        # An untracked file under an SDK path is not vendored: it must not satisfy the gate.
        (r / EXTERNAL / "A/sdk-1/lib/untracked.so").write_bytes(b"untracked-bytes")
        try:
            build_spdx(r, deb, "x.deb", "f" * 64, "t", "n",
                       payload={"usr/lib/alpacabridge/untracked.so": file_digests(b"untracked-bytes")}, installed={})
        except ValueError as e:
            assert "usr/lib/alpacabridge/untracked.so" in str(e), e
        else:
            raise AssertionError("an untracked file must not match")
        (r / EXTERNAL / "A/sdk-1/lib/untracked.so").unlink()
        # A tracked file deleted from the working tree is drift, not a traceback.
        (r / EXTERNAL / "Q/deep/qhy.hex").unlink()
        errs = check_drift(r)
        assert len(errs) == 1 and f"{EXTERNAL}/Q/deep" in errs[0] and "--pin" in errs[0], errs
        (r / EXTERNAL / "Q/deep/qhy.hex").write_bytes(fw)
        assert check_drift(r) == [], check_drift(r)
        # A shipped vendor file that matches no vendored file fails the SBOM.
        try:
            build_spdx(r, deb, "x.deb", "f" * 64, "t", "n",
                       payload={"usr/lib/alpacabridge/libevil.so": file_digests(b"evil")}, installed={})
        except ValueError as e:
            assert "usr/lib/alpacabridge/libevil.so" in str(e), e
        else:
            raise AssertionError("an unmatched shipped file must fail")
        # A file built from a vendored tarball is skipped, but only inside its own subtree.
        built = {"usr/lib/alpacabridge/libgphoto2/lib/libgphoto2.so.6": file_digests(b"built")}
        doc = build_spdx(r, deb, "x.deb", "f" * 64, "t", "n", payload=built, installed={})
        assert doc.get("files", []) == [], doc.get("files")
        try:
            build_spdx(r, deb, "x.deb", "f" * 64, "t", "n",
                       payload={"usr/lib/alpacabridge/libgphoto2.so.6": file_digests(b"built")}, installed={})
        except ValueError as e:
            assert "usr/lib/alpacabridge/libgphoto2.so.6" in str(e), e
        else:
            raise AssertionError("a file next to the built subtree must still fail")
        (r / EXTERNAL / "New").mkdir()
        errs = check_drift(r)
        assert len(errs) == 1 and f"{EXTERNAL}/New is not in {TABLE}" in errs[0], errs
        (r / EXTERNAL / "New").rmdir()
        (r / EXTERNAL / "Q/new").mkdir()
        errs = check_drift(r)
        assert len(errs) == 1 and f"{EXTERNAL}/Q/new is not in {TABLE}" in errs[0], errs
        (r / EXTERNAL / "Q/new").rmdir()
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
    ap.add_argument("--spdx", metavar="DEB")
    ap.add_argument("--deb", metavar="DEB", help="a built .deb whose resolved Depends and Recommends to report")
    ap.add_argument("--newest-deb", metavar="DIR", help="print the newest nightlyYYYYMMDDHHMM .deb in DIR")
    ap.add_argument("--pin", action="store_true")
    ap.add_argument("--apt-packages", choices=("debian", "ubuntu"))
    ap.add_argument("--debian-policy", metavar="FILE", help="apt-cache policy output from debian:trixie")
    ap.add_argument("--ubuntu-policy", metavar="FILE", help="apt-cache policy output from the CI runner")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    deb = read_deb_fields(Path(a.deb)) if a.deb else None
    if a.apt_packages:
        print("\n".join(apt_packages(ROOT, a.apt_packages, deb)))
        return 0
    if a.newest_deb:
        code, newest, warnings = newest_deb_in_dir(Path(a.newest_deb))
        for w in warnings:
            print(f"::warning::{w}", file=sys.stderr)
        if code:
            print("ERROR: no .deb carries a nightlyYYYYMMDDHHMM stamp", file=sys.stderr)
        elif newest:
            print(newest)
        return code
    if a.pin:
        pin_sdk_hashes(ROOT)
        print(f"{TABLE}: sha256 pins rewritten")
        return 0
    if a.spdx:
        deb_path = Path(a.spdx)
        fields = ("Package", "Version", "Architecture", "Depends")
        out = subprocess.run(["dpkg-deb", "-f", str(deb_path), *fields],
                             check=True, capture_output=True, text=True).stdout
        deb = parse_deb_fields(out)
        sha256 = hashlib.sha256(deb_path.read_bytes()).hexdigest()
        payload = {}
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run(["dpkg-deb", "-x", str(deb_path), tmp], check=True)
            for top in SHIPPED_VENDOR_DIRS:
                for dirpath, _, names in os.walk(Path(tmp) / top):
                    for n in names:
                        p = Path(dirpath) / n
                        if p.is_file() and not p.is_symlink():
                            payload[p.relative_to(tmp).as_posix()] = file_digests(p.read_bytes())
        installed = installed_packages()
        repo = os.environ.get("GITHUB_REPOSITORY", "open-astro/AlpacaBridge")
        created = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        try:
            doc = build_spdx(ROOT, deb, deb_path.name, sha256, created,
                             f"https://github.com/{repo}/sbom/{deb_path.name}-{sha256}",
                             payload=payload, installed=installed)
        except ValueError as e:
            print(f"ERROR: {e}", file=sys.stderr)
            return 1
        text = json.dumps(doc, indent=2) + "\n"
        if a.output:
            Path(a.output).write_text(text, encoding="utf-8")
        else:
            sys.stdout.write(text)
        return 0
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
    candidates = {}
    for distro, path in (("debian", a.debian_policy), ("ubuntu", a.ubuntu_policy)):
        if path:
            candidates[distro] = parse_apt_policy(Path(path).read_text(encoding="utf-8"))
            missing = [n for n in apt_packages(ROOT, distro, deb if distro == "debian" else None)
                       if n not in candidates[distro]]
            if missing:
                print(f"WARNING: no {distro} apt candidate (left unversioned): {', '.join(missing)}",
                      file=sys.stderr)
    text = json.dumps(build_snapshot(ROOT, sha, ref, job_id, scanned, candidates, deb), indent=2) + "\n"
    if a.output:
        Path(a.output).write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
