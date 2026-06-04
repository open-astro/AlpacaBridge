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
- [ ] **CHANGELOG.md**: Is there an entry under `## [x.x.x] - UNRELEASED`?
- [ ] **SUPPORTED-DRIVERS.md**: If this adds or validates a driver, is the table updated?
- [ ] **AGENTS.md**: Were lessons learned captured?
- [ ] **SSPL license headers**: Do new source files have the license header?
- [ ] **SDK cleanup**: If SDK files were added under `external/`, have Windows/macOS/32-bit/demo files been removed?

Present the checklist to the user with pass/fail status. If critical items are missing (tests, CHANGELOG), recommend fixing before submitting but let the user decide.

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

## Step 8 — Post-submission

After the PR is created:
- Display the PR URL
- Remind the user: "Watch for CI checks and reviewer feedback on the PR."
- If AGENTS.md wasn't updated: "Consider updating AGENTS.md with any lessons learned from this work."

Do NOT merge the PR automatically — that's up to the maintainers.
