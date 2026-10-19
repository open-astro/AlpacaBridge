#!/usr/bin/env python3
"""Delete the Actions caches a newer cache with the same key prefix has superseded.

The compiler caches are saved under a key that ends in the commit SHA
(`ccache-build-test-<sha>`) and restored by prefix (`restore-keys:
ccache-build-test-`), which picks the newest cache with that prefix. Every push
to main saves a new set of about 2 GB, so the repository passes GitHub's 10 GB
cache limit within a few pushes and GitHub evicts the least recently used
caches: the nightly's `ccache-nightly-optimized-` cache, saved once a day, was
gone within hours. Only the newest cache per prefix can ever be restored, so
the older ones are deleted. The workflow .github/workflows/cache-prune.yml (issue #1350) runs
this after each CI, Nightly Build and CodeQL run on main.

The prefix is the key up to a 40-hex commit SHA, so a prefix that begins
another one (`x-` and `x-tsan-`) is still its own group, and a CodeQL key with the run
id after the SHA groups by the part before it. A key without a commit SHA (the
cppcheck build, keyed by a 64-hex hash of ci.yml) is never deleted. Caches on
different refs never supersede each other.

Usage:
  prune_actions_caches.py [--repo OWNER/NAME] [--dry-run]
  prune_actions_caches.py --self-test
"""
import argparse
import json
import os
import re
import subprocess
import sys

SHA_KEY_RE = re.compile(r"^(?P<prefix>.*?-)[0-9a-f]{40}(?:-|$)")


def superseded(caches):
    """The caches in `caches` (GitHub API objects) that a newer cache with the same ref and key prefix replaces."""
    groups = {}
    for c in caches:
        m = SHA_KEY_RE.match(c["key"])
        if m:
            groups.setdefault((c["ref"], m.group("prefix")), []).append(c)
    stale = []
    for group in groups.values():
        group.sort(key=lambda c: c["created_at"], reverse=True)
        stale.extend(group[1:])
    return stale


def gh(*args):
    return subprocess.run(["gh", *args], capture_output=True, text=True)


def list_caches(repo):
    res = gh("api", "--paginate", "repos/%s/actions/caches?per_page=100" % repo, "--jq", ".actions_caches[]")
    if res.returncode != 0:
        raise RuntimeError("listing the caches failed: " + res.stderr.strip())
    return [json.loads(line) for line in res.stdout.splitlines() if line.strip()]


def run(args):
    caches = list_caches(args.repo)
    stale = superseded(caches)
    total = sum(c["size_in_bytes"] for c in caches)
    freed = sum(c["size_in_bytes"] for c in stale)
    print("%d caches, %.2f GB; %d superseded, %.2f GB." % (len(caches), total / 1e9, len(stale), freed / 1e9))
    failed = 0
    for c in stale:
        if args.dry_run:
            print("- would delete `%s` (%s)" % (c["key"], c["ref"]))
            continue
        res = gh("api", "-X", "DELETE", "repos/%s/actions/caches/%d" % (args.repo, c["id"]))
        if res.returncode == 0:
            print("- deleted `%s` (%s)" % (c["key"], c["ref"]))
        elif "404" in res.stderr or "Not Found" in res.stderr:
            print("- `%s` is already gone" % c["key"])
        else:
            failed += 1
            print("- deleting `%s` failed: %s" % (c["key"], res.stderr.strip()))
    return 1 if failed else 0


def self_test():
    sha = ["%040x" % n for n in range(1, 5)]

    def cache(n, key, created, ref="refs/heads/main"):
        return {"id": n, "key": key, "ref": ref, "created_at": created, "size_in_bytes": 1}

    caches = [
        cache(1, "ccache-build-test-" + sha[0], "2026-10-19T03:00:00Z"),
        cache(2, "ccache-build-test-" + sha[1], "2026-10-19T04:00:00Z"),
        cache(3, "ccache-build-test-" + sha[2], "2026-10-19T05:00:00Z"),
        # One prefix begins the other (the ASan key did until #1355); each keeps its own newest cache.
        cache(4, "ccache-sanitizers-" + sha[0], "2026-10-19T03:00:00Z"),
        cache(5, "ccache-sanitizers-tsan-" + sha[1], "2026-10-19T04:00:00Z"),
        cache(6, "ccache-sanitizers-tsan-" + sha[0], "2026-10-19T03:30:00Z"),
        # CodeQL: run id and attempt after the SHA.
        cache(7, "codeql-overlay-base-database-1-6144-cpp-2.27.2-%s-39202587957-1" % sha[0], "2026-10-19T03:00:00Z"),
        cache(8, "codeql-overlay-base-database-1-6144-cpp-2.27.2-%s-39204739249-1" % sha[1], "2026-10-19T04:00:00Z"),
        # No commit SHA in the key: never deleted, even with an older one beside it.
        cache(9, "cppcheck-Linux-ARM64-" + "a" * 64, "2026-10-18T22:00:00Z"),
        cache(10, "cppcheck-Linux-ARM64-" + "b" * 64, "2026-10-19T05:00:00Z"),
        # The only cache with its prefix: kept however old.
        cache(11, "ccache-nightly-optimized-" + sha[3], "2026-10-10T00:00:00Z"),
        # Another ref does not supersede main's cache, nor main's it.
        cache(12, "ccache-build-test-" + sha[3], "2026-10-19T06:00:00Z", ref="refs/pull/7/merge"),
        # A SHA followed by more hex is not a commit SHA.
        cache(13, "x-" + sha[0] + "ff", "2026-10-19T01:00:00Z"),
        cache(14, "x-" + sha[1] + "ff", "2026-10-19T02:00:00Z"),
    ]
    cases = [
        ("newest per prefix kept, the rest superseded", caches, [1, 2, 6, 7]),
        ("nothing listed", [], []),
        ("one cache", [caches[0]], []),
        ("input order does not matter", list(reversed(caches)), [1, 2, 6, 7]),
    ]
    bad = 0
    for name, given, want in cases:
        got = sorted(c["id"] for c in superseded(given))
        if got != want:
            bad += 1
            print("FAIL %s: got %r, want %r" % (name, got, want))
    if bad:
        return 1
    print("prune_actions_caches self-test: %d cases passed" % len(cases))
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--self-test", action="store_true")
    p.add_argument("--repo", default=os.environ.get("GH_REPO"))
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args()
    if args.self_test:
        return self_test()
    if not args.repo:
        p.error("--repo or GH_REPO is required")
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
