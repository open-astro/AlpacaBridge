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

## Modes (beta channel, `docs/beta-channel.md`)

Every minor or major release goes through a stable branch `stable/X.Y` and an opt-in beta first.
Pick the mode from the argument and the branch (ask when unclear):

- **Beta** (`/bump-release beta` or `X.Y.0-beta.N`): on `main`, cut `stable/X.Y` for the first beta
  (`git checkout main && git pull --ff-only && git checkout -b stable/X.Y && git push -u origin
  stable/X.Y`; branch protection and pushes to `stable/*` are the maintainer's); on `stable/X.Y`,
  the next beta. Follow the steps below with these differences: the release PR branch
  `release/X.Y.Z-beta.N` is cut from `stable/X.Y` and its PR targets `stable/X.Y`; `VERSION` and the
  README badge are `X.Y.0~betaN` (Step 2.1-2.2; the badge date is the beta date); skip Step 2.3: the
  fragments are NOT consumed and `CHANGELOG.md` keeps its entries; the one edit there is a legacy
  `## [A.B.C] - UNRELEASED` heading whose label is not `X.Y.0`, renamed to `## [X.Y.0] - UNRELEASED`
  (heading only), so the `.deb` build does not warn about the label; skip Step 2.4 unless it is stale,
  but run Step 2.5: the badge date is now the beta date, and docs-drift check 16 fails while the
  `## Updated` line in `SUPPORTED-DRIVERS.md` is older than it; Step 3 writes `docs/releases/X.Y.0-beta.N.md`, starting from
  `python3 scripts/changelog_fragments.py --preview --version X.Y.0~betaN` translated by the Step 3
  rules (it opens with a line saying it is a beta; the beta scope rule in Step 3 says what the
  second and later betas repeat); Step 5 and Step 6 use the beta spellings shown there; before the
  first beta tag of a branch, Step 6 dry-runs `release.yml`; Step 6 tags `vX.Y.0-beta.N` on
  `stable/X.Y` (`release.yml` publishes a pre-release, no dated CHANGELOG section needed) and then
  runs the **Merge down** below. Publishing the `.deb` to the apt `beta` component happens outside
  this repo. Every driver's `DriverVersion` reads `X.Y.0~betaN` during the beta (it is
  `alpacacore::kVersion`); ConformU only logs that string (`DeviceTesterBaseClass.cs`, the
  `DriverVersion` switch: OK for any non-empty value), so it is not a conformance failure.
- **Stable** (`/bump-release` on `stable/X.Y`, or a hotfix `X.Y.Z`): the checklist below, with
  every `main` read as `stable/X.Y`: the release PR targets `stable/X.Y`, Step 2.3 consumes the
  fragments (Step 2.1 wrote `VERSION` `X.Y.0` first; `--release` takes a bare `X.Y.Z` only and
  refuses a `~betaN`, so it cannot consume the fragments during a beta), Step 6 tags
  `vX.Y.Z` on `stable/X.Y`, then run the **Merge down**. Check the promotion criteria first: 14
  days since the last beta tag with no open regression, a full maintainer rig session on the final
  beta, ConformU re-run for every driver touched in the beta. A hotfix needs no beta round.
- **Plain release from `main`** is no longer the normal path; use it only when the maintainer says so
  (a stable tag is not branch-checked by `release.yml`; a beta tag must sit on its own `stable/X.Y` or the
  workflow refuses it).

**Merge down** (after every beta tag, after the stable tag, after a hotfix): open a PR from
`stable/X.Y` into `main` (and, for a hotfix while `stable/X.Z` is in beta, a second one into
`stable/X.Z`), each from its own short-lived `merge-down/X.Y-to-<main|X.Z>` head (never
`stable/X.Y` itself), and merge it with the merge-commit method, never squash
(`/submit-pr --merge-down stable/X.Y [--into stable/X.Z]`, then `/pr-checker`). Two repository
settings decide whether that is safe, and neither is in this repo: before the first merge down
confirm `gh api repos/open-astro/AlpacaBridge --jq .allow_merge_commit` prints `true` (it did on
2026-10-09 NZ) and that an active ruleset covers `refs/heads/stable/**` with `deletion` and
`non_fast_forward` rules (the `/submit-pr` merge-down section has the commands;
`delete_branch_on_merge` is on). Ruleset 24766374, "stable branch protection", has covered it
since 2026-10-09 (#924). If the query no longer lists it, stop and ask the maintainer to restore it. **Version files:** the receiving branch keeps its own `VERSION` and README badge, so `main`
never carries a beta `VERSION` and dev builds from `main` never report one; the exception is a
merge into `main` that brings a stable release newer than `main`'s `VERSION` (the promotion, or a
hotfix before the next promotion), which takes the stable side's `VERSION` and badge. The first
merge down after promotion therefore carries the dated CHANGELOG section, `VERSION`, badge and the
fragment deletions. A dated `## [X.Y.Z]` section is inserted in version order below the receiving
branch's sections; `docs/releases/` notes and the code come across as they are. After the final
merge down of a promotion, the previous `stable/` branch retires (no further tags); it is not
retired at the cut. Keep `release/X.Y.Z` as the short-lived
release PR branch name; never name a long-lived branch `release/...`.

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
- A beta is cut on an up-to-date `stable/X.Y` (the first one from `main`, which creates the
  branch); a stable release or hotfix is cut on an up-to-date `stable/X.Y`. Pull first. On any
  other branch, ask which mode applies — never cut a release from a stale or half-merged branch.
- `changelog.d/` must hold at least one fragment besides `README.md` (or `CHANGELOG.md` a legacy
  `## [X.Y.Z] - UNRELEASED` section). If neither exists and the top heading is already dated, the
  release has been cut; go to Step 6 (tag) if no tag exists, otherwise report and stop.
- Run `python3 scripts/changelog_fragments.py --check` (it must pass), then
  `python3 scripts/changelog_fragments.py --bump --version "$(tr -d '[:space:]' < VERSION)"`: it prints the proposed `X.Y.Z` from the latest
  dated release (a legacy UNRELEASED label and a beta `VERSION` are floors) per the SemVer rule in AGENTS.md ("Version
  bump policy"): a `Breaking changes` subsection means major, an unqualified `Added` means minor,
  otherwise patch. `X.Y.Z` must be greater than the latest release tag; use the proposal unless
  the user names a higher one. `--preview` prints the section that Step 2 will write.
- **Never commit on `main` or `stable/X.Y`.** Create `release/X.Y.Z` (beta: `release/X.Y.Z-beta.N`)
  before any edit, from the branch the mode names.

```bash
git checkout stable/X.Y && git pull --ff-only && git checkout -b release/X.Y.Z
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
- **Beta scope** (beta mode): the notes cover everything since the last stable release, because
  `--preview --version X.Y.0~betaN` renders every fragment on the branch and a beta tester may
  join at any beta. From the second beta on, add a `## New since X.Y.0-beta.(N-1)` section right
  after the summary, listing only the fragments added since the previous beta tag
  (`git diff --name-only vX.Y.0-beta.(N-1) -- changelog.d/`), translated by the same rules; the
  rest of the file is the full picture, restated.
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

   Beta mode uses the tag spelling and names what did not change:

   ```
   Release X.Y.0-beta.N

   VERSION and README badge set to X.Y.0~betaN / YYYY-MM-DD; fragments kept; plain-language
   beta notes in docs/releases/X.Y.0-beta.N.md.
   ```

   Then commit (with the session's attribution trailer) and push the branch.
3. Open the PR with `gh pr create` (beta and stable modes: `--base stable/X.Y`) titled `Release X.Y.Z`
   (beta: `Release X.Y.0-beta.N`). The body is
   `.github/PULL_REQUEST_TEMPLATE.md` filled in (the `pr-template` job refuses any other shape):
   the two-sentence summary from the notes under **What Changed**, plus "Notes for the GitHub
   Release: `docs/releases/X.Y.Z.md`."; "No issue exists" and the release under **Linked Issues
   or Issue Description**; the checks this skill ran under **Verification**. Check it with
   `python3 scripts/check_pr_template.py --body-file <file>`. Do not go through `/submit-pr`'s
   release question; this skill already did that work.
4. Run the `/pr-checker` loop on the PR until it is merged. A docs-only release PR should be one
   round; the bot's notes on wording of the release notes are the user's call, not defects.

## Step 6 — Tag and verify the Release

```bash
git checkout stable/X.Y && git pull --ff-only
test "$(tr -d '[:space:]' < VERSION)" = "X.Y.Z"
git tag -a vX.Y.Z -m "Release X.Y.Z"
git push origin vX.Y.Z
```

Beta mode (the tag spelling differs from `VERSION`; `release_tag.py` rejects any other form):

```bash
git checkout stable/X.Y && git pull --ff-only
test "$(tr -d '[:space:]' < VERSION)" = "X.Y.0~betaN"
test -f docs/beta-channel.md   # the Release body links this file at the tag
git tag -a vX.Y.0-beta.N -m "Release X.Y.0-beta.N"
git push origin vX.Y.0-beta.N
```

Before the first beta tag of a branch, dry-run the workflow against the merged branch and read
its "Build release notes" step and the `beta-deb` job's "Build the .deb" step (an all-vendors
build in a `debian:trixie` container, several minutes); it stops before publishing anything. Dispatch it with
`--ref stable/X.Y` and no other ref: a dry run has no tag, so the branch check tests the tip of the
dispatched branch, and from any other branch it fails even when the eventual tag would pass:

```bash
gh workflow run release.yml --ref stable/X.Y -f tag=vX.Y.0-beta.N
sleep 10   # the new run is not listed at once; the filters keep an older tag run out
gh run watch "$(gh run list --workflow=release.yml --event workflow_dispatch --branch stable/X.Y --limit 1 --json databaseId --jq '.[0].databaseId')" --exit-status
```

Then wait for the `Release` workflow. A stable tag runs only the text-only `release` job and
finishes in under a minute. A beta tag also runs `beta-deb`, which builds the `.deb` and attaches
it with its `.sha256` after the pre-release is already published; that takes several minutes, and
`gh run watch` waits for it:

```bash
gh run list --workflow=release.yml --limit 1
gh run watch "$(gh run list --workflow=release.yml --limit 1 --json databaseId --jq '.[0].databaseId')" --exit-status
gh release view vX.Y.Z --json name,body --jq '.name, (.body | .[0:400])'
```

The body must start with the plain-language notes, not the CHANGELOG bullets.

Beta mode: also confirm the assets before announcing the beta, because its notes tell testers to
download them:

```bash
gh release view vX.Y.0-beta.N --json assets --jq '.assets[].name'
# alpacabridge_X.Y.0-beta.N_arm64.deb
# alpacabridge_X.Y.0-beta.N_arm64.deb.sha256
```

If `beta-deb` failed, the pre-release stands with notes and no `.deb`: fix the cause, then re-run
the failed job (`gh run rerun <run-id> --failed`; the upload uses `--clobber`).

If the workflow
failed (tag/VERSION mismatch, an undated CHANGELOG section), fix the cause on a new PR, delete and re-push
the tag after it merges (`git tag -d vX.Y.Z && git push origin :refs/tags/vX.Y.Z`), and verify
again. If the workflow never ran, the tag landed on a commit without `release.yml`; create the
Release by hand with `gh release create vX.Y.Z --notes-file docs/releases/X.Y.Z.md --verify-tag`
(beta: `gh release create vX.Y.0-beta.N --prerelease --notes-file docs/releases/X.Y.0-beta.N.md
--verify-tag`; without `--prerelease` the beta would become the Latest release above the current
stable). A beta Release created by hand carries no `.deb`, and its notes point testers at one:
build it on a Debian trixie arm64 host and attach it under the tag spelling:

```bash
scripts/build_deb.sh
cp ../alpacabridge_X.Y.0~betaN_arm64.deb alpacabridge_X.Y.0-beta.N_arm64.deb
sha256sum alpacabridge_X.Y.0-beta.N_arm64.deb > alpacabridge_X.Y.0-beta.N_arm64.deb.sha256
gh release upload vX.Y.0-beta.N alpacabridge_X.Y.0-beta.N_arm64.deb alpacabridge_X.Y.0-beta.N_arm64.deb.sha256
```

## Step 7 — Wrap up

- Open the merge-down PR (Modes above) and run `/pr-checker` on it.
- Delete the local `release/X.Y.Z` branch (origin deletes the remote one on merge).
- Report: the version, the PR number, the tag, the Release URL, and the apt publish reminder
  (apt.openastro.net is published outside this repo; the Release is not the install channel).
- The next feature PR adds its own `changelog.d/` fragment; the next release collapses this
  section into `<details>`.
