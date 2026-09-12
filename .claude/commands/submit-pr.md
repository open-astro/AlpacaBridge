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

If this branch adds or modifies any ConformU files under `AlpacaCore/conformu/**` or `AlpacaHTTP/conformu/**`, every report must pass before the PR can be submitted. Merging a failing report misleads downstream consumers of `SUPPORTED-DRIVERS.md` (or `AlpacaHTTP/conformu/README.md`'s own compliance claim) into thinking a driver or endpoint is validated.

This is checked here so a bad report never reaches CI in the first place, but it is **not** only a prompt-level rule anymore: the `conformu-reports` CI job runs the identical check (`scripts/check_conformu_reports.py`) on every PR and blocks the merge regardless of how the PR was opened. Run it directly instead of doing this by hand:

```bash
python3 scripts/check_conformu_reports.py main
```

If it reports "nothing to check", skip the rest of this subsection. Otherwise it prints exactly which file and which counts/lines failed — the pass criteria are:

- **JSON reports** (`*.json`) — fail if any of `ErrorCount`, `IssueCount`, `TimingIssuesCount` is non-zero.
- **Text logs** (`*.txt`) — fail if any of these are true:
  - a line matches `OUTSIDE (FAST|STANDARD|EXTENDED) RESPONSE TIME TARGET`
  - a line matches `took longer than its target response time`
  - the file does NOT contain any of the known ConformU success phrasings (see `SUCCESS_PATTERNS` in the script — there are two, since the per-device and protocol-level ConformU checks word it differently)

If it fails, **STOP**. Do NOT push. Do NOT open the PR. Tell the user exactly which file and which counts/lines failed:

> "ConformU report `<file>` shows Errors=N, Issues=N, TimingIssues=N (or matching lines). The driver is not validated. Fix the driver, re-run ConformU until clean, replace the report on this branch, and try again. PR is blocked until every ConformU report on this branch passes. See `/driver-build` Step 10 for the full pass criteria."

Do NOT offer to open the PR "anyway", as a draft, or with a TODO. This block exists because a green-looking PR with a failing ConformU report is the worst-case outcome — it gets merged and misadvertises the driver as validated. Catching it here just saves a CI cycle; the CI job is now the actual backstop.

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
  5. Remind the user that after the PR merges they tag the merge commit
     (`git tag -a v<version> -m "Release <version>" && git push origin v<version>`); the
     `Release` workflow then creates the GitHub Release from the CHANGELOG section. See
     "Releases" in `docs/development.md`.

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
  - `Validate SynScan HEQ5 PRO on arm64 (ConformU <version used>)`

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
- [ ] ConformU (latest release, but NOT arm64 4.5.0 — see `/conformu` step 2g) passes on Linux arm64
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

## Step 8 — Watch for the review bot (poll every three minutes)

The bot (`.github/workflows/claude-review.yml`) runs automatically on every push to a **same-repo**
PR branch. On a **fork** PR it runs only once a maintainer applies the `safe-to-review` label
(`pull_request_target`, gated per-PR — see AGENTS.md § "Review bot on fork PRs" for why). It is
NOT limited to any one maintainer's own PRs; a fork contributor listed in `allowed_non_write_users`
in that workflow gets reviewed the same way once labeled. Determine which flow applies from the
Step 2 repo detection:

- **Same-repo PR** (origin = `open-astro/AlpacaBridge`): the bot is already running. Follow this
  step and Step 9 as written — poll immediately.
- **Fork PR**: check whether `safe-to-review` is already on the PR:

  ```bash
  gh pr view <number> --json labels --jq '.labels[].name'
  ```

  - **Label present**: the bot is running (or has already posted). Follow this step and Step 9 as
    written.
  - **Label absent**: do NOT poll yet — nothing will happen until a maintainer applies the label.
    Post a comment asking for it instead of asking someone to review by hand:

    ```bash
    gh pr comment <number> --body "This PR is from a fork — could a maintainer apply the \`safe-to-review\` label so the review bot runs?"
    ```

    Then go to Step 10 and tell the user the PR is waiting on that label; once it's applied, resume
    this step with the real poll (not the `BUDGET=0` pre-check: the label re-triggers the bot on
    the same head, see the exit-code notes below).

Every PR gets an automated review (`.github/workflows/claude-review.yml`), posted by the
workflow's own step as `github-actions`. It ends in a verdict line: `✅ Approved` or
`⚠️ Issues found`. After creating the PR (and after **every** push, which restarts a full fresh
review), watch for the next bot comment.

First check whether a verdict for the current head is already there (a fast review, or any delay
between the push and starting the poll). "For the current head" means: the newest `review` check-run on the
head SHA that was not cancelled or skipped has completed (a failed run counts: the assert step can
fail after the verdict was posted), none is still queued or running, and the newest bot verdict
was updated after that run started. Commit dates are not used: a commit's committer date is when it
was made locally, so a verdict on the previous head can be newer than it. The block below applies that
rule every three minutes (`TICK`) within a 30-minute budget, first look after one tick so a
run just re-triggered on the same head has appeared; `BUDGET=0` gives a single immediate look
when nothing was just re-triggered (do NOT foreground-sleep; run
it with `run_in_background`). It is the same block as `/pr-checker` Step 2:

```bash
PR=<number>; TICK=${TICK:-180}; DEADLINE=$(( $(date +%s) + ${BUDGET:-1800} ))
# A re-trigger on the same head (relabel, update-branch) takes a few seconds to show a
# new check-run; a first look inside that window classifies the OLD run as final. So a
# real poll sleeps one tick before it looks. BUDGET=0 is the one-pass pre-check, which
# is only valid when nothing was just re-triggered, and looks at once.
if [ "${BUDGET:-1800}" -gt 0 ]; then sleep "$TICK"; fi
while :; do
  # Bind the verdict to the review check-run on the head SHA (filter=all so a later
  # cancelled attempt cannot hide the finished one). Everything is fetched with
  # --paginate: unpaginated, both endpoints return only the 30 oldest items.
  SHA=$(gh api "repos/open-astro/AlpacaBridge/pulls/$PR" --jq .head.sha)
  if [ "$SHA" != "${LAST_SHA:-}" ]; then CANCELLED_SEEN=0; LAST_SHA=$SHA; fi   # new head, new latch
  RUNS=$(gh api --paginate "repos/open-astro/AlpacaBridge/commits/$SHA/check-runs?per_page=100&filter=all" | jq -s 'map(.check_runs[]) | map(select(.name == "review"))')
  # Newest finished run that was not cancelled or skipped. A failed run still counts:
  # the assert step can fail after the post step published the verdict, and nothing
  # re-runs a failed run on its own, so it must be reported, not waited out.
  DONE=$(jq -r '[.[] | select(.status == "completed" and (.conclusion | IN("cancelled","skipped") | not))] | max_by(.started_at) | select(. != null) | "\(.started_at) \(.conclusion) \(.html_url)"' <<<"$RUNS")
  RUN_STARTED=${DONE%% *}; RUN_STATE=${DONE#* }
  PENDING=$(jq -r '[.[] | select(.status == "queued" or .status == "in_progress")] | length' <<<"$RUNS")
  SKIPPED=$(jq -r '[.[] | select(.conclusion == "skipped")] | length' <<<"$RUNS")
  CANCELLED=$(jq -r '[.[] | select(.conclusion == "cancelled")] | length' <<<"$RUNS")
  # REST, not `gh pr view --json comments`: only REST exposes updated_at. The author
  # pattern is the workflow's own assert-step pattern. `select(. != null)` matters: with
  # no verdict yet, `last` is null and would otherwise print the literal "null".
  LAST=$(gh api --paginate "repos/open-astro/AlpacaBridge/issues/$PR/comments?per_page=100" | jq -r -s 'add // [] | [.[] | select((.user.login | test("^(claude|github-actions)(\\[bot\\])?$")) and (.body | test("✅ Approved|⚠️ Issues found")))] | last | select(. != null) | "\(.updated_at) \(.body)"')
  if [ -n "$RUN_STARTED" ] && [ "$PENDING" = 0 ] && [ -n "$LAST" ] \
     && [ "$(date -u -d "${LAST%% *}" +%s)" -ge "$(date -u -d "$RUN_STARTED" +%s)" ]; then
    printf '%s\n' "${LAST#* }"; exit 0
  fi
  if [ -n "$RUN_STARTED" ] && [ "$PENDING" = 0 ]; then
    # Finished, nothing pending, no verdict for this head. Never wait 30 minutes for it.
    case "$RUN_STATE" in
      success*)
        # The PR edits the review workflow and the action skipped it (the assert step
        # passes it). Hand back for eyeball review; this is a hard stop, never a merge.
        if gh pr diff "$PR" --name-only | grep -qxF ".github/workflows/claude-review.yml"; then
          echo "NO VERDICT for head $SHA: this PR edits the review workflow and the action skipped it. Review it by eye." >&2; exit 2
        fi
        # Green run, ordinary PR, no verdict: the agent wrote its verdict to chat instead of
        # review-comment.md (the PR #209 failure). Nothing re-runs it; report, do not wait.
        echo "REVIEW RUN succeeded for head $SHA but published no verdict: ${RUN_STATE#* }" >&2; exit 3 ;;
      *)
        echo "REVIEW RUN ENDED ${RUN_STATE%% *} for head $SHA with no verdict: ${RUN_STATE#* }" >&2; exit 3 ;;
    esac
  fi
  # Only skipped runs and nothing pending: the actor gate skipped the job (fork push
  # without the label, or an author outside allowed_non_write_users). A cancelled run
  # beside the skipped one means a labelled run existed and was re-triggered mid-poll;
  # that is the two-look cancelled case below, not a skip. see the fork-label rule above.
  if [ -z "$RUN_STARTED" ] && [ "$PENDING" = 0 ] && [ "$SKIPPED" != 0 ] && [ "$CANCELLED" = 0 ]; then
    echo "REVIEW SKIPPED for head $SHA: no run to wait for. Apply or re-apply safe-to-review (see above)." >&2; exit 3
  fi
  # Only cancelled runs: a superseding trigger never came. A relabel issued mid-poll
  # cancels the in-flight run a few seconds before its replacement appears, so this
  # state has to hold on two consecutive looks before it is reported.
  if [ -z "$RUN_STARTED" ] && [ "$PENDING" = 0 ] && [ "$CANCELLED" != 0 ]; then
    if [ "${CANCELLED_SEEN:-0}" = 1 ]; then
      echo "REVIEW CANCELLED for head $SHA and nothing replaced it: re-trigger with the relabel trick." >&2; exit 3
    fi
    CANCELLED_SEEN=1
  else
    CANCELLED_SEEN=0
  fi
  [ "$(date +%s)" -ge "$DEADLINE" ] && break
  sleep "$TICK"
done
echo "NO VERDICT for head ${SHA:-?} after $(( ${BUDGET:-1800} / 60 )) min (review runs pending: ${PENDING:-?}, newest finished: ${RUN_STATE:-none})" >&2; exit 1
```

**Exit `1` from a `BUDGET=0` pre-check is not a timeout.** Right after `gh pr create` or a push
there is no `review` check-run yet, so the single look finds nothing and exits `1` with "after
0 min": that means **no verdict yet**, start the real poll. Only an exit `1` from the full
30-minute budget is a stall. **Skip the pre-check when the bot was just re-triggered on the same
head** (the maintainer just applied `safe-to-review`, or you just ran `update-branch`): for a
few seconds the only check-run is the old skipped or cancelled one, and the pre-check would
report it as final. Start the real poll instead; its first look comes after one tick.
Exit `2` (the action skipped a workflow-editing PR) and exit `3` (no run will produce a
verdict: the newest run failed, succeeded without publishing one, the job was skipped, or every run was cancelled) both
mean there is no verdict to act on: tell the user, and for exit `3` read the run log first. If the poll times out, surface
the stall to the user and check the workflow (`gh run list --workflow=claude-review.yml --limit 3`)
instead of restarting the loop blindly. `date -d` is GNU; the skills run on the Linux dev VM.

While waiting, also keep an eye on CI: `gh pr checks <number>`. A red CI check should be fixed
(and pushed) without waiting for the review verdict.

## Step 9 — Act on the review verdict

Read the bot's newest review comment in full. It has two sections (`claude-review.yml` prompt):
**Defects**, which are what made the verdict a rejection, and **Notes**, which never block. Only
Defects are work in this PR. A Note is a follow-up issue only when the user wants one; a Note the
bot marks "out of scope, open an issue" is listed for the user in Step 10, never fixed here.

### Verdict: `⚠️ Issues found`

1. Fix each **Defect** on the branch. Batch ALL fixes into ONE commit/push — every push restarts a
   full fresh review (PR #99 took 46 rounds; don't trickle pushes). A Note is never a reason for
   a push on its own; a mechanical Note (a comment fix, an unused include, a missing test for a
   string this PR added) may ride along in the same commit.
2. A Defect that would need a wrong or unsafe change, or a product decision the user has not
   made, goes back to the user instead of being fixed on faith.
3. Show the user the fixes and the planned push for approval, push once, then return to Step 8
   and poll for the fresh review.

### Verdict: `✅ Approved`

**Do NOT push anything further to this branch — approval is the stopping point.** The Notes an
approval carries are listed in Step 10; open a follow-up issue for one only when the user asks.
Then ask the user for approval to merge; on yes:

```bash
gh pr merge <number> --merge
```

### Follow-up issue format

Match the established pattern (e.g. issues #135–#137 from PR #134). One issue per finding:

```bash
gh issue create --title "<component>: <concise defect summary>" --body "$(cat <<'EOF'
From the PR #<N> review (a Note, non-blocking): <full technical description of the finding,
including file/function references and the suggested fix direction from the review comment>.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

Show the user each issue title/body for approval before creating.

## Step 10 — Wrap-up

- Display the PR URL, final verdict, the Notes the final review carried, and any follow-up issues opened
- If AGENTS.md wasn't updated: "Consider updating AGENTS.md with any lessons learned from this work."
