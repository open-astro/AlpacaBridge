---
description: Cut an AlpacaBridge release end to end — finalize VERSION, README badge and CHANGELOG date, write plain-language release notes, open and merge the release PR, tag it, and verify the GitHub Release
allowed-tools: Read, Edit, Write, Bash, Grep, Glob
---

You are the release assistant for the AlpacaBridge project. `/bump-release` turns the current
changelog fragments (`changelog.d/`) into a shipped release. It does the whole job in one
run: version files, plain-language release notes, the release PR through the review bot, the tag,
and a check that the GitHub Release published with the right notes. The user reads the CHANGELOG
for technical detail; the GitHub Release is written for someone standing at a telescope.

The user may pass a version (`/bump-release 4.0.0`). Without one, the version is the one
`python3 scripts/changelog_fragments.py --bump` proposes from the fragments.

## Step 1 — Preconditions

```bash
git branch --show-current
git status --porcelain
git fetch origin main --quiet && git log --oneline HEAD..origin/main | head
grep -n '^## \[' CHANGELOG.md | head -2
ls changelog.d
cat VERSION
gh release list --limit 1
```

- The working tree must be clean. If not, STOP and tell the user to `/commit` first.
- Releases are cut from an up-to-date `main`. If on `main`, pull first. If on another branch,
  ask whether to release from `main` (the normal case) — never cut a release from a stale or
  half-merged branch.
- `changelog.d/` must hold at least one fragment besides `README.md` (or `CHANGELOG.md` a legacy
  `## [X.Y.Z] - UNRELEASED` section). If neither exists and the top heading is already dated, the
  release has been cut; go to Step 6 (tag) if no tag exists, otherwise report and stop.
- Run `python3 scripts/changelog_fragments.py --check` (it must pass), then
  `python3 scripts/changelog_fragments.py --bump`: it prints the proposed `X.Y.Z` from the latest
  dated release (a legacy UNRELEASED label is a floor) per the SemVer rule in AGENTS.md ("Version
  bump policy"): a `Breaking changes` subsection means major, an unqualified `Added` means minor,
  otherwise patch. `X.Y.Z` must be greater than the latest release tag; use the proposal unless
  the user names a higher one. `--preview` prints the section that Step 2 will write.
- **Never commit on `main`.** Create `release/X.Y.Z` before any edit.

```bash
git checkout main && git pull --ff-only && git checkout -b release/X.Y.Z
```

## Step 2 — Finalize the version files

Today's date in `YYYY-MM-DD` (UTC is fine). Then:

1. `printf '%s\n' X.Y.Z > VERSION`
2. README badge line: `#### [X.Y.Z] - YYYY-MM-DD &middot; [Changelog](CHANGELOG.md)`.
   `scripts/check_docs_drift.py` check 4 requires it to match `VERSION` exactly.
3. CHANGELOG: `python3 scripts/changelog_fragments.py --release X.Y.Z --date YYYY-MM-DD`. It writes
   the expanded `## [X.Y.Z] - YYYY-MM-DD` section under the intro, collapses the previous release
   into `<details>`, merges any legacy `UNRELEASED` section, and deletes the fragments (commit
   those deletions). The newest release stays expanded.
4. README headline count: `- **N validated devices. M brands. One server.** <brand list>`.
   Recount rather than trust the old number — the line was three releases stale at 4.0.0. The
   script that gates the line (check 15, issue #684) also prints its numbers, so there is one
   command and one copy of the row filter (issue #689):

   ```bash
   python3 scripts/check_docs_drift.py --counts
   ```

   It prints N (validated model rows), the brand count with its spelled-out word, the brand list,
   a paste-ready `headline:` line and the README's current line. Replace the README line with the
   `headline:` one when they differ. The list keeps the README's existing item order and appends
   new brands at the end; reorder by hand if you want to. Do not restate the row filter here or
   anywhere else: `count_validated_device_rows` in the script owns it, and the failure message of
   the full check names the numbers.

   The brand list is not the heading list: the script maps every `### ` heading in
   `SUPPORTED-DRIVERS.md` onto a README item through `SUPPORTED_HEADING_TO_README_BRAND` (the two
   Sky-Watcher headings share one item, and `README_BRANDS_WITHOUT_HEADING` covers the Unihedron
   SQM-LE item, a sensor read through the WeeWX driver with no row of its own). A heading the map
   does not know fails the gate, and `--counts` names it as `unmapped heading`, so a new brand needs
   its README item and a map entry together. Fix what the check reports rather than working around it.
5. The `## Updated YYYY-MM-DD` line near the top of `SUPPORTED-DRIVERS.md` → today's date. Every release re-verifies
   the file (Step 2.4 recounts from it), and check 16 (issue #692) fails when the line is older than
   the README badge date, so this is not optional: it read 2026-09-24 on the 2026-09-27 release.
   Between releases `/conformu` and `/commit` move it forward; it may run ahead of the badge, never
   behind.

Verify with `python3 scripts/check_docs_drift.py` before moving on.

## Step 3 — Write the plain-language release notes

Create `docs/releases/X.Y.Z.md`. `release.yml` uses this file as the GitHub Release body when it
exists, with a link to the CHANGELOG section appended; the CHANGELOG stays the technical record.

Read the whole, now dated, `## [X.Y.Z]` CHANGELOG section (`python3 scripts/changelog_section.py X.Y.Z`) and
translate it. Rules for the file:

- **Audience**: an amateur astronomer deciding whether to `apt upgrade` tonight. No issue numbers,
  no PR numbers, no class, function, file, mutex or flag names, no "AlpacaCore"/"AlpacaHTTP".
  Say what the user sees and what to do, not how it was fixed.
- **Shape** (skip a section that would be empty):
  1. `# AlpacaBridge X.Y.Z` and a two-sentence summary of the release.
  2. **Read this first** — every "Breaking changes" bullet, each as: what changed, what the user
     must do, where in the web UI. This section is mandatory for a major release.
  3. **New gear you can plug in** — new drivers and newly supported models, grouped by brand.
     Name the web UI vendor to pick when a rebadge is involved.
  4. **More hardware confirmed working** — ConformU-validated models with no driver change,
     one sentence listing them.
  5. **Things that just work better** — user-visible improvements.
  6. **Fixes worth knowing about** — bugs a user could have hit, one line each.
  7. **Upgrading** — the `apt update && apt upgrade alpacabridge` block, plus any post-upgrade
     step the breaking changes require.
- Style: short sentences, bold the first words of each bullet, no em dashes (use commas or
  periods), no headers deeper than `##`. Internal-only entries (CI gates, test seams, skills,
  review-bot changes, doc drift checks) collapse into one closing line under "Fixes" at most, or
  are left out.
- Show the user the notes and get an OK before continuing; wording is their call.

## Step 4 — Record the release in the CHANGELOG (first run only)

If this is the first release using `docs/releases/`, nothing more is needed: the workflow change
shipped in 4.0.0. Otherwise leave the CHANGELOG alone; the release notes file is not a changelog
entry.

## Step 5 — Commit, PR, review loop, merge

1. Run `./scripts/ci_preflight.sh`. It must pass; fix anything it flags on this branch.
2. Show the user the diff summary and this commit message, and wait for approval:

   ```
   Release X.Y.Z

   VERSION, README badge and CHANGELOG date set to X.Y.Z / YYYY-MM-DD; plain-language
   release notes in docs/releases/X.Y.Z.md.
   ```

   Then commit (with the session's attribution trailer) and push the branch.
3. Open the PR with `gh pr create` titled `Release X.Y.Z`, body = the two-sentence summary from
   the notes plus "Notes for the GitHub Release: `docs/releases/X.Y.Z.md`." Do not go through
   `/submit-pr`'s release question; this skill already did that work.
4. Run the `/pr-checker` loop on the PR until it is merged. A docs-only release PR should be one
   round; the bot's notes on wording of the release notes are the user's call, not defects.

## Step 6 — Tag and verify the Release

```bash
git checkout main && git pull --ff-only
test "$(tr -d '[:space:]' < VERSION)" = "X.Y.Z"
git tag -a vX.Y.Z -m "Release X.Y.Z"
git push origin vX.Y.Z
```

Then wait for the `Release` workflow (it is text-only and finishes in under a minute):

```bash
gh run list --workflow=release.yml --limit 1
gh run watch "$(gh run list --workflow=release.yml --limit 1 --json databaseId --jq '.[0].databaseId')" --exit-status
gh release view vX.Y.Z --json name,body --jq '.name, (.body | .[0:400])'
```

The body must start with the plain-language notes, not the CHANGELOG bullets. If the workflow
failed (tag/VERSION mismatch, an undated CHANGELOG section), fix the cause on a new PR, delete and re-push
the tag after it merges (`git tag -d vX.Y.Z && git push origin :refs/tags/vX.Y.Z`), and verify
again. If the workflow never ran, the tag landed on a commit without `release.yml`; create the
Release by hand with `gh release create vX.Y.Z --notes-file docs/releases/X.Y.Z.md --verify-tag`.

## Step 7 — Wrap up

- Delete the local `release/X.Y.Z` branch (origin deletes the remote one on merge).
- Report: the version, the PR number, the tag, the Release URL, and the apt publish reminder
  (apt.openastro.net is published outside this repo; the Release is not the install channel).
- The next feature PR adds its own `changelog.d/` fragment; the next release collapses this
  section into `<details>`.
