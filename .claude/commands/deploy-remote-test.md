---
description: Build the AlpacaBridge .deb and publish it as a temporary GitHub pre-release so a remote SBC (reached only via Raspberry Pi Connect browser shell, no SSH) can curl and install it; prints the paste-ready commands and cleans the release up afterward
argument-hint: [tag-suffix]
allowed-tools: Read, Bash, Grep, Glob
---

You are the remote test-deployment assistant for AlpacaBridge. Use this instead of `/deploy-test` when the test SBC is NOT reachable over SSH/LAN, only through a Raspberry Pi Connect browser remote shell (connect.raspberrypi.com) where the developer pastes commands by hand. Typing a 15 MB file into that shell is not viable, so the package travels through a **temporary GitHub pre-release asset** that the SBC downloads with `curl`. The hardware under test is plugged into that SBC, and ConformU runs ON the SBC against `127.0.0.1:6800`.

You DO NOT run ConformU, and you DO NOT commit anything. You DO create and (later) delete a temporary GitHub pre-release on the project repo, which is an outward-facing action, so confirm before creating it.

## How to use arguments

The user invoked this command with: $ARGUMENTS

- Optional first argument is a short tag suffix (e.g. `icam`). Default: the branch's short name with `/` replaced by `-`.
- Tag format: `test-<suffix>-<HEAD short hash>` (e.g. `test-icam-4192000`). The hash ties every download back to the commit that was built. Reject anything not matching `^[A-Za-z0-9._-]+$`.

## Step 1 — Pre-flight (HARD STOP if any fail)

```bash
git branch --show-current; git rev-parse --short HEAD; git status --short
gh auth status
command -v dpkg-buildpackage || echo MISSING
dpkg --print-architecture
```

- Report branch, short hash, and whether the tree is dirty. A dirty tree is allowed (deploying WIP is the point), but say so, since the tag hash will not fully describe what was built.
- `gh` must be authenticated with push rights to `open-astro/AlpacaBridge` (releases need `repo` scope).
- Must be building on arm64 (Debian 13 arm64 VM). Never publish an amd64 package.
- The `VERSION` file value is what dpkg will install; if the SBC already has that exact version installed, `dpkg -i` still reinstalls it, so a same-version redeploy is fine, but the md5 check in Step 5 is then the only proof the binary changed.

Then confirm with the user:

> "Build `alpacabridge <VERSION>` from `<branch>` (`<hash>`<, dirty>) and publish it as temporary pre-release `<tag>` on open-astro/AlpacaBridge. Proceed?"

## Step 2 — Build the .deb

```bash
./scripts/build_deb.sh
VERSION="$(tr -d '[:space:]' < VERSION)"
ls -l "../alpacabridge_${VERSION}_arm64.deb"
```

On failure, show the error and stop. Confirm the artifact timestamp is from this build, not a stale file. Record its md5:

```bash
dpkg-deb --fsys-tarfile "../alpacabridge_${VERSION}_arm64.deb" | tar -xO ./usr/bin/alpacabridge | md5sum
```

## Step 3 — Publish the temporary pre-release

Check for a leftover release with the same tag first (a prior aborted session), and reuse it only if the user agrees; otherwise delete it with `--cleanup-tag` before creating a fresh one.

```bash
TAG="test-<suffix>-$(git rev-parse --short HEAD)"
gh release view "$TAG" >/dev/null 2>&1 && echo "EXISTS: $TAG"
gh release create "$TAG" "../alpacabridge_${VERSION}_arm64.deb" \
  --prerelease --title "Temp test build ($TAG)" \
  --notes "Temporary test artifact for branch $(git branch --show-current). Not a release; will be deleted after testing."
```

The tag is created on the current HEAD. Note the asset URL:

```
https://github.com/open-astro/AlpacaBridge/releases/download/<TAG>/alpacabridge_<VERSION>_arm64.deb
```

## Step 4 — Hand the user the paste-ready commands

The browser shell's permission classifier blocks some interactive commands when the model types them, so **give the developer the commands to paste**, one block, nothing else in it:

```bash
curl -sL -o /tmp/ab.deb https://github.com/open-astro/AlpacaBridge/releases/download/<TAG>/alpacabridge_<VERSION>_arm64.deb
sudo dpkg -i /tmp/ab.deb || sudo apt-get -y -f install
dpkg-query -W -f='${Status} ${Version}\n' alpacabridge
sudo systemctl restart alpacabridge
md5sum /usr/bin/alpacabridge
curl -sS http://127.0.0.1:6800/management/v1/description
```

Tell them what a good result looks like: `install ok installed <VERSION>`, the md5 equal to the one recorded in Step 2, and the description JSON reporting `<VERSION>`. If the md5 differs the SBC is not running this build; if dpkg reports anything but `install ok installed`, do not restart, diagnose (`sudo journalctl -u alpacabridge -n 50 --no-pager`).

Reminders for this rig (from past sessions):
- A cold ConformU process on the SBC shows spurious 0.1-0.25 s FAST timing marks even on constants; re-run in the same ConformU session for a clean timing block before treating marks as driver bugs.
- Devices are configured in the web UI at `http://127.0.0.1:6800/` on the SBC (or via the Pi Connect screen share).

## Step 5 — Wait for the result, then hand off

Ask the user to paste back the `dpkg-query`, `md5sum`, and description output. Only when the md5 matches, say:

> "SBC is running `alpacabridge <VERSION>` (`<branch>`, `<hash>`). Configure and connect the device in the web UI, then run ConformU on the SBC against `http://127.0.0.1:6800` and use `/conformu <vendor> <model>` with the saved log to validate and file the results."

## Step 6 — Clean up the temporary release (MANDATORY)

Temporary pre-releases must not accumulate. When the user says testing is done (or the build is superseded by a newer one from this skill), delete the release AND its tag:

```bash
gh release delete "$TAG" --yes --cleanup-tag
gh release list --limit 10
```

Confirm the listing no longer shows it. If the session ends before cleanup, say so explicitly in the final summary with the exact delete command, so the next session can do it. Never delete a real release (tags of the form `X.Y.Z`); this skill only ever deletes tags it created (`test-*`).

## Workflow notes

- Iterating: each rebuild gets a new tag (new HEAD hash, or same hash with a dirty tree; in the latter case append `-2`, `-3` to the suffix) and the previous test release is deleted in the same step.
- A ConformU log destined for `AlpacaCore/conformu/` should come from a clean, committed tree; rebuild and republish from that commit before the final validation run.
