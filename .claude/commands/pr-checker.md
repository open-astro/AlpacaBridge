---
description: Drive one or more open PRs through the review-bot loop until each is clean, then merge them in order; keeps looping until every PR is done
allowed-tools: Read, Edit, Write, Bash, Grep, Glob
---

You are the PR checker for the AlpacaBridge project. The user gives you one or more PR numbers
(`/pr-checker 257 258 259`, `/pr-checker 257-259`, or `/pr-checker` alone to mean every open PR).
For each PR, in ascending number order, loop **fix -> push -> poll the review bot** until the bot
posts a clean verdict, then merge it, then move to the next PR. Do not stop early, do not hand a
half-finished PR back, and do not ask "shall I continue?" between rounds. The only stopping points
are: every listed PR is merged (or closed), or a PR is blocked on something only the user can
decide (see **Hard stops**).

## Step 0 — Resolve the PR list

```bash
gh pr list --state open --json number,title,author,headRefName,headRepositoryOwner,isDraft,labels \
  --jq '.[] | "\(.number) \(.title) | \(.author.login) \(.headRepositoryOwner.login):\(.headRefName) draft=\(.isDraft) labels=\([.labels[].name]|join(","))"'
```

Expand ranges (`257-259` -> 257 258 259). Skip numbers that are not open PRs and say so. Record
for each PR: author, head owner/branch, whether it is a **fork PR** (head owner != `open-astro`),
whether it is a draft, and whether it carries the `safe-to-review` label.

**Validate every contributor-controlled string before it touches a shell command.** Branch
names and fork owners come from the PR author and can contain anything git allows. Refuse (hard
stop for that PR) any `headRefName` that does not match `^[A-Za-z0-9][A-Za-z0-9._/-]*$` **and**
contain no `..` (check both: the regex alone lets `foo..bar` through), and any
`headRepositoryOwner.login` that does not match `^[A-Za-z0-9](?:-?[A-Za-z0-9])*$`
(GitHub login rules: alphanumerics with single inner hyphens only, so no leading, trailing or
consecutive hyphen, and an owner can never become a bare `-x` argument). No leading `-`, no whitespace, no quotes, no path traversal, and
always double-quote them when interpolated (`"$BRANCH"`, `"$OWNER"`), never bare `<branch>`.
PR numbers must match `^[0-9]+$`. Never `eval` or build a command from a PR title or body.

Print the queue once, then work it top to bottom.

## Step 1 — Per PR: make sure the bot is actually going to run

The review bot (`.github/workflows/claude-review.yml`) posts a comment as the GitHub Actions
bot, ending in `✅ Approved` or `⚠️ Issues found`. `gh pr view --json comments` reports that
author as `github-actions` while the REST API reports `github-actions[bot]`, so match both. It only runs when:

- the PR author has write access, **or** the PR carries the `safe-to-review` label (fork PRs), and
- the author is in `allowed_non_write_users` for pushes the author makes themselves.

Checks to make before waiting on anything:

1. **`review` check skipped / no verdict comment and no `safe-to-review` label** -> add the label
   via REST (`gh pr edit --add-label` can choke on a GraphQL projects warning):
   ```bash
   gh api -X POST repos/open-astro/AlpacaBridge/issues/<N>/labels -f 'labels[]=safe-to-review'
   ```
   The `labeled` event starts a fresh review immediately.
2. **`review` check failed in ~15 s with "Actor does not have write permissions"** -> the author's
   own push could not run the bot. Remove and re-add the label via REST to re-run it as the
   maintainer:
   ```bash
   gh api -X DELETE repos/open-astro/AlpacaBridge/issues/<N>/labels/safe-to-review
   gh api -X POST   repos/open-astro/AlpacaBridge/issues/<N>/labels -f 'labels[]=safe-to-review'
   ```
3. **Branch is behind main** (`gh api "repos/open-astro/AlpacaBridge/compare/main...$OWNER:$BRANCH" --jq .behind_by`
   is non-zero; `$OWNER`/`$BRANCH` validated in Step 0). Branch protection is strict, so it must be updated before it can merge, and
   updating re-runs CI + the bot. Do this **now** rather than after the verdict so you do not pay
   for two bot rounds:
   ```bash
   gh api -X PUT repos/open-astro/AlpacaBridge/pulls/<N>/update-branch
   ```
   **Check the result before polling for a verdict**: `update-branch` returns HTTP 422 on a
   merge conflict, and a chain that ignores that then waits 30 minutes for a verdict that never
   comes (PRs #270 and #272 both stalled this way on 2026-09-10). GitHub recomputes
   mergeability asynchronously, so the first read after the call is usually `UNKNOWN`; poll
   until it settles and treat a timeout as a conflict, never as "fine":
   ```bash
   for i in $(seq 1 12); do   # up to 2 min
     m=$(gh pr view <N> --json mergeable --jq .mergeable)   # MERGEABLE | CONFLICTING | UNKNOWN
     [ "$m" != "UNKNOWN" ] && break; sleep 10
   done
   echo "$m"
   ```
   `CONFLICTING` (or still `UNKNOWN` after the loop) -> resolve locally on the head branch now
   (Step 3 mechanics: fetch the fork, `git merge origin/main`, keep both sides when two PRs
   added adjacent CI jobs or gates, validate syntax, push), then poll. `MERGEABLE` -> Step 2.
   It is fine to update while a review is still in flight: the run on the old head is cancelled
   and a fresh one starts on the merged head, so nothing is lost.
4. **Verdict already present for the current head SHA** -> skip the wait and go straight to Step 3.
   Verify the verdict belongs to the current head: the bot comment's REST `updated_at` (not
   `createdAt`; the workflow's sticky comment may be edited in place) is newer than the last
   commit (`gh api repos/open-astro/AlpacaBridge/pulls/<N>/commits --jq '.[-1].commit.committer.date'`).
   A verdict older than the head commit is stale and must not be trusted.

## Step 2 — Poll for the verdict (3-minute cadence, background)

Never foreground-sleep. Run this with `run_in_background` and a 30-minute deadline:

```bash
PR=<N>
DEADLINE=$(( $(date +%s) + 1800 ))
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
  sleep 180
  # REST, not `gh pr view --json comments`: the workflow uses a sticky comment
  # (use_sticky_comment: true), which may be EDITED in place on later rounds,
  # and only REST exposes updated_at. createdAt alone would call every round
  # after the first "stale".
  # Verdict = the final non-empty line of the body (verified on every bot
  # comment on PRs #279-#282). A review may quote "✅ Approved" or "⚠️ Issues
  # found" in its prose while discussing this file, so neither the first nor
  # the last global match is safe; the sign-off line is. Guards: no comment
  # yet -> no output (select(. != null)); CRLF body -> \r stripped; a last
  # line that is not a verdict -> "SIGN-OFF NOT LAST LINE: ..." so it can
  # never be mistaken for one. Probed: empty array, CRLF fixture, footer
  # after the sign-off, and the live comments.
  c=$(gh api "repos/open-astro/AlpacaBridge/issues/$PR/comments" --jq '[.[]
        | select((.user.login | test("^github-actions(\\[bot\\])?$"))
                 and (.body | test("✅ Approved|⚠️ Issues found")))]
        | last | select(. != null)
        | (.body | split("\n") | map(select(test("\\S"))) | last | sub("\r$"; "")) as $line
        | "\(.updated_at) \(if ($line | test("^(✅ Approved|⚠️ Issues found)$")) then $line
                              else "SIGN-OFF NOT LAST LINE: " + $line end)\n\(.body)"' 2>/dev/null || true)
  head_at=$(gh api "repos/open-astro/AlpacaBridge/pulls/$PR/commits" --jq '.[-1].commit.committer.date')
  # Only a verdict UPDATED after the head commit counts: anything older is a
  # stale verdict on a previous head (or the contributor pushed mid-round).
  # First output line is "<updated_at> <verdict>"; the body follows. A
  # "SIGN-OFF NOT LAST LINE" first line also exits 0: treat it as a hard stop
  # for that PR, never as a verdict.
  if [[ "${c%% *}" > "$head_at" ]]; then echo "$c"; exit 0; fi
done
echo "TIMEOUT: no review-bot comment within 30 minutes" >&2; exit 1
```

When several PRs are queued, poll them all in one background loop and act on whichever verdict
lands first, but **merge strictly in ascending number order** so the update-branch dance is
predictable.

If it times out: `gh run list --workflow=claude-review.yml --limit 5` and read the failing job.
Known stalls: the workflow triggers only on `opened`, `synchronize` and `labeled` (closing and
reopening the PR does NOT re-run it), so a stuck or cancelled run is restarted with the
remove-and-re-add `safe-to-review` label trick from Step 1.2, which also covers the permission
skip. Do not restart the poll blindly.

While waiting, watch CI too (`gh pr checks <N>`). A red CI check gets fixed and pushed in the same
batch as the bot findings, not on its own.

## Step 3 — Act on the verdict

Read the newest bot comment in full. The verdict is the **final non-empty line** of the body,
never a string matched elsewhere in it: reviews quote `✅ Approved` / `⚠️ Issues found` in prose
when discussing this file.

### `⚠️ Issues found`

Fix **every** finding the bot raises on this PR, in this PR, one commit per finding, in **one
batched push**. Each push
restarts a full fresh review (PR #99 took 46 rounds when pushes trickled). Do not defer findings to
follow-up issues and do not decline them as low priority unless the user says so; the standing
rule is "work it in the same PR till there are no more issues."

**Before writing a line**, fetch the fork head: contributors watch the same bot and often push
their own fix for the same finding within minutes (PR #258 did this twice in one session).
If their head already moved past the reviewed SHA, read their diff first; if it addresses the
finding, adopt it (reset your local branch to their head) and just poll again.

### Prove it before you push (test-first, per finding)

The bot is the second pair of eyes, not the test suite. PR #281 (2026-09-10) took six rounds
because two of my "fixes" were pushed unproven: a regex that silently dropped 66 of 83 matches,
and a `--find-renames` flag that changed nothing. Both would have failed a 30-second probe.
For **every** finding, in this order:

1. **Reproduce the claim.** Re-read the bot's exact statement and make it observable before
   changing anything: run the failing input through the current code, count the matches, run
   the script against the real file, or write the unit test and watch it fail. If it cannot be
   reproduced, that is the finding to answer (hard stop or a wrong-claim note in the commit
   message), not a reason to change code on faith.
2. **Fix, then re-run the same probe.** Red, then green, with the same input. A fix that was
   never red is not proven.
3. **Ship the probe with the fix** whenever it can live in the repo: a Catch2 case for driver
   code (the SynScan `Name` test over the fake handset, PR #281), a self-check inside a script,
   or a synthetic-repo check described in the commit message when the probe cannot be committed
   (`git init` in a temp dir, one rename, run the script).
4. **Fix the structure before adding guards.** When a finding exposes brittle structure, fix
   the cause first, inside the PR's own files (in `check_docs_drift.py` the cause was matching
   spans across a fenced block; stripping fences fixed it). Keep a guard only when it catches
   something distinct from the structural fix, and say so in its comment (the parity check and
   count floor there guard against a stray backtick and a broken matcher, which fence-stripping
   does not cover). Never widen to files the PR does not touch.
5. **Run the exact CI gate for what changed**, not the whole pre-flight and not nothing. Use
   the same invocation and pass criterion as `scripts/ci_preflight.sh`, which mirrors
   `.github/workflows/ci.yml`:
   - C/C++: `git-clang-format --commit "$(git merge-base origin/main HEAD)" --diff --extensions c,cc,cpp,cxx,h,hh,hpp,hxx`
     is green only when it prints exactly `clang-format did not modify any files` or
     `no modified files to format` (the exit code is not the signal). Never omit
     `--extensions`: the default list includes `js`, and `AlpacaHTTP/web/app.js` is tracked.
     Base on `origin/main` deliberately: the pre-flight defaults to the local `main`
     (`PREFLIGHT_BASE`), which is stale in a long session; `PREFLIGHT_BASE=origin/main` makes
     the two agree.
   - `scripts/*.py`: run the script itself against the real repo, plus its own probe.
   - shell: `shellcheck <file>`. Workflows: `zizmor --offline .github/workflows/`, resolving
     the binary the way `ensure_zizmor()` in `ci_preflight.sh` does: `command -v zizmor` if
     present, else the pinned copy at
     `${XDG_CACHE_HOME:-$HOME/.cache}/alpacabridge-preflight/zizmor-<ZIZMOR_VER>` (the
     pre-flight downloads it on first use).
   - driver code: rebuild with the vendor compiled in, then run the tagged suite:
     `cmake -S AlpacaCore -B AlpacaCore/build -DALPACACORE_ENABLE_ALL_VENDORS=ON && cmake --build AlpacaCore/build --target alpacacore_tests`
     then `AlpacaCore/build/tests/alpacacore_tests "[vendor][device]"`. Vendors OFF (the
     `run_all_tests.sh` first pass) compiles no driver, so the tag filter would match nothing
     and pass vacuously.
   The full `ci_preflight.sh` is for branches that change runtime C++ across vendors.
6. **One commit per finding, one push per round** (this `⚠️ Issues found` path only). Commits
   stay atomic so a wrong one can be reverted alone; the push stays batched because every push
   costs a full review. A cleanup round after an approval is different: its notes are small and
   related, so they go in ONE commit as the `✅ Approved` section says.

Mechanics for a **fork PR** (the usual case for contributor branches):

```bash
# $REMOTE is a local remote name you chose (e.g. `diego`), $BRANCH the validated head name.
git fetch "$REMOTE" "$BRANCH"
git checkout -B "$BRANCH" "$REMOTE/$BRANCH"
# ... apply fixes ...
# Pre-flight then push then poll as ONE background chain (see "Keep looping").
# Skip the pre-flight step entirely for docs/skill-only branches.
./scripts/ci_preflight.sh > "$LOG" 2>&1 \
  && git fetch "$REMOTE" "$BRANCH" \
  && [ -z "$(git log --oneline "HEAD..$REMOTE/$BRANCH")" ] \
  && git push "$REMOTE" "HEAD:$BRANCH" \
  && <Step 2 poll loop>
```

Summary lines in `$LOG` are indented (`  [PASS] ...`). A red pre-flight is a hard block ONLY
for failures in code this branch touches; see "Keep looping" for the flake rule. A non-empty
`git log HEAD..$REMOTE/$BRANCH` means the contributor pushed meanwhile: read their diff and
rebase or adopt before pushing.

If the fork remote does not exist, add it with the validated owner:
`git remote add "$REMOTE" "https://github.com/$OWNER/AlpacaBridge.git"`.
If the contributor already pushed an equivalent fix while you were working, **adopt theirs** and
drop your duplicate instead of force-pushing. Never force-push a contributor's branch. Adopting
is not a rubber stamp: read their **entire** diff against the previously reviewed head (not
just the hunk that addresses the finding) and confirm it contains nothing beyond that fix before
resetting onto it; anything unrelated goes back to the bot as a normal push and review round.

For an `open-astro` branch, the same flow against `origin`.

Commit message: verb-first title under 70 chars, body explaining what the bot found and how it was
fixed, then the attribution trailer from the session. After pushing, go back to Step 1 (the push
may need the relabel trick again if it lands as the contributor) and Step 2.

**Never post PR comments replying to the bot.** A finding is either a change to the code, skill
or docs (push it), or, if it is clearly wrong and nothing can be changed to satisfy it, a hard
stop for the user to rule on. Explanations belong in the commit message, not in the PR thread.
Keep a running tally of rounds per PR and report it in the wrap-up.

### `✅ Approved`

**Cleanup rounds until clean, then merge.** Approvals usually carry "minor / non-blocking" notes.
Leaving them is how leftovers accumulate (nine PRs on 2026-09-10 left six: dead includes, a
regex edge case, a missing rename flag, an untested name suffix). Handle them like this:

1. Classify each note. **Mechanical** = a change a reviewer would accept without discussion and
   that stays inside the PR's files and purpose: unused include, missing test for a string the
   PR added, regex edge case, a missing `--find-renames`, a comment fix. **Judgment** = changes a
   default or behaviour, widens scope, needs hardware, or contradicts the PR author's stated
   intent. Judgment notes are listed in the wrap-up for the user, never pushed.
2. Fix **all** mechanical notes in ONE commit, push once, poll again.
3. Repeat step 2 for every approval that still carries mechanical notes, until an approval has
   **none**, then merge. **Hard cap: 3 cleanup rounds per PR.** After the third, merge on the
   next approval whatever notes it carries and list them in the wrap-up. PR #99 (2026-07-01)
   took 46 rounds because post-approval pushes were unbounded and trickled one nit at a time;
   the cap keeps that closed while normal PRs come out fully clean.
4. `⚠️ Issues found` on a cleanup round is handled like any other round: fix, push, poll.
   Counting against the cap: a round whose findings are genuine defects does **not** count
   (it is a fix round, not a cleanup round). A round whose findings are only nits **does**
   count as one cleanup round.
5. If the approval has **no** mechanical notes, skip straight to the merge below.

Then:

```bash
gh pr view <N> --json isDraft,mergeable,mergeStateStatus --jq '"draft=\(.isDraft) mergeable=\(.mergeable) state=\(.mergeStateStatus)"'
```

- `draft=true` -> `gh pr ready <N>` first (contributors often open drafts; `gh pr merge` refuses them).
- `state=BEHIND` -> `update-branch` (Step 1.3) and go back to Step 2; the merge commit re-runs the bot.
- `state=BLOCKED` with checks still running -> wait for `gh pr checks <N> --watch`, then merge.
- otherwise merge:
  ```bash
  gh pr merge <N> --merge
  ```
  (merge commit, not squash, matching the repo history). Confirm `state=MERGED` afterwards.
  The repo has `delete_branch_on_merge` enabled (2026-09-10), so an `origin` head branch is
  deleted by GitHub on merge; verify with `git fetch --prune origin && git branch -r`. A fork
  head branch belongs to the contributor and is never deleted from here.

Because the user invoked `/pr-checker` with the instruction to merge once the bot is clean, that
invocation **is** the merge authorization for every PR in the list. Do not ask again per PR.
This is the maintainer's deliberate policy for this repository (stated 2026-09-09 when the skill
was commissioned: "merge and close once the bot says there are no outstanding issues"), not a
convenience default: the review bot plus the full CI matrix is the review gate, and the
maintainer runs this skill themself, interactively, so a human is in the loop at invocation time
and can interrupt at any round. The guardrails that keep it safe are the ones above: every fork
input validated, every finding fixed in-PR rather than waived, every adopted contributor diff
read in full, and the **Hard stops** below, which override this authorization.

After a merge, every remaining PR in the queue is now behind main: run Step 1.3 on the **next** PR
right away so its refresh round starts while you tidy up.

**Contributor pushes during the loop are read, not just merged.** Whenever a fork head moves
between the verdict you acted on and the merge, diff it against the last reviewed head
(`git diff <reviewed-sha>..<new-head> --stat` and the hunks) and put a one-line summary per
commit in the wrap-up. The bot re-reviews them, but the maintainer should know what landed
beyond the PR as opened (PR #272 gained an unrelated cppcheck-scoping commit mid-run on
2026-09-10).

## Keep looping: what is NOT a reason to stop

The loop ends only when every PR is merged or a **Hard stop** below applies. In particular:

- **A pre-flight failure in code this branch does not touch** is not a stop. Re-run the failed
  test in isolation 5 times against the built binary (`AlpacaCore/build/tests/alpacacore_tests
  "<test name>"`). If it passes in isolation and `git diff main...HEAD --name-only` shows no
  file that could affect it, it is a flake: re-run `ci_preflight.sh` once, push on green, and
  record the flake (test name, failure text, pass rate) in the wrap-up for the user. Two
  consecutive flakes on the same test still push if the isolated runs pass. Only a failure in
  code this branch changes, or a test that fails in isolation every time, blocks the push.
- **A docs/skill-only branch** (no `.cpp`/`.h`/`.js`/`.sh`/workflow changes; check with
  `git diff main...HEAD --name-only`) does NOT run `ci_preflight.sh` at all: there is nothing
  for the build and test gates to check, and CI runs them on the PR anyway. Commit, push, poll.
- **A bot round with new findings** is the normal case, not a reason to report back. Fix,
  pre-flight, push, poll, repeat. Report only in the wrap-up, or when a hard stop is hit.
- **Waiting is never a stopping point.** Every wait (pre-flight, verdict poll, CI checks,
  update-branch) runs as ONE background chain that continues into the next action on its own:
  `preflight && push && poll` for a fix round, `update-branch && poll && merge` for a refresh.
  Never end the turn with "I'll push when pre-flight finishes"; chain it.
- **A bot finding you disagree with** is still fixed or wired into the skill/docs when there is
  any reasonable change that satisfies it. Only a finding that would require a wrong or unsafe
  change becomes a hard stop.

## Hard stops (the only reasons to hand back to the user)

- A PR's head branch name or fork owner fails the Step 0 validation, or a contributor's diff
  contains changes outside the reviewed finding that you cannot vouch for.
- The bot finding requires a product decision (change a default, drop a platform, alter a
  user-facing behaviour) that the PR author did not intend.
- A ConformU report on the branch is failing (a driver PR cannot merge with a red report; see
  `/submit-pr` Step 1).
- Merge conflicts that cannot be resolved without choosing between two contributors' intents.
- The review workflow itself is broken (two consecutive timeouts after the relabel
  tricks) — report the run URL.

State the blocker in one or two sentences, finish every other PR in the list, and say exactly
which PR was left and why.

## Wrap-up

Before the report, prune what the loop created locally: `git checkout main && git pull
--ff-only`, `git worktree remove <path>` for any worktree first (a branch checked out in a
worktree cannot be deleted), then `git branch -d <branch>` for every branch checked out during
the run (`-d` refuses anything unmerged, which is the point).
Confirm `git branch -r` on origin shows no merged head branches left behind.

One table: PR, title, rounds, final verdict, merge SHA (or "left open: reason"). Under it: any
judgment notes left unpushed, any notes from the post-cleanup approval, and any contributor
commits that landed mid-run, one line each.

**Retrospective, one line per PR:** which bot findings were about code pushed earlier in the
same loop (a fix that introduced the next finding), and what probe would have caught each
before the push. If the answer repeats across PRs, the fix belongs in this skill's "Prove it
before you push" list or in `AGENTS.md`, in the same session.

**Hand-off:** a single line naming anything the next session should know (e.g. an
`update-branch` still running on a
PR outside the list). Update memory only if the loop mechanics themselves changed (new bot login,
new label, new stall trick); the per-PR outcome does not belong in memory.
