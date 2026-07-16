---
description: Submit a pull request from the current feature branch to the upstream main branch
allowed-tools: Read, Edit, Bash, Grep, Glob
---

You are a PR submission assistant for the AlpacaBridge project. Your job is to ensure the branch is ready, build a well-formatted PR, and submit it — whether the user is a direct contributor or working from a fork.

## Step 1 — Safety checks

### Branch guard

```bash
git branch --show-current
```

- If the current branch is `main` or `master`, **STOP immediately**. Tell the user:
  > "You're on `main`. PRs must come from a feature branch. Create a branch first (e.g., `git checkout -b driver/vendor-device`) and commit your changes there."
- Do NOT proceed. Do NOT offer to create the branch automatically — the user may have uncommitted work or specific naming in mind.

### Uncommitted changes

```bash
git status
```

- If there are uncommitted changes, **STOP**. Tell the user:
  > "You have uncommitted changes. Use `/commit` to stage and commit before submitting a PR."
- Do NOT proceed until the working tree is clean.

### Unpushed commits

```bash
git log @{u}..HEAD --oneline 2>/dev/null || echo "NO_UPSTREAM"
```

- If there are unpushed commits or no upstream tracking branch, note this — the branch will need to be pushed in Step 5 (after the Step 4 local CI pre-flight).

### ConformU report validation (HARD BLOCK)

If this branch adds or modifies any ConformU files under `AlpacaCore/conformu/**`, every report must pass before the PR can be submitted. Merging a failing report misleads downstream consumers of `SUPPORTED-DRIVERS.md` into thinking a driver is validated on arm64.

Find the reports on this branch:

```bash
git diff --name-only main..HEAD | grep -E '^AlpacaCore/conformu/.*\.(json|txt)$'
```

If the list is empty, skip the rest of this subsection.

For each file in the list:

- **JSON reports** (`*.json`) — **fail** if any of `ErrorCount`, `IssueCount`, `TimingIssuesCount` is non-zero:
  ```bash
  jq -r '{ErrorCount, IssueCount, TimingIssuesCount}' <file>
  ```
- **Text logs** (`*.txt`) — **fail** if any of these are true:
  - `grep -E "OUTSIDE (FAST|STANDARD|EXTENDED) RESPONSE TIME TARGET" <file>` matches
  - `grep "took longer than its target response time" <file>` matches
  - The file does NOT contain `Congratulations, no errors, warnings or issues found`

If any file fails, **STOP**. Do NOT push. Do NOT open the PR. Tell the user exactly which file and which counts/lines failed:

> "ConformU report `<file>` shows Errors=N, Issues=N, TimingIssues=N (or matching grep lines). The driver is not validated. Fix the driver, re-run ConformU until clean, replace the report on this branch, and try again. PR is blocked until every ConformU report on this branch passes. See `/driver-build` Step 10 for the full pass criteria."

Do NOT offer to open the PR "anyway", as a draft, or with a TODO. This block exists because a green-looking PR with a failing ConformU report is the worst-case outcome — it gets merged and misadvertises the driver as validated.

## Step 2 — Detect repository setup

Determine whether the user is a direct contributor or working from a fork:

```bash
git remote -v
```

### Direct contributor (origin = open-astro/AlpacaBridge)

The `origin` remote points to `open-astro/AlpacaBridge`. PRs go directly to `main` on the same repo.

### Fork contributor (origin = user's fork)

The `origin` remote points to the user's fork (e.g., `github.com/username/AlpacaBridge`). Check if an `upstream` remote exists:

```bash
git remote -v | grep upstream
```

- If no `upstream` remote, tell the user to add one:
  > "Your origin is a fork but no `upstream` remote is configured. Add it with:
  > `git remote add upstream https://github.com/open-astro/AlpacaBridge.git`"
- PRs from forks target `open-astro/AlpacaBridge:main` as the base.

## Step 3 — Analyze the branch for PR content

Gather all changes on this branch relative to `main`:

```bash
git log main..HEAD --oneline
git diff main..HEAD --stat
git diff main..HEAD
```

Read the full diff and all commit messages. Understand:
- What was added, changed, or fixed
- Which components were touched (driver, HTTP, tests, docs, SDK, ConformU)
- Whether this is a new driver, bug fix, feature enhancement, or documentation update

### Pre-submission checklist — warn if any are missing

Review the branch contents and warn the user about anything that's missing:

- [ ] **Unit tests**: Does the branch include Catch2 tests? (required for all driver code)
- [ ] **ConformU results**: If this is a driver PR, is an arm64 ConformU report included AND clean (verified in Step 1 — errors=0, issues=0, timing issues=0)?
- [ ] **CHANGELOG.md**: Is there an entry under `## [x.x.x] - UNRELEASED`, and does the version match the **Versioning policy** below for everything on this branch?
- [ ] **SUPPORTED-DRIVERS.md**: If this adds or validates a driver, is the table updated?
- [ ] **AGENTS.md**: Were lessons learned captured?
- [ ] **AGPL license headers**: Do new source files have the license header?
- [ ] **SDK cleanup**: If SDK files were added under `external/`, have Windows/macOS/32-bit/demo files been removed?

Present the checklist to the user with pass/fail status. If critical items are missing (tests, CHANGELOG), recommend fixing before submitting but let the user decide.

### Verify the UNRELEASED version (Versioning policy)

Look at everything this branch adds/changes (from the diff above) and confirm the
`## [x.x.x] - UNRELEASED` heading in `CHANGELOG.md` reflects the **highest-severity** change.
AlpacaBridge is an end-user appliance, so "breaking" means breaks an existing user's install/
setup. Bump relative to the last **released** version:

- **MAJOR** `x.0.0` — breaks an existing user (drop a platform, remove a driver, config-format
  change needing migration, change a default that alters behavior).
- **MINOR** `x.Y.0` — new backward-compatible capability: **a new driver**, new device/model
  support, a new optional feature/flag. Resets patch to 0.
- **PATCH** `x.y.Z` — no new capability: bug fix to an existing driver, ConformU re-validation,
  packaging fix, docs/skill/spec changes.

A branch that adds a new driver MUST be a minor bump, never a patch. If the UNRELEASED heading
undershoots (e.g. it says `2.0.1` but the branch adds a driver, so it should be `2.1.0`), flag
it and recommend running `/commit` to correct the heading before opening the PR — don't open a
PR with a version that misrepresents the change.

### Release version bump (ask the user — MANDATORY, every run)

Most PRs leave the version as `UNRELEASED` and the actual release is cut separately. On **every**
run of this skill, before pushing, ask the user whether this PR is cutting the release — never
skip or assume the answer:

> "Is this PR cutting the `<UNRELEASED version>` release? If so I can update the `VERSION` file
> and the `README.md` version badge to `<UNRELEASED version>` (and date the CHANGELOG entry) so
> they're ready for release. Otherwise I'll leave everything as UNRELEASED."

- **If NO** (default for feature / driver / fix PRs) — leave the `VERSION` file, the `README.md`
  badge, and the CHANGELOG `UNRELEASED` heading untouched. Proceed to Step 4.
- **If YES** — finalize the version (the `[x.x.x]` from the CHANGELOG UNRELEASED heading):
  1. Write the bare version (e.g. `2.1.0`) into the `VERSION` file — `printf '%s\n' <version> > VERSION`.
  2. Update the README badge line `#### [x.x.x] - YYYY-MM-DD &middot; [Changelog](CHANGELOG.md)`
     to the new version and **today's date**.
  3. Change the CHANGELOG heading `## [x.x.x] - UNRELEASED` to `## [x.x.x] - YYYY-MM-DD` (today),
     so `VERSION`, the README badge, and the CHANGELOG agree.
  4. These are now uncommitted changes (Step 1 required a clean tree). Show the user the diff and
     a commit message (e.g. `Release <version>`) for approval, commit them on this branch
     following the project's commit conventions, then continue to the Step 4 pre-flight and push.

## Step 4 — Local CI pre-flight (HARD BLOCK)

CI runs a strict set of gates on every PR (`.github/workflows/ci.yml`). Reproduce them **locally before pushing** so the PR never opens red and wastes a CI cycle. This is automated by `scripts/ci_preflight.sh`:

```bash
./scripts/ci_preflight.sh
```

The script reproduces, in order, the CI jobs that can run on this arm64 host and prints a `[PASS]/[FAIL]/[SKIP]` summary:

1. **clang-format** changed lines (CI `format`)
2. **Unicode / Trojan-Source** scan (CI `unicode`)
3. **Build + unit tests, vendors OFF** — `run_all_tests.sh` (CI `build-test`)
4. **Build + unit tests, vendors ON** — `run_all_tests.sh` (CI `build-vendors`)
5. **clang-tidy** changed lines (CI `clang-tidy`)
6. **cppcheck** changed files (CI `cppcheck`)
7. **shellcheck** — only if shell scripts changed (CI `shellcheck`)
8. **javascript** — `node --check` of web UI JS, only if `AlpacaHTTP/web/*.js` changed (CI `javascript`)
9. **zizmor** — only if `.github/workflows/*` changed (CI `zizmor`)

It **auto-installs** every missing tool so each gate actually runs rather than being skipped: `clang-tidy`/`cppcheck`/`shellcheck`/`clang-format`/`nodejs` via `sudo apt-get`, and `zizmor` as a pinned, checksum-verified release binary cached under `~/.cache` (no sudo). The two `run_all_tests.sh` invocations are full rebuilds and are the slow part — that's expected.

Knobs:
- `PREFLIGHT_BASE=upstream/main ./scripts/ci_preflight.sh` — fork contributors whose PR base is the upstream remote.
- `RUN_SANITIZERS=1 ./scripts/ci_preflight.sh` — also reproduce the ASan+UBSan `sanitizers` job (a third rebuild). Recommended when the branch changes C++ runtime logic; skip for docs/CI-only changes.
- `PREFLIGHT_NO_INSTALL=1 ./scripts/ci_preflight.sh` — never apt-install; missing tools are reported `[SKIP]` instead.

**Gate:** the script exits non-zero if any mandatory check failed. If it does, **STOP** — do not push, do not open the PR. Report the failing check(s) to the user and let them fix it, then re-run. A `[SKIP]` only appears when a check is not applicable (no matching files changed) or `PREFLIGHT_NO_INSTALL=1` left a tool uninstalled — in the latter case, surface it so the user knows CI will still enforce that gate.

## Step 5 — Push the branch

Only after the Step 4 pre-flight is green. If the branch has unpushed commits or no upstream tracking:

```bash
git push -u origin <branch-name>
```

Confirm the push succeeded before proceeding.

## Step 6 — Build the PR

### PR title

- Under 70 characters
- Same verb-first convention as commit messages: Add, Fix, Update, Implement, Validate
- Include vendor/device when relevant
- Examples:
  - `Add ToupTek camera driver with HTTP/UI integration`
  - `Fix iOptron HEM27 Wi-Fi pulse guide timing`
  - `Validate SynScan HEQ5 PRO ConformU 4.3.0 on arm64`

### PR body

Build the body from the branch's commits and diffs. Use this structure:

```markdown
## Summary
- Bullet points summarizing what this PR does (1-4 bullets)
- Include vendor, device model, and key technical details
- Reference ConformU results if applicable (e.g., "0 errors, 0 issues on arm64")

## Changes
Group by component using bold tags:
- **Vendor Device Driver** (AlpacaCore): what was added/changed
- **Vendor Device Support** (AlpacaHTTP): router, web UI changes
- **Vendor Unit Tests**: test count and assertion count
- **Vendor SDK**: version and location
- **ConformU Validation**: platforms tested, results
- **Documentation**: CHANGELOG, SUPPORTED-DRIVERS.md, AGENTS.md updates

## Test plan
- [ ] Local CI pre-flight green: `run_all_tests.sh` (vendors OFF + ON), clang-format, unicode scan, and (when installed) clang-tidy/cppcheck
- [ ] Unit tests pass (`cd build && ctest`)
- [ ] ConformU 4.3.0 passes on Linux arm64
- [ ] Web UI configuration works in browser
- [ ] Device connects and operates correctly
(Include only items relevant to this PR)

## ConformU results
(If applicable — link to the report files in the branch)
- **arm64**: `AlpacaCore/conformu/Vendor/Model/arm64/`
```

### Present for approval

Show the user the full PR title and body before submitting. Ask for approval or edits.

## Step 7 — Submit the PR

### Direct contributor

```bash
gh pr create --base main --title "<title>" --body "$(cat <<'EOF'
<PR body here>
EOF
)"
```

### Fork contributor

```bash
gh pr create --repo open-astro/AlpacaBridge --base main --head <username>:<branch> --title "<title>" --body "$(cat <<'EOF'
<PR body here>
EOF
)"
```

After submission, display the PR URL to the user.

## Step 8 — Watch for the review bot (poll every minute) — MAINTAINER ONLY

**The automated review bot only runs for the maintainer (@joeytroy).** External/fork contributors
must not execute against the bot. Determine which flow applies from the Step 2 repo detection and
the authenticated user (`gh api user --jq .login`):

- **Maintainer** (direct contributor, login `joeytroy`): follow this step and Step 9 as written.
- **External/fork contributor** (anyone else): do NOT poll for a bot review — none will come.
  Instead, after creating the PR, post a comment tagging the maintainer so they can kick off a
  local agent review:

  ```bash
  gh pr comment <number> --body "@joeytroy this PR is ready for review — please kick off a local agent review when you have a chance."
  ```

  Then skip Step 9 entirely (verdicts, fixes-per-round, merging, and follow-up issues are the
  maintainer's side) and go to Step 10. Remind the user the maintainer will review and respond
  on the PR.

Every PR gets an automated review from the `claude` bot (`.github/workflows/claude-review.yml`).
It posts a PR comment ending in a verdict line: `✅ Approved` or `⚠️ Issues found`. After creating
the PR (and after **every** push, which restarts a full fresh review), watch for the next bot
comment.

Record the baseline count of bot comments, then start a background poll that exits when a new
one arrives (do NOT foreground-sleep; run this with `run_in_background`):

```bash
PR=<number>
BASE=$(gh pr view "$PR" --json comments --jq '[.comments[] | select(.author.login=="claude")] | length')
while :; do
  sleep 60
  N=$(gh pr view "$PR" --json comments --jq '[.comments[] | select(.author.login=="claude")] | length')
  [ "$N" -gt "$BASE" ] && break
done
gh pr view "$PR" --json comments --jq '[.comments[] | select(.author.login=="claude")] | last | .body'
```

While waiting, also keep an eye on CI: `gh pr checks <number>`. A red CI check should be fixed
(and pushed) without waiting for the review verdict.

## Step 9 — Act on the review verdict

Read the bot's newest review comment in full and classify every finding as **in-scope** (a real
defect in this PR's changes) or **out-of-scope** (pre-existing, non-blocking, or beyond this PR's
purpose). Findings under a "Notes (no action needed)" heading need no action unless clearly wrong.

### Verdict: `⚠️ Issues found`

1. Fix each **in-scope** finding on the branch. Batch ALL fixes into ONE commit/push — every push
   restarts a full fresh review (PR #99 took 46 rounds; don't trickle pushes).
2. For each **out-of-scope** finding, open a follow-up issue instead (format below).
3. Show the user the fixes and the planned push for approval, push once, then return to Step 8
   and poll for the fresh review.

### Verdict: `✅ Approved`

**Do NOT push anything further to this branch — approval is the stopping point.** Any remaining
or newly-noticed items (including "approved, non-blocking" findings in the review itself) become
follow-up issues, not commits. Then ask the user for approval to merge; on yes:

```bash
gh pr merge <number> --merge
```

### Follow-up issue format

Match the established pattern (e.g. issues #135–#137 from PR #134). One issue per finding:

```bash
gh issue create --title "<component>: <concise defect summary>" --body "$(cat <<'EOF'
From the PR #<N> review (approved, non-blocking): <full technical description of the finding,
including file/function references and the suggested fix direction from the review comment>.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

Show the user each issue title/body for approval before creating.

## Step 10 — Wrap-up

- Display the PR URL, final verdict, and any follow-up issues opened
- If AGENTS.md wasn't updated: "Consider updating AGENTS.md with any lessons learned from this work."
