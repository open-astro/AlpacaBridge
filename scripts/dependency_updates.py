#!/usr/bin/env python3
"""Open one GitHub issue per dependency that has a newer upstream version.

The dependencies are the ones scripts/dependency_snapshot.py submits to the dependency
graph, so there is one inventory. What is checked:

- every ``pkg:github/OWNER/REPO@VERSION`` entry (cppcheck, zizmor, OSV-Scanner, nlohmann/json,
  ConformU): the repository's latest GitHub release, or its highest version tag without one;
- every SDK in scripts/dependency_sdk_versions.json, through its ``updates`` block: provider
  ``release-page`` (the highest version a regex matches on a page) or ``json`` (a field of the
  matching entry in a JSON list, ZWO's SDK endpoint);
- every entry in that file's ``documents`` list (protocol documents, the INDI sources drivers
  were derived from, the gphoto2 tool, the ConformU beta build): provider ``github-file`` (the
  newest commit on a file), ``hash`` (sha256 of the document, or of an HTML page's visible text),
  ``page`` (the highest version a regex matches) or ``github-release`` (the repository's latest
  release), against the recorded ``baseline``;
- every entry in its ``system_libraries`` list (libraries the build takes from Debian, such as
  LibRaw): the upstream repository's latest release against the version Debian
  trixie ships.

Other Debian, Ubuntu and docker entries are not checked: apt and the image tag already resolve
to the newest package at build time, and .github/workflows/osv-scan.yml reports their
vulnerabilities. An SDK with no ``updates`` block is listed as a manual check. Update and
document issues end with a "## Changelog" section when the source has one.

Issues (state lives in a marker comment in each issue body; nothing is committed):
- a newer version opens "Update <name> <current> -> <latest>" (dependencies, enhancement, P4),
  and a changed document "Upstream document changed: <name> ..." (dependencies, documentation, P4);
  a still newer one retitles it with a comment; the repository reaching it closes it;
- a source that cannot be read opens "Update check broken: <name>" (dependencies, bug, P4),
  closed when the source reads again, so a changed page is never mistaken for "no update".

--osv-summary reads osv-scanner JSON and opens one issue (dependencies, bug, P2) listing the
actionable findings no earlier summary issue reported: CVSS >= 7.0, a ``fixed`` version in the
package's Debian trixie entry, not rated "unimportant" by Debian, one row per advisory and
source package however many binary packages report it.

  dependency_updates.py [--dry-run]            check and update issues (gh, GH_TOKEN, GH_REPO)
  dependency_updates.py --osv-summary FILE [--dry-run]
  dependency_updates.py --self-test
"""

from __future__ import annotations

import argparse
import contextlib
import gzip
import hashlib
import html
import http.client
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import zlib
from pathlib import Path
from typing import Callable

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dependency_snapshot as snapshot  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
MARKER = "dependency-updates"
MARKER_RE = re.compile(r"<!-- " + MARKER + r": (\{.*?\}) -->")
UPDATE_LABELS = ("dependencies", "enhancement", "P4")
BROKEN_LABELS = ("dependencies", "bug", "P4")
DOC_LABELS = ("dependencies", "documentation", "P4")
OSV_LABELS = ("dependencies", "bug", "P2")
TAG_PAGES = 10
PAGE_TIMEOUT = 30  # seconds per page or API request; about 45 run in sequence each week
DOCUMENT_TIMEOUT = 300  # a hashed document can be large (the ~50 MB ConformU beta tarball)
LIST_LIMIT = 5000  # issues read back for their markers; the OSV dedupe needs closed ones too
OSV_THRESHOLD = 7.0  # CVSS: High is 7.0-8.9, Critical 9.0-10.0
OSV_MARKER = "osv2"  # "osv" was the first format (#1135), which listed unfixable findings
OSV_MAX_ROWS = 100  # rows per summary issue; keeps the body far below GitHub's 65536 characters

HttpGet = Callable[[str], str]
Fetch = Callable[[str], "tuple[bytes, str]"]  # url -> (body, content type)


class FetchError(Exception):
    pass


def parse_version(v: str) -> tuple[tuple[int, ...], bool]:
    """(numeric parts, is a pre-release) for '3.11.3', 'v1.25.2', '4.5.1-beta', '2.3'."""
    m = re.match(r"^[vV]?(\d+(?:\.\d+)*)(.*)$", v.strip())
    if not m:
        raise ValueError(f"not a version: {v!r}")
    parts = tuple(int(x) for x in m.group(1).split("."))
    while len(parts) > 1 and parts[-1] == 0:
        parts = parts[:-1]
    return parts, bool(re.search(r"(alpha|beta|rc|pre|dev)", m.group(2), re.I))


def is_newer(latest: str, current: str) -> bool:
    (lt, lpre), (ct, cpre) = parse_version(latest), parse_version(current)
    return lt > ct or (lt == ct and cpre and not lpre)


def highest(versions: list[str]) -> str | None:
    stable = []
    for v in versions:
        try:
            parts, pre = parse_version(v)
        except ValueError:
            continue
        if not pre:
            stable.append((parts, v))
    return max(stable)[1] if stable else None


def http_fetch(url: str, timeout: int = PAGE_TIMEOUT) -> tuple[bytes, str]:
    """(body, content type). A gzip body is decompressed: urllib does not, and some vendor
    sites send one even unasked."""
    headers = {"User-Agent": "Mozilla/5.0 (compatible; alpacabridge-dependency-updates)"}
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if token and url.startswith("https://api.github.com/"):
        headers["Authorization"] = f"Bearer {token}"
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=timeout) as r:
            return decode_body(r.read(), r.headers.get("Content-Encoding", "")), r.headers.get("Content-Type", "")
    except urllib.error.HTTPError as e:
        raise FetchError(f"HTTP {e.code} from {url}") from e
    except (urllib.error.URLError, TimeoutError, OSError, http.client.HTTPException) as e:
        # HTTPException covers a connection dropped mid-body (IncompleteRead): a broken source.
        raise FetchError(f"{url}: {type(e).__name__}: {e}") from e


def decode_body(body: bytes, encoding: str) -> bytes:
    if encoding.strip().lower() != "gzip":
        return body
    try:
        return gzip.decompress(body)
    except (OSError, EOFError, zlib.error) as e:  # a truncated or corrupt body: a broken source
        raise FetchError(f"corrupt gzip body: {e}") from e


def http_get(url: str) -> str:
    return http_fetch(url)[0].decode("utf-8", errors="replace")


def latest_github(repo: str, get: HttpGet) -> str:
    try:
        tag = json.loads(get(f"https://api.github.com/repos/{repo}/releases/latest"))["tag_name"]
        if highest([tag]):
            return tag
    except FetchError as e:
        if "HTTP 404" not in str(e):
            raise
    # /tags is ordered by ref, not by version, so read every page (up to TAG_PAGES).
    tags: list[str] = []
    for page in range(1, TAG_PAGES + 1):
        url = f"https://api.github.com/repos/{repo}/tags?per_page=100" + (f"&page={page}" if page > 1 else "")
        try:
            batch = [t["name"] for t in json.loads(get(url))]
        except FetchError as e:
            if page > 1 and "HTTP 404" in str(e):
                break
            raise
        tags += batch
        if len(batch) < 100:
            break
    else:
        print(f"warning: {repo}: read only the first {TAG_PAGES * 100} tags; a higher version "
              "may be beyond them", file=sys.stderr)
    best = highest(tags)
    if not best:
        raise FetchError(f"{repo}: no release and no version tag")
    return best


def latest_json_field(url: str, path: str, match: dict, field: str, get: HttpGet) -> str | None:
    """Any ``field`` of the entry matching ``match`` (a changelog URL), or None if it has none."""
    node = json.loads(get(url))
    for seg in path.split("."):
        node = node[int(seg)] if seg.isdigit() else node[seg]
    for entry in node if isinstance(node, list) else []:
        if isinstance(entry, dict) and all(entry.get(k) == v for k, v in match.items()):
            return entry.get(field) or None
    return None


def latest_json(url: str, path: str, match: dict, field: str, get: HttpGet) -> str:
    """The ``field`` of the entry matching ``match`` in the list at dotted ``path`` (ZWO's SDK list)."""
    node = json.loads(get(url))
    try:
        for seg in path.split("."):
            node = node[int(seg)] if seg.isdigit() else node[seg]
    except (KeyError, IndexError, TypeError) as e:
        raise FetchError(f"{url}: no {path!r} in the response (has the format changed?)") from e
    for entry in node if isinstance(node, list) else []:
        if isinstance(entry, dict) and all(entry.get(k) == v for k, v in match.items()):
            if field in entry and highest([str(entry[field])]):
                return str(entry[field])
            raise FetchError(f"{url}: entry {match} has no version in {field!r}")
    raise FetchError(f"{url}: no entry matches {match} (has the list changed?)")


def latest_file_commit(repo: str, path: str, get: HttpGet) -> str:
    """The newest commit on the default branch that touched ``path``."""
    commits = json.loads(get(f"https://api.github.com/repos/{repo}/commits?path={urllib.parse.quote(path)}&per_page=1"))
    if not commits:
        raise FetchError(f"{repo}: no commit touches {path} (moved or deleted?)")
    return commits[0]["sha"]


def normalised_text(html_text: str) -> str:
    """Visible text of an HTML page: no scripts, styles, comments or tags, whitespace collapsed."""
    t = re.sub(r"(?is)<(script|style|noscript)[^>]*>.*?</\1>", " ", html_text)
    t = re.sub(r"(?s)<!--.*?-->", " ", t)
    t = re.sub(r"(?s)<[^>]+>", " ", t)
    return re.sub(r"\s+", " ", html.unescape(t)).strip()


def content_hash(url: str, fetch: Fetch) -> str:
    """sha256 of a document: its bytes, or for an HTML page its normalised visible text."""
    body, ctype = fetch(url)
    if "html" in ctype.lower():
        body = normalised_text(body.decode("utf-8", errors="replace")).encode("utf-8")
    return hashlib.sha256(body).hexdigest()


def latest_page(url: str, pattern: str, get: HttpGet) -> str:
    best = highest(re.findall(pattern, get(url)))
    if not best:
        raise FetchError(f"{url}: no version matches {pattern!r} (has the page changed?)")
    return best


def targets(snap: dict, table: dict) -> tuple[list[dict], list[str]]:
    """(checkable dependencies, names of SDKs with no update source).

    Each target has ``compare``: "version" (an update is a higher version) or "equal" (a
    document: a commit SHA or content hash that differs from the recorded baseline).
    """
    out: dict[str, dict] = {}
    for path, man in snap["manifests"].items():
        for purl in man["resolved"]:
            m = re.match(r"^pkg:github/([^/@]+/[^/@?]+)@([^?#]+)$", purl)
            if m:
                repo, ver = m.groups()
                out.setdefault(repo.lower(), {"key": repo.lower(), "name": repo, "current": ver, "where": path,
                                              "check": ("github", repo), "source": f"https://github.com/{repo}/releases",
                                              "compare": "version", "document": False})
    manual = []
    for e in table["sdks"]:
        up = e.get("updates") or {}
        base = {"key": e["name"], "name": e["name"], "current": e.get("version"), "where": snapshot.TABLE,
                "source": up.get("url", ""), "compare": "version", "document": False}
        if up.get("changelog"):
            base["changelog"] = up["changelog"]
        if up.get("provider") == "release-page" and e.get("version"):
            out.setdefault(e["name"], {**base, "check": ("page", up["url"], up["pattern"])})
        elif up.get("provider") == "json" and e.get("version"):
            out.setdefault(e["name"], {**base, "check": ("json", up["url"], up["path"], up["match"], up["field"])})
            if up.get("changelog_field"):
                out[e["name"]]["changelog_json"] = (up["url"], up["path"], up["match"], up["changelog_field"])
        else:
            manual.append(e["name"])
    for d in table.get("documents", []):
        c = d["check"]
        base = {"key": "doc:" + d["name"], "name": d["name"], "current": d["baseline"], "where": snapshot.TABLE,
                "document": True, "path": d.get("path")}
        if c["provider"] == "github-file":
            out["doc:" + d["name"]] = {**base, "check": ("github-file", c["repo"], c["file"]), "compare": "equal",
                                       "source": f"https://github.com/{c['repo']}/commits/HEAD/{c['file']}"}
        elif c["provider"] == "hash":
            out["doc:" + d["name"]] = {**base, "check": ("hash", c["url"]), "compare": "equal", "source": c["url"]}
        elif c["provider"] == "page":
            out["doc:" + d["name"]] = {**base, "check": ("page", c["url"], c["pattern"]), "compare": "version",
                                       "source": c["url"]}
        elif c["provider"] == "github-release":
            out["doc:" + d["name"]] = {**base, "check": ("github", c["repo"]), "compare": "version",
                                       "source": f"https://github.com/{c['repo']}/releases"}
        else:
            manual.append(d["name"])
    # Libraries the build takes from Debian (debian/control), checked against their upstream
    # releases; the baseline is the version Debian trixie ships.
    for lib in table.get("system_libraries", []):
        out[lib["name"]] = {"key": lib["name"], "name": lib["name"], "current": lib["baseline"],
                            "where": snapshot.TABLE, "check": ("github", lib["repo"]), "compare": "version",
                            "document": False, "system": True, "debian": lib.get("debian", lib["name"]),
                            "source": f"https://github.com/{lib['repo']}/releases"}
    return sorted(out.values(), key=lambda t: t["key"]), manual


def check(t: dict, get: HttpGet, fetch: Fetch | None = None) -> dict:
    fetch = fetch or (lambda u: http_fetch(u, timeout=DOCUMENT_TIMEOUT))
    try:
        if t.get("compare", "version") == "version":
            parse_version(t["current"])  # a pin that is no version (a commit SHA) is a broken entry
        kind = t["check"][0]
        if kind == "github":
            latest = latest_github(t["check"][1], get)
        elif kind == "json":
            latest = latest_json(*t["check"][1:], get)
        elif kind == "github-file":
            latest = latest_file_commit(t["check"][1], t["check"][2], get)
        elif kind == "hash":
            latest = content_hash(t["check"][1], fetch)
        else:
            latest = latest_page(t["check"][1], t["check"][2], get)
        return {**t, "latest": latest, "error": None}
    except (FetchError, KeyError, ValueError, json.JSONDecodeError) as e:
        return {**t, "latest": None, "error": str(e)}


def differs(r: dict) -> bool:
    """True when upstream is ahead of the repository (or, for a document, has changed)."""
    if r.get("compare") == "equal":
        return r["latest"] != r["current"]
    return is_newer(r["latest"], r["current"])


def short(v: str) -> str:
    return v[:12] if re.fullmatch(r"[0-9a-f]{40,64}", v or "") else v


def title_of(r: dict) -> str:
    if r.get("document"):
        return f"Upstream document changed: {r['name']} {short(r['current'])} -> {short(r['latest'])}"
    return f"Update {r['name']} {r['current']} -> {r['latest']}"


def marker(state: dict) -> str:
    return f"<!-- {MARKER}: {json.dumps(state, sort_keys=True)} -->"


def read_marker(body: str) -> dict | None:
    m = MARKER_RE.search(body or "")
    if not m:
        return None
    try:
        d = json.loads(m.group(1))
    except json.JSONDecodeError:
        return None
    return d if isinstance(d, dict) else None


class GhIssues:
    """Issue operations through the gh CLI (GH_TOKEN and GH_REPO from the environment)."""

    def __init__(self, dry_run: bool):
        self.dry_run = dry_run

    def _gh(self, *args: str, stdin: str | None = None) -> str:
        if self.dry_run and args[1] != "list":
            print("DRY RUN: gh " + " ".join(args) + (f"\n{stdin}" if stdin else ""))
            return ""
        return subprocess.run(["gh", *args], input=stdin, check=True, capture_output=True, text=True).stdout

    def list(self, state: str) -> list[dict]:
        out = self._gh("issue", "list", "--label", "dependencies", "--state", state, "--limit", str(LIST_LIMIT),
                       "--json", "number,title,body,state")
        return json.loads(out or "[]")

    def create(self, title: str, body: str, labels: tuple[str, ...]) -> None:
        args = ["issue", "create", "--title", title, "--body-file", "-"]
        for label in labels:
            args += ["--label", label]
        self._gh(*args, stdin=body)

    def update(self, number: int, title: str, body: str, comment: str) -> None:
        self._gh("issue", "edit", str(number), "--title", title, "--body-file", "-", stdin=body)
        self._gh("issue", "comment", str(number), "--body-file", "-", stdin=comment)

    def edit_body(self, number: int, body: str) -> None:
        self._gh("issue", "edit", str(number), "--body-file", "-", stdin=body)

    def close(self, number: int, comment: str, body: str | None = None) -> None:
        if body is not None:
            self._gh("issue", "edit", str(number), "--body-file", "-", stdin=body)
        self._gh("issue", "close", str(number), "--comment", comment)


def update_body(r: dict) -> str:
    if r.get("document"):
        return document_body(r)
    if r.get("system"):
        return system_body(r)
    return (f"{marker({'kind': 'update', 'key': r['key'], 'version': r['latest'], 'current': r['current']})}\n"
            f"`{r['name']}` has a newer upstream version.\n\n"
            f"| Field | Value |\n|---|---|\n| In this repository | `{r['current']}` |\n| Upstream | `{r['latest']}` |\n"
            f"| Pinned in | `{r['where']}` |\n| Source | {r['source']} |\n\n"
            "Bump the pin where it is declared (for a vendored SDK, also its directory and the "
            "`--pin` digest, see docs/development.md \"Dependency graph\"), check the upstream "
            "changelog, and let CI run. This issue closes by itself once the repository is at "
            "that version.\n\nOpened by `.github/workflows/dependency-updates.yml`.\n")


def system_body(r: dict) -> str:
    return (f"{marker({'kind': 'update', 'key': r['key'], 'version': r['latest'], 'current': r['current']})}\n"
            f"Upstream `{r['name']}` is ahead of the version this project builds against.\n\n"
            f"| Field | Value |\n|---|---|\n| Recorded baseline (Debian trixie `{r['debian']}`) | `{r['current']}` |\n"
            f"| Upstream | `{r['latest']}` |\n| Source | {r['source']} |\n\n"
            "The build takes this library from Debian (`debian/control`), so there is no pin to bump. Review "
            "what the new release adds or fixes for the driver (for LibRaw: new camera raw formats), and set "
            "the `baseline` in `scripts/dependency_sdk_versions.json` to the upstream version (without a `v` "
            "prefix, as Debian spells it) once reviewed, or when Debian ships it. This issue closes by itself once the baseline matches.\n\n"
            "Opened by `.github/workflows/dependency-updates.yml`.\n")


CHANGELOG_MAX = 3000  # characters of upstream release notes quoted in an issue
FENCE = "`" * 3


def changelog_for(r: dict, get: HttpGet) -> str:
    """A "## Changelog" section for an update or document issue, or "" when there is none.

    Upstream text goes in one code block, its own fences neutralised, so an @mention or #123 in
    release notes cannot ping anyone or link to an issue here.
    """
    kind, link, text = r["check"][0], r.get("changelog"), ""
    if kind == "github":
        repo = r["check"][1]
        try:
            rel = json.loads(get(f"https://api.github.com/repos/{repo}/releases/tags/{urllib.parse.quote(r['latest'])}"))
            link, text = rel.get("html_url") or link, (rel.get("body") or "").strip()
        except (FetchError, json.JSONDecodeError):
            # Upstream tags may carry a "v" prefix the recorded baseline lacks (libgphoto2: v2.5.34).
            base = r["current"]
            if r["latest"][:1] in "vV" and base[:1] not in "vV":
                base = r["latest"][0] + base
            link = link or f"https://github.com/{repo}/compare/{base}...{r['latest']}"
    elif kind == "github-file":
        link = f"https://github.com/{r['check'][1]}/compare/{r['current']}...{r['latest']}"
    if r.get("changelog_json"):
        url, path, match, field = r["changelog_json"]
        try:
            link = latest_json_field(url, path, match, field, get) or link
        except (FetchError, KeyError, IndexError, TypeError, ValueError):
            pass
    if not link and not text:
        return ""
    out = "## Changelog\n\n" + (f"{link}\n" if link else "")
    if text:
        if len(text) > CHANGELOG_MAX:
            text = text[:CHANGELOG_MAX] + "\n[cut; see the link above]"
        out += f"\n{FENCE}text\n" + text.replace(FENCE, "` ` `") + f"\n{FENCE}\n"
    return out


def document_body(r: dict) -> str:
    local = f"| Local copy | `{r['path']}` |\n" if r.get("path") else ""
    # Documents reuse kind "update"; their keys carry a "doc:" prefix (targets()), which is what
    # keeps them apart from SDK and purl keys in reconcile().
    return (f"{marker({'kind': 'update', 'key': r['key'], 'version': r['latest'], 'current': r['current']})}\n"
            f"An upstream document a driver is built from has changed: `{r['name']}`.\n\n"
            f"| Field | Value |\n|---|---|\n| Recorded baseline | `{r['current']}` |\n| Upstream now | `{r['latest']}` |\n"
            f"{local}| Source | {r['source']} |\n\n"
            "Review what changed upstream against the driver (and refresh the local copy if there is one), "
            "then set this document's `baseline` in `scripts/dependency_sdk_versions.json` to the upstream "
            "value above. This issue closes by itself once the baseline matches.\n\n"
            "Opened by `.github/workflows/dependency-updates.yml`.\n")


def broken_body(r: dict) -> str:
    return (f"{marker({'kind': 'broken', 'key': r['key']})}\n"
            f"The weekly update check could not read the upstream version of `{r['name']}`:\n\n"
            f"```\n{r['error']}\n```\n\nSource: {r['source']}\n\n"
            "Until this is fixed a new release of it goes unreported. If the page or repository "
            "moved, update the source in `scripts/dependency_sdk_versions.json` or the pin it "
            "comes from. This issue closes by itself when the source reads again.\n")


def resolved_body(issue: dict, state: dict) -> str:
    """The issue body with its marker flagged ``resolved``: closed by this script, not by hand."""
    return MARKER_RE.sub(lambda _: marker({**state, "resolved": True}), issue.get("body", ""), count=1)


def reconcile(results: list[dict], issues, all_issues: list[dict], notes=None) -> list[str]:
    """Open, retitle and close issues so they match results. Returns one line per action;
    a line starting with ``FAILED`` is an issue call that failed (the others still run).

    all_issues holds open and closed issues. A closed issue whose marker is not ``resolved``
    was closed by a maintainer. For an update issue that means "won't bump": its (current,
    latest) pair is skipped until upstream releases a newer version or the repository moves
    (a "Closes #N" PR that bumped only part of the way). For a broken-source issue it means
    "known": no new copy opens while the source stays unreadable, and the hand-close is
    cleared once the source reads again. An open issue for a dependency no longer in results
    closes.
    """
    by, declined, declined_broken = {}, set(), {}
    for i in all_issues:
        st = read_marker(i.get("body", ""))
        if not (st and st.get("kind") in ("update", "broken") and "key" in st):
            continue
        if i.get("state", "OPEN").upper() == "OPEN":
            by[(st["kind"], st["key"])] = (i, st)
        elif st.get("resolved"):
            continue
        elif st["kind"] == "update":
            declined.add((st["key"], st.get("version"), st.get("current")))
        else:
            declined_broken[st["key"]] = (i, st)
    log = []
    for r in results:
        try:
            log += _reconcile_one(r, issues, by, declined, declined_broken, notes)
        except Exception as e:  # one failed gh call must not stop the other dependencies
            log.append(f"FAILED {r['name']}: {getattr(e, 'stderr', None) or e}")
    tracked = {r["key"] for r in results}
    for (kind, key), (issue, st) in sorted(by.items(), key=lambda kv: kv[1][0]["number"]):
        if key not in tracked:
            try:
                issues.close(issue["number"], f"`{key}` is no longer in the dependency inventory.",
                             resolved_body(issue, st))
                log.append(f"closed {kind} issue #{issue['number']} (no longer tracked)")
            except Exception as e:
                log.append(f"FAILED closing #{issue['number']}: {getattr(e, 'stderr', None) or e}")
    return log


def _with_notes(r: dict, notes) -> str:
    """The issue body plus its changelog section; a failed changelog lookup only drops the section."""
    section = ""
    if notes is not None:
        try:
            section = notes(r) or ""
        except Exception as e:  # the changelog is a convenience: never let it stop the issue
            print(f"warning: no changelog for {r.get('name')}: {type(e).__name__}: {e}", file=sys.stderr)
            section = ""
    return update_body(r) + (("\n" + section) if section else "")


def _reconcile_one(r: dict, issues, by: dict, declined: set, declined_broken: dict, notes=None) -> list[str]:
    log = []
    broken = by.get(("broken", r["key"]))
    if r["error"]:
        if not broken and r["key"] not in declined_broken:
            issues.create(f"Update check broken: {r['name']}", broken_body(r), BROKEN_LABELS)
            log.append(f"opened broken-source issue for {r['name']}")
        return log
    if broken:
        issues.close(broken[0]["number"], f"The source reads again (`{r['latest']}`).", resolved_body(*broken))
        log.append(f"closed broken-source issue #{broken[0]['number']}")
    if r["key"] in declined_broken:
        issues.edit_body(declined_broken[r["key"]][0]["number"], resolved_body(*declined_broken[r["key"]]))
        log.append(f"cleared the hand-close of broken-source issue #{declined_broken[r['key']][0]['number']}")
    upd = by.get(("update", r["key"]))
    newer = differs(r)
    title = title_of(r)
    if newer and not upd:
        if (r["key"], r["latest"], r["current"]) not in declined:
            issues.create(title, _with_notes(r, notes), DOC_LABELS if r.get("document") else UPDATE_LABELS)
            log.append(f"opened update issue for {r['name']} {r['latest']}")
    elif newer and upd is not None and (upd[1].get("version"), upd[1].get("current")) != (r["latest"], r["current"]):
        note = (f"Upstream is now at `{r['latest']}`." if upd[1].get("version") != r["latest"]
                else f"The repository is now at `{r['current']}`, still behind `{r['latest']}`.")
        issues.update(upd[0]["number"], title, _with_notes(r, notes), note)
        log.append(f"retitled #{upd[0]['number']} to {r['current']} -> {r['latest']}")
    elif not newer and upd is not None:
        if r.get("document"):
            note = f"The recorded baseline now matches upstream (`{short(r['current'])}`)."
        else:
            note = f"The repository is now at `{r['current']}`; upstream latest is `{r['latest']}`."
        issues.close(upd[0]["number"], note, resolved_body(*upd))
        log.append(f"closed update issue #{upd[0]['number']}")
    return log


OSV_RELEASE = "Debian:13"  # trixie, the release the .deb is built for and the scan image runs


def osv_fix(vulns: dict, ids: list[str], ecosystem: str, name: str) -> str | None:
    """The version that fixes any of ``ids`` in this package's release, or None when there is no
    fix there or Debian rates the issue "unimportant".

    A bare "Debian" (or missing) ecosystem means OSV_RELEASE. Debian advisories name the source
    package; when none of an advisory's entries for the release carries the report's name (a
    binary name such as libc6), its entries for that release are used.
    """
    release = ecosystem if ":" in (ecosystem or "") else OSV_RELEASE
    for vid in ids:
        entries = [a for a in (vulns.get(vid) or {}).get("affected") or []
                   if (a.get("package") or {}).get("ecosystem") == release]
        named = [a for a in entries if (a.get("package") or {}).get("name") == name] or entries
        for a in named:
            if (a.get("ecosystem_specific") or {}).get("urgency") == "unimportant":
                continue
            for r in a.get("ranges") or []:
                for e in r.get("events") or []:
                    if "fixed" in e:
                        return e["fixed"]
    return None


def direct_sources(depends: str, sources: str) -> set[str]:
    """Debian source packages of the .deb's own runtime dependencies, without the C/C++ runtime.

    depends: the .deb's Depends field (dpkg-deb -f). sources: "binary source" lines from
    dpkg-query -W -f='${Package} ${source:Package}' in the scan image. A dependency with no
    line keeps its own name.
    """
    to_source = {}
    for line in sources.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            to_source[parts[0].split(":")[0]] = parts[1]
    names = set()
    for group in depends.replace("\n", " ").split(","):
        for alt in group.split("|"):  # every alternative: the image may have installed any of them
            m = re.match(r"\s*([a-z0-9][a-z0-9+.\-]*)", alt)
            if m:
                names.add(m.group(1))
    found = {to_source.get(name, name) for name in names}
    return {src for src in found if not is_toolchain_runtime(src)}


def is_toolchain_runtime(source: str) -> bool:
    """glibc and gcc-N (libc6, libstdc++6, libgcc-s1): the C/C++ runtime every program links,
    which no application can replace or pin, so its advisories are never actionable here."""
    return source == "glibc" or re.fullmatch(r"gcc-\d+", source) is not None


def osv_unimportant(report: dict, vid: str, name: str) -> bool:
    """True when the report's record of ``vid`` has Debian rate it "unimportant" for ``name``.

    The first record of the ID is enough: a Debian advisory (DEBIAN-CVE-*, DSA, DLA) is the same
    record wherever the report repeats it, with one affected entry per source package and release.
    """
    for res in report.get("results") or []:
        for p in res.get("packages") or []:
            for v in p.get("vulnerabilities") or []:
                if v.get("id") != vid:
                    continue
                entries = [a for a in v.get("affected") or []
                           if (a.get("package") or {}).get("ecosystem") == OSV_RELEASE]
                named = [a for a in entries if (a.get("package") or {}).get("name") == name] or entries
                return bool(named) and all((a.get("ecosystem_specific") or {}).get("urgency") == "unimportant"
                                           for a in named)
    return False


SARIF_PACKAGE_RE = re.compile(r"Package '([^@']+)@[^']*' is vulnerable to '([^']+)'")


def filter_sarif(sarif: dict, report: dict, direct: set[str]) -> dict:
    """The SARIF with only findings in ``direct`` source packages that Debian does not rate
    unimportant: what code scanning shows. Rules stay as they are (results refer to them by index).

    Each kept result's fingerprint becomes a hash of its source package and advisory alone.
    osv-scanner's own includes the package version, so every Debian security update to a package
    (8.14.1-2+deb13u5 -> +deb13u7) closed its alerts and opened them again, dismissals lost."""
    out = json.loads(json.dumps(sarif))
    for run in out.get("runs") or []:
        kept = []
        for r in run.get("results") or []:
            m = SARIF_PACKAGE_RE.search((r.get("message") or {}).get("text", ""))
            if m and m.group(1) in direct and not osv_unimportant(report, m.group(2), m.group(1)):
                key = f"{m.group(1)}|{m.group(2)}".encode()
                r["partialFingerprints"] = {"primaryLocationLineHash": hashlib.sha256(key).hexdigest()}
                kept.append(r)
        run["results"] = kept
    return out


def osv_scope_unreadable(report: dict, sources: str) -> bool:
    """True when the JSON names a package by its binary name (libcurl4t64) rather than its Debian
    source (curl): the direct-dependency scope would then match nothing and open no issue, silently.
    sources: the same "binary source" lines direct_sources() reads."""
    binary_only = set()
    source_names = set()
    for line in sources.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            binary, source = parts[0].split(":")[0], parts[1]
            source_names.add(source)
            if binary != source:
                binary_only.add(binary)
    binary_only -= source_names
    named = {(p.get("package") or {}).get("name") for res in report.get("results") or []
             for p in res.get("packages") or []}
    return bool(named & binary_only)


def sarif_unreadable(sarif: dict) -> tuple[int, int]:
    """(results, results whose message SARIF_PACKAGE_RE cannot read)."""
    results = [r for run in sarif.get("runs") or [] for r in run.get("results") or []]
    return len(results), sum(1 for r in results if not SARIF_PACKAGE_RE.search((r.get("message") or {}).get("text", "")))


def osv_findings(report: dict, direct: set[str] | None = None) -> list[dict]:
    """Actionable High/Critical findings from osv-scanner --format json: max_severity >=
    OSV_THRESHOLD and a fixed version in the package's own release. One finding per advisory
    group and source package, however many binary packages of that source report it."""
    found: list[dict] = []
    for res in report.get("results") or []:
        for p in res.get("packages") or []:
            pkg = p.get("package", {})
            if direct is not None and pkg.get("name") not in direct:
                continue  # the base system, not one of the .deb's own dependencies
            vulns = {v.get("id"): v for v in p.get("vulnerabilities") or []}
            for g in p.get("groups") or []:
                try:
                    sev = float(g.get("max_severity") or "")
                except ValueError:
                    continue
                ids = sorted(g.get("ids") or [])
                if sev < OSV_THRESHOLD or not ids:
                    continue
                fixed = osv_fix(vulns, ids, pkg.get("ecosystem", ""), pkg.get("name", ""))
                if fixed is None:
                    continue
                same = next((f for f in found if f["package"] == pkg.get("name", "?") and set(f["ids"]) & set(ids)), None)
                if same:
                    same["ids"] = sorted(set(same["ids"]) | set(ids))
                    continue
                found.append({"id": ids[0], "ids": ids, "severity": sev, "package": pkg.get("name", "?"),
                              "version": pkg.get("version", "?"), "fixed": fixed})
    return sorted(found, key=lambda f: (-f["severity"], f["id"]))


def osv_summary(report: dict, issues, all_issues: list[dict], repo: str, direct: set[str] | None = None) -> list[str]:
    reported = set()
    for i in all_issues:
        st = read_marker(i.get("body", ""))
        if st and st.get("kind") == OSV_MARKER:
            reported.update(st.get("ids", []))
    found = [f for f in osv_findings(report, direct) if not reported.intersection(f["ids"])]
    if not found:
        return []
    # Keep the body under GitHub's 65536-character limit: list at most OSV_MAX_ROWS; the rest
    # stay unreported and open the next run's issue.
    new, rest = found[:OSV_MAX_ROWS], len(found) - OSV_MAX_ROWS
    rows = "\n".join(f"| {f['severity']:.1f} | `{f['package']}` {f['version']} | `{f['fixed']}` | {', '.join(f['ids'])} |"
                     for f in new)
    body = (f"{marker({'kind': OSV_MARKER, 'ids': sorted({i for f in new for i in f['ids']})})}\n"
            f"The OSV scan of the nightly `.deb`'s installed runtime packages (Debian trixie) found "
            f"{len(new)} new High or Critical finding(s) that Debian has already fixed in trixie:\n\n"
            f"| CVSS | Package | Fixed in | Advisories |\n|---|---|---|---|\n"
            f"{rows}\n\n"
            + (f"{rest} more are not listed here; the next scan opens an issue for them.\n\n" if rest > 0 else "")
            + f"Findings below High or with no trixie fix yet are listed only under "
            f"https://github.com/{repo}/security/code-scanning (tool `osv-scanner`). Base-system packages, the "
            "C/C++ runtime and findings Debian rates unimportant are left out of both; they are only in the "
            "scan run's `osv-full-results` artifact. A fixed version reaches users "
            "through `apt upgrade`; act here when AlpacaBridge should require it (a higher minimum version in "
            "`Depends`) or when a build or test image still carries the affected version.\n\n"
            "Opened by `.github/workflows/osv-scan.yml`.\n")
    issues.create(f"Vulnerabilities: {len(new)} fixable High/Critical in the nightly .deb's packages", body, OSV_LABELS)
    return [f"opened OSV summary issue for {len(new)} finding(s)"]


def self_test() -> int:
    assert is_newer("3.12.0", "v3.11.3") and not is_newer("v3.11.3", "3.11.3")
    assert is_newer("4.5.1", "4.5.1-beta") and not is_newer("v4.5.0", "4.5.1-beta")
    assert not is_newer("2.3.0", "2.3") and is_newer("2.3.1", "2.3")
    assert highest(["v1.2", "v1.10", "v2.0-rc1", "nightly"]) == "v1.10"

    pages = {
        "https://api.github.com/repos/a/b/releases/latest": json.dumps({"tag_name": "v2.0.0"}),
        "https://api.github.com/repos/c/d/tags?per_page=100": json.dumps([{"name": "1.9"}, {"name": "1.10"}]),
        "https://k.org/gpiod/": "libgpiod-2.2.4.tar.xz libgpiod-2.3.1.tar.xz libgpiod-2.3.tar.xz",
    }

    def get(url: str) -> str:
        if url not in pages:
            raise FetchError(f"HTTP 404 from {url}")
        return pages[url]

    assert latest_github("a/b", get) == "v2.0.0"
    assert latest_github("c/d", get) == "1.10", "no releases: highest version tag"
    # /tags is not sorted by version: a later page can hold the highest one.
    pages["https://api.github.com/repos/g/h/tags?per_page=100"] = json.dumps([{"name": f"0.{i}"} for i in range(100)])
    pages["https://api.github.com/repos/g/h/tags?per_page=100&page=2"] = json.dumps([{"name": "3.0"}])
    assert latest_github("g/h", get) == "3.0", "tags on page 2 are read"
    # Every page full: the cap is reported, not silent.
    for page in range(1, TAG_PAGES + 1):
        pages[f"https://api.github.com/repos/m/n/tags?per_page=100" + (f"&page={page}" if page > 1 else "")] = \
            json.dumps([{"name": f"{page}.{i}"} for i in range(100)])
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        assert latest_github("m/n", get) == f"{TAG_PAGES}.99"
    assert "first 1000 tags" in err.getvalue(), err.getvalue()
    assert latest_page("https://k.org/gpiod/", r"libgpiod-(\d[\d.]*\d)\.tar\.xz", get) == "2.3.1"

    snap = {"manifests": {"ci.yml": {"resolved": {"pkg:github/a/b@v1.0.0": {}, "pkg:deb/ubuntu/x?arch=arm64": {}}},
                          "doc.md": {"resolved": {"pkg:github/c/d@1.10": {}}},
                          "gone.yml": {"resolved": {"pkg:github/e/f@1.0": {}}}}}
    table = {"sdks": [{"name": "libgpiod", "version": "2.2.4",
                       "updates": {"provider": "release-page", "url": "https://k.org/gpiod/",
                                   "pattern": r"libgpiod-(\d[\d.]*\d)\.tar\.xz"}},
                      {"name": "vendor-sdk", "version": "1"}]}
    ts, manual = targets(snap, table)
    assert [t["key"] for t in ts] == ["a/b", "c/d", "e/f", "libgpiod"] and manual == ["vendor-sdk"], (ts, manual)
    results = [check(t, get) for t in ts]
    res = {r["key"]: r for r in results}
    assert res["a/b"]["latest"] == "v2.0.0" and res["c/d"]["latest"] == "1.10" and res["libgpiod"]["latest"] == "2.3.1"
    assert res["e/f"]["error"] and "404" in res["e/f"]["error"], res["e/f"]

    class Fake:
        def __init__(self):
            self.issues: list[dict] = []
            self.calls: list[tuple] = []

        def create(self, title, body, labels):
            self.issues.append({"number": len(self.issues) + 1, "title": title, "body": body,
                                "state": "OPEN", "labels": labels})
            self.calls.append(("create", title))

        def update(self, number, title, body, comment):
            self.issues[number - 1].update(title=title, body=body)
            self.calls.append(("update", number, title))

        def edit_body(self, number, body):
            self.issues[number - 1]["body"] = body
            self.calls.append(("edit_body", number))

        def close(self, number, comment, body=None):
            self.issues[number - 1]["state"] = "CLOSED"
            self.issues[number - 1]["closing_comment"] = comment
            if body is not None:
                self.issues[number - 1]["body"] = body
            self.calls.append(("close", number))

        def open(self):
            return [i for i in self.issues if i["state"] == "OPEN"]

    f = Fake()
    reconcile(results, f, f.issues)
    titles = sorted(i["title"] for i in f.issues)
    assert titles == ["Update a/b v1.0.0 -> v2.0.0", "Update check broken: e/f",
                      "Update libgpiod 2.2.4 -> 2.3.1"], titles
    assert {i["title"]: i["labels"] for i in f.issues}["Update check broken: e/f"] == BROKEN_LABELS
    assert {i["title"]: i["labels"] for i in f.issues}["Update a/b v1.0.0 -> v2.0.0"] == UPDATE_LABELS
    # Same results again: nothing to do (no duplicate issues).
    f.calls.clear()
    assert reconcile(results, f, f.issues) == [] and f.calls == []
    # A still newer upstream retitles the open issue instead of opening a second one.
    pages["https://api.github.com/repos/a/b/releases/latest"] = json.dumps({"tag_name": "v2.1.0"})
    pages["https://api.github.com/repos/e/f/releases/latest"] = json.dumps({"tag_name": "1.0"})
    results = [check(t, get) for t in ts]
    f.calls.clear()
    reconcile(results, f, f.issues)
    assert ("update", 1, "Update a/b v1.0.0 -> v2.1.0") in f.calls, f.calls
    assert ("close", 2) in f.calls, "the broken source reads again: its issue closes"
    assert not any(c[0] == "create" for c in f.calls), f.calls
    # The repository bumps to an intermediate release still behind upstream: the title follows.
    snap["manifests"]["ci.yml"]["resolved"] = {"pkg:github/a/b@v2.0.0": {}}
    ts, _ = targets(snap, table)
    f.calls.clear()
    reconcile([check(t, get) for t in ts], f, f.issues)
    assert ("update", 1, "Update a/b v2.0.0 -> v2.1.0") in f.calls, f.calls
    # The repository catches up: the update issue closes.
    snap["manifests"]["ci.yml"]["resolved"] = {"pkg:github/a/b@v2.1.0": {}}
    ts, _ = targets(snap, table)
    f.calls.clear()
    reconcile([check(t, get) for t in ts], f, f.issues)
    assert ("close", 1) in f.calls, f.calls
    # A maintainer closes an update issue by hand ("won't bump"): the same version stays skipped,
    # and only a newer upstream release opens a new issue.
    snap["manifests"]["ci.yml"]["resolved"] = {"pkg:github/a/b@v1.0.0": {}}
    ts, _ = targets(snap, table)
    reconcile([check(t, get) for t in ts], f, f.issues)
    reopened = [i for i in f.issues if i["state"] == "OPEN" and i["title"].startswith("Update a/b")]
    assert len(reopened) == 1, reopened
    reopened[0]["state"] = "CLOSED"
    f.calls.clear()
    reconcile([check(t, get) for t in ts], f, f.issues)
    assert f.calls == [], "a hand-closed issue for this version is not reopened"
    # A "Closes #N" PR that bumps only part of the way also closes the issue without the
    # resolved flag; the repository moved, so the remaining gap is reported again.
    snap["manifests"]["ci.yml"]["resolved"] = {"pkg:github/a/b@v1.5.0": {}}
    ts, _ = targets(snap, table)
    reconcile([check(t, get) for t in ts], f, f.issues)
    assert ("create", "Update a/b v1.5.0 -> v2.1.0") in f.calls, f.calls
    [i for i in f.issues if i["title"] == "Update a/b v1.5.0 -> v2.1.0"][0]["state"] = "CLOSED"
    snap["manifests"]["ci.yml"]["resolved"] = {"pkg:github/a/b@v1.0.0": {}}
    ts, _ = targets(snap, table)
    pages["https://api.github.com/repos/a/b/releases/latest"] = json.dumps({"tag_name": "v2.2.0"})
    reconcile([check(t, get) for t in ts], f, f.issues)
    assert ("create", "Update a/b v1.0.0 -> v2.2.0") in f.calls, f.calls
    # A pin that is not a version (a commit SHA) is a broken entry, not a crash of the whole run.
    snap["manifests"]["ci.yml"]["resolved"] = {"pkg:github/a/b@deadbeef": {}}
    ts, _ = targets(snap, table)
    res = {r["key"]: r for r in (check(t, get) for t in ts)}
    assert res["a/b"]["error"] and "deadbeef" in res["a/b"]["error"], res["a/b"]
    f.calls.clear()
    reconcile(list(res.values()), f, f.issues)
    assert ("create", "Update check broken: a/b") in f.calls, f.calls
    # A dependency that leaves the inventory: its open update and broken issues close.
    snap["manifests"]["ci.yml"]["resolved"] = {}
    ts, _ = targets(snap, table)
    f.calls.clear()
    reconcile([check(t, get) for t in ts], f, f.issues)
    assert not [i for i in f.issues if i["state"] == "OPEN" and "a/b" in i["title"]], \
        [i["title"] for i in f.issues if i["state"] == "OPEN"]
    # A body someone edited into garbage is not mistaken for a tracked issue.
    assert read_marker("<!-- dependency-updates: {not json} -->") is None and read_marker("") is None

    def vuln(vid, fixed_in=None, urgency="low", name="glibc"):
        """An OSV record shaped like Debian's: one affected entry per release, fixed or not."""
        affected = []
        for eco in ("Debian:12", "Debian:13", "Debian:14"):
            events = [{"introduced": "0"}] + ([{"fixed": fixed_in[eco]}] if fixed_in and eco in fixed_in else [])
            affected.append({"package": {"ecosystem": eco, "name": name}, "ranges": [{"type": "ECOSYSTEM", "events": events}],
                             "ecosystem_specific": {"urgency": urgency}})
        return {"id": vid, "affected": affected}

    def pkg(name, groups, vulns, binary=None):
        return {"package": {"name": name, "version": "2.41-12", "ecosystem": "Debian:13", **({"binary": binary} if binary else {})},
                "groups": groups, "vulnerabilities": vulns}

    fixed13 = {"Debian:13": "2.41-12+deb13u5"}
    report = {"results": [{"packages": [
        # The same advisory reported through two binary packages of one source: one finding.
        pkg("glibc", [{"ids": ["CVE-2026-1", "DEBIAN-CVE-2026-1"], "max_severity": "9.8"}],
            [vuln("DEBIAN-CVE-2026-1", fixed13)], binary="libc6"),
        pkg("glibc", [{"ids": ["CVE-2026-1", "DEBIAN-CVE-2026-1"], "max_severity": "9.8"}],
            [vuln("DEBIAN-CVE-2026-1", fixed13)], binary="libc-bin"),
        pkg("glibc", [{"ids": ["DEBIAN-CVE-2019-1010022"], "max_severity": "9.8"},  # no fix anywhere, unimportant
                      {"ids": ["DEBIAN-CVE-2026-97399"], "max_severity": "8.0"},    # fixed only in Debian 14
                      {"ids": ["DEBIAN-CVE-2026-6"], "max_severity": "9.0"},        # fixed in trixie but unimportant
                      {"ids": ["DEBIAN-CVE-2026-2"], "max_severity": "5.0"},        # fixed, below High
                      {"ids": ["DEBIAN-CVE-2026-3"], "max_severity": ""}],          # fixed, unscored
            [vuln("DEBIAN-CVE-2019-1010022", None, "unimportant"), vuln("DEBIAN-CVE-2026-97399", {"Debian:14": "2.43-7"}),
             vuln("DEBIAN-CVE-2026-6", fixed13, "unimportant"), vuln("DEBIAN-CVE-2026-2", fixed13),
             vuln("DEBIAN-CVE-2026-3", fixed13)]),
        pkg("curl", [{"ids": ["DEBIAN-CVE-2026-4"], "max_severity": "7.0"}],
            [vuln("DEBIAN-CVE-2026-4", {"Debian:13": "8.14.1-2+deb13u2"}, name="curl")])]}]}
    found = osv_findings(report)
    assert [x["id"] for x in found] == ["CVE-2026-1", "DEBIAN-CVE-2026-4"], found
    assert found[0]["fixed"] == "2.41-12+deb13u5" and found[1]["fixed"] == "8.14.1-2+deb13u2", found
    f = Fake()
    assert osv_summary(report, f, f.issues, "o/r") and len(f.issues) == 1 and f.issues[0]["labels"] == OSV_LABELS
    body = f.issues[0]["body"]
    assert body.count("| 9.8 |") == 1, "one row per advisory, not per binary package"
    for absent in ("DEBIAN-CVE-2019-1010022", "DEBIAN-CVE-2026-97399", "DEBIAN-CVE-2026-6", "DEBIAN-CVE-2026-2",
                   "DEBIAN-CVE-2026-3"):
        assert absent not in body, absent
    assert "2.41-12+deb13u5" in body, "the row names the fixed version"
    f.issues[0]["state"] = "CLOSED"
    assert osv_summary(report, f, f.issues, "o/r") == [], "findings already reported, even in a closed issue"
    # OSV adds an alias that sorts first: still the same, already reported finding.
    report["results"][0]["packages"][0]["groups"][0]["ids"].append("AAA-2026-1")
    assert osv_summary(report, f, f.issues, "o/r") == [], "a reported group matched on any of its ids"
    report["results"][0]["packages"][3]["groups"].append({"ids": ["DEBIAN-CVE-2026-5"], "max_severity": "8.1"})
    report["results"][0]["packages"][3]["vulnerabilities"].append(
        vuln("DEBIAN-CVE-2026-5", {"Debian:13": "8.14.1-2+deb13u3"}, name="curl"))
    assert osv_summary(report, f, f.issues, "o/r") and "DEBIAN-CVE-2026-5" in f.issues[1]["body"]
    assert "DEBIAN-CVE-2026-4" not in f.issues[1]["body"], "only the new finding is listed"
    # The report's own spelling may differ from the advisory's: a bare "Debian" ecosystem, or a binary
    # package name where Debian advisories name the source package. Neither drops the finding.
    odd = {"results": [{"packages": [
        {"package": {"name": "libc6", "version": "2.41-12", "ecosystem": "Debian"},
         "groups": [{"ids": ["DEBIAN-CVE-2026-7"], "max_severity": "9.1"}],
         "vulnerabilities": [vuln("DEBIAN-CVE-2026-7", fixed13)]}]}]}
    assert [x["id"] for x in osv_findings(odd)] == ["DEBIAN-CVE-2026-7"], osv_findings(odd)
    # The same CVE in two different source packages (a library and a copy bundled elsewhere) stays
    # two findings, each with its own fixed version.
    two = {"results": [{"packages": [
        pkg("curl", [{"ids": ["CVE-2026-8"], "max_severity": "8.0"}], [{**vuln("CVE-2026-8", {"Debian:13": "1-1"}, name="curl")}]),
        pkg("libcurl-bundler", [{"ids": ["CVE-2026-8"], "max_severity": "8.0"}],
            [vuln("CVE-2026-8", {"Debian:13": "9-9"}, name="libcurl-bundler")])]}]}
    assert sorted((x["package"], x["fixed"]) for x in osv_findings(two)) == [("curl", "1-1"), ("libcurl-bundler", "9-9")]
    # Only AlpacaBridge's own runtime dependencies count: the .deb's Depends, mapped to their Debian
    # source packages (dpkg-query in the scan image); the base system's packages are dropped.
    depends = "libc6 (>= 2.38), libcurl4t64 (>= 7.16.2), libgphoto2-6t64 (>= 2.5.10), libhidapi-hidraw0, adduser"
    sources_txt = "libc6 glibc\nlibcurl4t64 curl\nlibgphoto2-6t64 libgphoto2\nlibhidapi-hidraw0 hidapi\nutil-linux util-linux\n"
    direct = direct_sources(depends + ", libstdc++6 (>= 14), libgcc-s1", sources_txt + "libstdc++6 gcc-14\nlibgcc-s1 gcc-14\n")
    assert direct == {"curl", "libgphoto2", "hidapi", "adduser"}, \
        "the C/C++ runtime (glibc, gcc-N) is excluded: no application can act on it"
    scoped = {"results": [{"packages": [
        pkg("util-linux", [{"ids": ["DEBIAN-CVE-2026-9"], "max_severity": "9.0"}],
            [vuln("DEBIAN-CVE-2026-9", {"Debian:13": "2.41-5+deb13u1"}, name="util-linux")]),
        pkg("curl", [{"ids": ["DEBIAN-CVE-2026-10"], "max_severity": "9.0"}],
            [vuln("DEBIAN-CVE-2026-10", {"Debian:13": "8.14.1-2+deb13u2"}, name="curl")])]}]}
    assert [x["package"] for x in osv_findings(scoped, direct)] == ["curl"], "a base-system package opens no issue"
    assert len(osv_findings(scoped)) == 2, "without a direct set every package counts (unchanged default)"
    # The code-scanning upload is filtered the same way, and drops what Debian rates unimportant.
    def result(pkg_name, vid):
        return {"ruleId": vid, "message": {"text": f"Package '{pkg_name}@1.0' is vulnerable to '{vid}'."}}
    sarif = {"runs": [{"tool": {"driver": {"name": "osv-scanner", "rules": [
        {"id": v} for v in ("DEBIAN-CVE-2026-9", "DEBIAN-CVE-2026-10", "DEBIAN-CVE-2019-1010022", "DEBIAN-CVE-2026-11",
                            "DEBIAN-CVE-2026-12")]}},
        "results": [result("util-linux", "DEBIAN-CVE-2026-9"), result("curl", "DEBIAN-CVE-2026-10"),
                    result("glibc", "DEBIAN-CVE-2019-1010022"), result("libgphoto2", "DEBIAN-CVE-2026-11"),
                    result("curl", "DEBIAN-CVE-2026-12")]}]}
    sarif_report = {"results": [{"packages": [
        pkg("glibc", [{"ids": ["DEBIAN-CVE-2019-1010022"], "max_severity": "9.8"}],
            [vuln("DEBIAN-CVE-2019-1010022", None, "unimportant")]),
        pkg("libgphoto2", [{"ids": ["DEBIAN-CVE-2026-11"], "max_severity": "8.8"}],
            [vuln("DEBIAN-CVE-2026-11", None, "not yet assigned", name="libgphoto2")]),
        # Unimportant in one of the .deb's own dependencies: still dropped from code scanning.
        pkg("curl", [{"ids": ["DEBIAN-CVE-2026-12"], "max_severity": "5.3"}],
            [vuln("DEBIAN-CVE-2026-12", None, "unimportant", name="curl")])]}]}
    kept = filter_sarif(sarif, sarif_report, direct)
    texts = [r["message"]["text"] for r in kept["runs"][0]["results"]]
    assert texts == ["Package 'curl@1.0' is vulnerable to 'DEBIAN-CVE-2026-10'.",
                     "Package 'libgphoto2@1.0' is vulnerable to 'DEBIAN-CVE-2026-11'."], texts
    assert len(kept["runs"][0]["tool"]["driver"]["rules"]) == 5, "rules stay: results refer to them by index"
    assert len(sarif["runs"][0]["results"]) == 5, "the input SARIF is not modified"
    # Code scanning matches an alert across scans by its fingerprint. osv-scanner's hashes the package
    # version, so a Debian security update (curl 8.14.1-2+deb13u5 -> +deb13u7) reopened every dismissed
    # alert as a new one; the fingerprint is the source package and advisory only.
    def fingerprint(version, pkg_name="curl", vid="DEBIAN-CVE-2026-10"):
        r = {"ruleId": vid, "partialFingerprints": {"primaryLocationLineHash": version},
             "message": {"text": f"Package '{pkg_name}@{version}' is vulnerable to '{vid}'."}}
        one = {"runs": [{"tool": {"driver": {"rules": [{"id": vid}]}}, "results": [r]}]}
        return filter_sarif(one, {"results": []}, {"curl", "libgphoto2"})["runs"][0]["results"][0]["partialFingerprints"]
    assert fingerprint("8.14.1-2+deb13u5") == fingerprint("8.14.1-2+deb13u7"), "one alert across Debian revisions"
    assert fingerprint("1.0") != fingerprint("1.0", vid="DEBIAN-CVE-2026-11"), "each advisory is its own alert"
    assert fingerprint("1.0") != fingerprint("1.0", pkg_name="libgphoto2"), "each source package is its own alert"
    # Every alternative of an "a | b" dependency counts: the image may have installed either.
    assert direct_sources("libcurl4t64 | libcurl3t64-gnutls, adduser",
                          "libcurl4t64 curl\nlibcurl3t64-gnutls curl-gnutls\n") == {"curl", "curl-gnutls", "adduser"}
    # The --filter-sarif CLI: scoped output written; and if osv-scanner's SARIF wording changes so no
    # result can be read, it fails loudly instead of uploading an empty, quiet code-scanning result.
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        (t / "deps").write_text(depends)
        (t / "src").write_text(sources_txt)
        (t / "report.json").write_text(json.dumps(sarif_report))
        (t / "in.sarif").write_text(json.dumps(sarif))
        args = ["--filter-sarif", str(t / "in.sarif"), "--report", str(t / "report.json"), "--output",
                str(t / "out.sarif"), "--direct-depends", str(t / "deps"), "--direct-sources", str(t / "src")]
        with contextlib.redirect_stdout(io.StringIO()):
            assert main(args) == 0
        assert len(json.loads((t / "out.sarif").read_text())["runs"][0]["results"]) == 2
        reworded = json.loads(json.dumps(sarif))
        for r in reworded["runs"][0]["results"]:
            r["message"]["text"] = r["message"]["text"].replace("is vulnerable to", "has advisory")
        (t / "in.sarif").write_text(json.dumps(reworded))
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            assert main(args) == 1
        assert "message format" in err.getvalue(), err.getvalue()
        try:
            with contextlib.redirect_stderr(io.StringIO()):
                main(["--filter-sarif", str(t / "in.sarif")])
        except SystemExit as e:
            assert e.code == 2, "--filter-sarif without --report/--output/--direct-* is a usage error"
        else:
            raise AssertionError("--filter-sarif alone must be refused")
    # The summary's scope relies on the JSON naming Debian source packages, as the SARIF does. If no
    # package that has findings is named like a direct dependency (binary names, say), fail loudly.
    # The check is on naming, not on findings: a scan where only base-system packages have findings
    # (direct dependencies clean) is a legitimate result, not an error.
    assert osv_scope_unreadable(scoped, sources_txt) is False
    base_only = {"results": [{"packages": [scoped["results"][0]["packages"][0]]}]}
    assert osv_scope_unreadable(base_only, sources_txt) is False, "clean direct dependencies are not a scope error"
    binary_named = {"results": [{"packages": [
        pkg("libcurl4t64", [{"ids": ["DEBIAN-CVE-2026-10"], "max_severity": "9.0"}],
            [vuln("DEBIAN-CVE-2026-10", {"Debian:13": "8.14.1-2+deb13u2"}, name="curl")])]}]}
    assert osv_scope_unreadable(binary_named, sources_txt) is True
    assert osv_scope_unreadable({"results": []}, sources_txt) is False
    with tempfile.TemporaryDirectory() as tmp:  # the --osv-summary CLI refuses before touching issues
        t = Path(tmp)
        (t / "deps").write_text(depends)
        (t / "src").write_text(sources_txt)
        (t / "osv.json").write_text(json.dumps(binary_named))
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            assert main(["--osv-summary", str(t / "osv.json"), "--direct-depends", str(t / "deps"),
                         "--direct-sources", str(t / "src")]) == 1
        assert "binary name" in err.getvalue(), err.getvalue()
    # A marker from the first summary format (kind "osv", #1135) does not suppress a finding.
    g = Fake()
    g.issues.append({"number": 1, "title": "old", "state": "CLOSED",
                     "body": marker({"kind": "osv", "ids": ["CVE-2026-1", "DEBIAN-CVE-2026-1"]})})
    assert osv_summary(report, g, g.issues, "o/r") and "CVE-2026-1" in g.issues[-1]["body"]
    # --- SDK and document providers ---
    assert decode_body(gzip.compress(b"page"), "gzip") == b"page" and decode_body(b"page", "") == b"page"
    # A connection that drops mid-body (http.client.IncompleteRead) is a broken source too, and
    # ordinary pages use a short timeout while a large document (the ConformU tarball) gets a long one.
    seen_timeouts = []

    class Resp:
        headers = {}

        def __enter__(self):
            return self

        def __exit__(self, *a):
            return False

        def read(self):
            raise http.client.IncompleteRead(b"partial", 1000)

    real_urlopen = urllib.request.urlopen
    urllib.request.urlopen = lambda req, timeout: seen_timeouts.append(timeout) or Resp()
    try:
        try:
            http_fetch("https://d.test/big.tar.xz")
        except FetchError as e:
            assert "IncompleteRead" in str(e) or "partial" in str(e) or "read" in str(e).lower(), e
        else:
            raise AssertionError("a dropped connection must raise FetchError")
        broken = check({"key": "doc:t", "name": "t", "current": "0", "compare": "equal", "document": True,
                        "check": ("hash", "https://d.test/big.tar.xz")}, http_get)
        assert broken["error"], broken
    finally:
        urllib.request.urlopen = real_urlopen
    assert seen_timeouts == [PAGE_TIMEOUT, DOCUMENT_TIMEOUT], seen_timeouts
    assert PAGE_TIMEOUT <= 30 < DOCUMENT_TIMEOUT
    # A truncated or corrupt gzip body is a broken source, not a crash of the whole run.
    for bad in (gzip.compress(b"page")[:-6], b"not gzip at all"):
        try:
            decode_body(bad, "gzip")
        except FetchError:
            pass
        else:
            raise AssertionError(f"corrupt gzip {bad!r} must raise FetchError")
    assert parse_version("V1.41") == ((1, 41), False) and highest(["2.0.2.1", "2.1.0.1"]) == "2.1.0.1"
    zwo = "https://zwo.test/sdk"
    pages[zwo] = json.dumps([{"acf": {"software_downloads": [
        {"software_download_title": "EAF SDK", "software_version": "V1.8.1"},
        {"software_download_title": "ASI Camera SDK", "software_version": "V1.41"}]}}])
    assert latest_json(zwo, "0.acf.software_downloads", {"software_download_title": "ASI Camera SDK"},
                       "software_version", get) == "V1.41"
    for path, match in (("0.acf.gone", {"software_download_title": "EAF SDK"}),
                        ("0.acf.software_downloads", {"software_download_title": "Renamed SDK"})):
        try:
            latest_json(zwo, path, match, "software_version", get)
        except FetchError:
            pass
        else:
            raise AssertionError(f"{path} {match} must be a broken source")
    pages["https://api.github.com/repos/indilib/indi/commits?path=drivers/x.cpp&per_page=1"] = json.dumps([{"sha": "b" * 40}])
    pages["https://api.github.com/repos/indilib/indi/commits?path=drivers/gone.cpp&per_page=1"] = "[]"
    assert latest_file_commit("indilib/indi", "drivers/x.cpp", get) == "b" * 40
    try:
        latest_file_commit("indilib/indi", "drivers/gone.cpp", get)
    except FetchError:
        pass
    else:
        raise AssertionError("a file no commit touches is a broken source")
    docs = {"https://d.test/a.html": (b"<html><script>var t=1;</script><p>Cmd  :GR#</p><!-- 12:00 --></html>", "text/html"),
            "https://d.test/b.html": (b"<html><script>var t=2;</script><p>Cmd\n:GR#</p><!-- 12:01 --></html>", "text/html"),
            "https://d.test/c.pdf": (b"%PDF-1", "application/pdf")}
    fetch = docs.__getitem__
    assert content_hash("https://d.test/a.html", fetch) == content_hash("https://d.test/b.html", fetch), \
        "scripts, comments and whitespace do not change an HTML page's hash"
    assert content_hash("https://d.test/c.pdf", fetch) == hashlib.sha256(b"%PDF-1").hexdigest()
    pages["https://o.test/dl"] = "Astroasis_Oasis_Focuser_ASCOM_2.1.0.1_Setup.zip Oasis_Focuser_Firmware_Ver_1.4.0.zip"
    table = {"sdks": [{"name": "zwo-eaf-sdk", "version": "1.7.7",
                       "updates": {"provider": "json", "url": zwo, "path": "0.acf.software_downloads",
                                   "match": {"software_download_title": "EAF SDK"}, "field": "software_version"}}],
             "documents": [
                 {"name": "indi-x", "check": {"provider": "github-file", "repo": "indilib/indi", "file": "drivers/x.cpp"},
                  "baseline": "a" * 40},
                 {"name": "pdf", "path": "AlpacaCore/external/x.md", "check": {"provider": "hash", "url": "https://d.test/c.pdf"},
                  "baseline": hashlib.sha256(b"%PDF-1").hexdigest()},
                 {"name": "oasis", "check": {"provider": "page", "url": "https://o.test/dl",
                                             "pattern": r"Astroasis_Oasis_Focuser_ASCOM_(\d+(?:\.\d+)+)_Setup"},
                  "baseline": "2.0.2.1"}]}
    ts, manual = targets({"manifests": {}}, table)
    assert manual == [] and [t["key"] for t in ts] == ["doc:indi-x", "doc:oasis", "doc:pdf", "zwo-eaf-sdk"], ts
    results = [check(t, get, fetch) for t in ts]
    res = {r["key"]: r for r in results}
    assert all(r["error"] is None for r in results), results
    assert [k for k in res if differs(res[k])] == ["doc:indi-x", "doc:oasis", "zwo-eaf-sdk"], \
        "a changed commit, a newer installer and a newer SDK differ; an unchanged hash does not"
    f = Fake()
    reconcile(results, f, f.issues)
    by_title = {i["title"]: i for i in f.issues}
    assert set(by_title) == {"Upstream document changed: indi-x aaaaaaaaaaaa -> bbbbbbbbbbbb",
                             "Upstream document changed: oasis 2.0.2.1 -> 2.1.0.1",
                             "Update zwo-eaf-sdk 1.7.7 -> V1.8.1"}, set(by_title)
    assert by_title["Upstream document changed: oasis 2.0.2.1 -> 2.1.0.1"]["labels"] == DOC_LABELS
    assert by_title["Update zwo-eaf-sdk 1.7.7 -> V1.8.1"]["labels"] == UPDATE_LABELS
    # The baseline is set to upstream in a PR: the document issue closes.
    table["documents"][0]["baseline"] = "b" * 40
    ts, _ = targets({"manifests": {}}, table)
    f.calls.clear()
    reconcile([check(t, get, fetch) for t in ts], f, f.issues)
    assert any(c[0] == "close" for c in f.calls), f.calls
    assert not [i for i in f.issues if i["state"] == "OPEN" and "indi-x" in i["title"]]
    closed = [i for i in f.issues if "indi-x" in i["title"]][0]["closing_comment"]
    assert "baseline now matches" in closed and "b" * 13 not in closed, closed

    # --- Debian-provided libraries tracked against their upstream (gphoto: libgphoto2, LibRaw) ---
    pages["https://api.github.com/repos/gphoto/libgphoto2/releases/latest"] = json.dumps({"tag_name": "v2.5.34"})
    pages["https://api.github.com/repos/gphoto/gphoto2/releases/latest"] = json.dumps({"tag_name": "v2.5.32"})
    lib_table = {"sdks": [], "system_libraries": [
        {"name": "libgphoto2", "repo": "gphoto/libgphoto2", "debian": "libgphoto2", "baseline": "2.5.31"}],
        "documents": [{"name": "gphoto2-cli", "check": {"provider": "github-release", "repo": "gphoto/gphoto2"},
                       "baseline": "2.5.32"}]}
    ts, manual = targets({"manifests": {}}, lib_table)
    by_key = {t["key"]: t for t in ts}
    assert manual == [] and set(by_key) == {"libgphoto2", "doc:gphoto2-cli"}, by_key
    lib = check(by_key["libgphoto2"], get)
    assert lib["latest"] == "v2.5.34" and differs(lib) and lib["system"], lib
    cli = check(by_key["doc:gphoto2-cli"], get)
    assert cli["latest"] == "v2.5.32" and not differs(cli), cli
    f = Fake()
    reconcile([lib], f, f.issues)
    assert f.issues[0]["title"] == "Update libgphoto2 2.5.31 -> v2.5.34" and f.issues[0]["labels"] == UPDATE_LABELS
    assert "Debian" in f.issues[0]["body"] and "baseline" in f.issues[0]["body"], "says it is Debian-provided"

    # Issue tables have a real header row: an empty "| | |" header renders as a blank row (#1134).
    for body in (update_body({**lib, "system": False, "where": "w"}), system_body(lib),
                 document_body({**lib, "document": True, "path": "p"})):
        assert "| | |" not in body and "| Field | Value |\n|---|---|\n" in body, body

    # --- Changelogs in issues ---
    pages["https://api.github.com/repos/gphoto/libgphoto2/releases/tags/v2.5.34"] = json.dumps(
        {"html_url": "https://github.com/gphoto/libgphoto2/releases/tag/v2.5.34",
         "body": "* Nikon Z8 support @someone\n* fix ```fence``` #12\n" + "x" * 9000})
    notes = changelog_for(lib, get)
    assert "https://github.com/gphoto/libgphoto2/releases/tag/v2.5.34" in notes and "Nikon Z8 support" in notes
    assert notes.count("```") == 2, "upstream text sits in one code block; its own fences are neutralised"
    assert len(notes) < 5000, "long release notes are cut"
    f = Fake()
    reconcile([lib], f, f.issues, notes=lambda r: changelog_for(r, get))
    assert "Nikon Z8 support" in f.issues[0]["body"] and "## Changelog" in f.issues[0]["body"]
    # No release notes (tag only): a compare link; a failing lookup: no section, the issue still opens.
    tag_only = {**lib, "latest": "v9.9.9"}
    assert "compare/v2.5.31...v9.9.9" in changelog_for(tag_only, get), "the baseline takes upstream's v prefix"
    f = Fake()
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        reconcile([tag_only], f, f.issues, notes=lambda r: 1 / 0)
    assert len(f.issues) == 1 and "## Changelog" not in f.issues[0]["body"]
    assert "no changelog for libgphoto2" in err.getvalue(), "a failed changelog lookup is logged, not silent"
    # A changed INDI file links the commits between the baseline and upstream.
    doc = {"key": "doc:indi-x", "name": "indi-x", "current": "a" * 40, "latest": "b" * 40, "document": True,
           "compare": "equal", "check": ("github-file", "indilib/indi", "drivers/x.cpp"), "source": "s"}
    assert "https://github.com/indilib/indi/compare/" + "a" * 40 + "..." + "b" * 40 in changelog_for(doc, get)
    # Changelog links that come from the table, through targets(): a json SDK's changelog_field
    # (ZWO's software_update_log_url, from the matching entry only) and a static changelog URL.
    pages["https://zwo.test/cl"] = json.dumps([{"acf": {"software_downloads": [
        {"software_download_title": "CAA SDK", "software_version": "V1.6.0", "software_update_log_url": "https://zwo.test/caa-log"},
        {"software_download_title": "EAF SDK", "software_version": "V1.8.1", "software_update_log_url": "https://zwo.test/eaf-log"}]}}])
    pages["https://q.test/sdk"] = "sdk_linux_arm64_26.07.28.tar.gz"
    cl_table = {"sdks": [
        {"name": "zwo-eaf-sdk", "version": "1.7.7",
         "updates": {"provider": "json", "url": "https://zwo.test/cl", "path": "0.acf.software_downloads",
                     "match": {"software_download_title": "EAF SDK"}, "field": "software_version",
                     "changelog_field": "software_update_log_url"}},
        {"name": "qhy-sdk", "version": "26.06.04",
         "updates": {"provider": "release-page", "url": "https://q.test/sdk",
                     "pattern": r"sdk_linux_arm64_(\d+\.\d+\.\d+)\.tar\.gz", "changelog": "https://q.test/log_en.md"}}]}
    cl = {t["key"]: check(t, get) for t in targets({"manifests": {}}, cl_table)[0]}
    zwo_notes = changelog_for(cl["zwo-eaf-sdk"], get)
    assert "https://zwo.test/eaf-log" in zwo_notes and "caa-log" not in zwo_notes, zwo_notes
    assert "https://q.test/log_en.md" in changelog_for(cl["qhy-sdk"], get)
    # A source with a static changelog URL in its table entry links it.
    qhy = {"key": "qhy-sdk", "name": "qhy-sdk", "current": "1", "latest": "2", "compare": "version",
           "check": ("page", "u", "p"), "changelog": "https://www.qhyccd.com/html/prepub/log_en.md", "source": "u"}
    assert "log_en.md" in changelog_for(qhy, get)

    # A large first scan: the issue body stays under GitHub's 65536-character limit, and the
    # findings it could not list stay unreported for the next run instead of failing every day.
    big = {"results": [{"packages": [pkg(f"lib{n}", [{"ids": [f"DEBIAN-CVE-2026-{n:05d}"], "max_severity": "8.0"}],
                                         [vuln(f"DEBIAN-CVE-2026-{n:05d}", fixed13, name=f"lib{n}")])
                                     for n in range(5000)]}]}
    f = Fake()
    osv_summary(big, f, f.issues, "o/r")
    assert len(f.issues) == 1 and len(f.issues[0]["body"]) < 65536, len(f.issues[0]["body"])
    listed = read_marker(f.issues[0]["body"])["ids"]
    assert 0 < len(listed) < 5000 and f"DEBIAN-CVE-2026-{0:05d}" in listed, len(listed)
    osv_summary(big, f, f.issues, "o/r")
    assert len(f.issues) == 2 and not set(listed) & set(read_marker(f.issues[1]["body"])["ids"])

    # A broken-source issue closed by hand stays closed while the source is still unreadable,
    # and is cleared once the source reads again, so a later break is reported.
    f = Fake()
    bad = [{"key": "x/y", "name": "x/y", "current": "1.0", "where": "ci.yml", "source": "s",
            "latest": None, "error": "HTTP 500"}]
    good = [{**bad[0], "latest": "1.0", "error": None}]
    reconcile(bad, f, f.issues)
    f.issues[0]["state"] = "CLOSED"
    f.calls.clear()
    reconcile(bad, f, f.issues)
    assert f.calls == [], "a hand-closed broken-source issue is not reopened"
    reconcile(good, f, f.issues)
    assert read_marker(f.issues[0]["body"]).get("resolved"), "a readable source clears the hand-close"
    f.calls.clear()
    reconcile(bad, f, f.issues)
    assert ("create", "Update check broken: x/y") in f.calls, f.calls

    # One failed issue call does not stop the others; the failure is returned.
    class Flaky(Fake):
        def create(self, title, body, labels):
            if "first" in title:
                raise subprocess.CalledProcessError(1, ["gh"], stderr="label not found")
            super().create(title, body, labels)

    f = Flaky()
    two = [{**bad[0], "key": "first", "name": "first"}, {**bad[0], "key": "second", "name": "second"}]
    log = reconcile(two, f, f.issues)
    assert [i["title"] for i in f.issues] == ["Update check broken: second"], f.issues
    assert any(line.startswith("FAILED") and "first" in line for line in log), log
    print("dependency_updates self-test: OK")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--osv-summary", metavar="FILE")
    ap.add_argument("--filter-sarif", metavar="SARIF", help="write the code-scanning SARIF scoped to --direct-*")
    ap.add_argument("--report", metavar="FILE", help="osv-scanner JSON, for --filter-sarif")
    ap.add_argument("--output", metavar="FILE", help="filtered SARIF, for --filter-sarif")
    ap.add_argument("--direct-depends", metavar="FILE", help="the .deb's Depends field (dpkg-deb -f DEB Depends)")
    ap.add_argument("--direct-sources", metavar="FILE", help="'binary source' lines from dpkg-query in the image")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    direct = None
    if a.direct_depends and a.direct_sources:
        direct = direct_sources(Path(a.direct_depends).read_text(encoding="utf-8"),
                                Path(a.direct_sources).read_text(encoding="utf-8"))
    if a.filter_sarif:
        if direct is None or not a.report or not a.output:
            ap.error("--filter-sarif needs --report, --output, --direct-depends and --direct-sources")
        sarif = json.loads(Path(a.filter_sarif).read_text(encoding="utf-8"))
        report = json.loads(Path(a.report).read_text(encoding="utf-8"))
        total, unreadable = sarif_unreadable(sarif)
        if total and unreadable == total:
            print(f"error: none of the {total} SARIF results matches the expected message format "
                  f"({SARIF_PACKAGE_RE.pattern!r}); has osv-scanner changed it? Not uploading an empty result.",
                  file=sys.stderr)
            return 1
        out = filter_sarif(sarif, report, direct)
        Path(a.output).write_text(json.dumps(out), encoding="utf-8")
        before = sum(len(r.get("results") or []) for r in sarif.get("runs") or [])
        after = sum(len(r.get("results") or []) for r in out.get("runs") or [])
        print(f"code scanning: {after} of {before} findings are in the .deb's direct dependencies "
              f"({', '.join(sorted(direct))}) and not rated unimportant")
        return 0
    issues = GhIssues(a.dry_run)
    if a.osv_summary:
        report = json.loads(Path(a.osv_summary).read_text(encoding="utf-8"))
        if direct is not None and osv_scope_unreadable(report, Path(a.direct_sources).read_text(encoding="utf-8")):
            print("error: osv-scanner's JSON names packages by binary name, not Debian source package; the "
                  "direct-dependency scope would match nothing. Not opening a misleading summary.", file=sys.stderr)
            return 1
        repo = os.environ.get("GH_REPO", "open-astro/AlpacaBridge")
        log = osv_summary(report, issues, issues.list("all"), repo, direct)
        print("\n".join(log) or "no new High/Critical findings")
        return 0
    snap = snapshot.build_snapshot(ROOT, "0" * 40, "refs/heads/main", "local", "1970-01-01T00:00:00Z")
    table = json.loads((ROOT / snapshot.TABLE).read_text(encoding="utf-8"))
    ts, manual = targets(snap, table)
    results = [check(t, http_get) for t in ts]
    print("| Dependency | Repository | Upstream | Status |\n|---|---|---|---|")
    for r in results:
        status = ("source unreadable: " + r["error"]) if r["error"] else (
            ("changed" if r.get("document") else "update available") if differs(r) else "current")
        print(f"| {r['name']} | {short(r['current'])} | {short(r['latest'] or '-')} | {status} |")
    for name in manual:
        print(f"| {name} | - | - | manual check (no `updates` source) |")
    log = reconcile(results, issues, issues.list("all"), notes=lambda r: changelog_for(r, http_get))
    print("\n" + ("\n".join(f"- {line}" for line in log) or "- no issue changes"))
    return 1 if any(line.startswith("FAILED") for line in log) else 0


if __name__ == "__main__":
    sys.exit(main())
