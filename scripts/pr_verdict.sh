#!/bin/bash
# pr_verdict.sh -- read the review bot's verdict for a PR, bound to the PR head.
#
# Usage: scripts/pr_verdict.sh <PR-number> [--oneshot]
#   default   poll every 3 minutes for up to 30 minutes
#   --oneshot one pass, no sleep; "not ready" exits 1 with the reason
#
# Exit codes and stdout (first line "<updated_at> <verdict>", then the body):
#   0  "✅ Approved" or "⚠️ Issues found" for the CURRENT head
#   1  not ready (oneshot) or 30-minute timeout; the reason is on stderr
#   2  the API or a timestamp failed FAILS_MAX ticks in a row; reason on stderr
#   3  the newest bot comment has no readable sign-off (hard stop)
#
# Verdict contract (see scripts/tests/pr_verdict_test.sh, which pins it):
#   Fenced code blocks (``` or ~~~, to the closing fence) are dropped first:
#   a review that quotes a verdict on its own line inside a fence, as any
#   review of this script or the skill will, is not signing off. Parity
#   guard: when the fence markers do not balance (a quoted fence, a truncated
#   body) nothing is dropped, so the sign-off stays visible and a quoted
#   rejection costs a round rather than a false hard stop. Inline code stays: a line that starts with a backtick never
#   matches either rule. Remaining lines are normalised: emphasis (*_)
#   removed throughout, leading #, > markers and a "Verdict:" label stripped,
#   whitespace trimmed, and a line that is empty or only a --- rule dropped so
#   it does not consume the five-line tail. Matching is case-insensitive
#   ("Issues Found" counts). The emoji is optional, like the workflow's own
#   assert grep; VS16 is optional.
#   1. Any line ANYWHERE (bullet marker also stripped) that STARTS WITH
#      "Issues found" -> Issues found. Trailing text is allowed ("Issues found
#      (2 blockers)"). Anywhere, so a sign-off buried under a long footer
#      still rejects, and it beats any "Approved" in the tail because a false
#      approval merges while a false rejection costs a round.
#   2. else any line among the LAST FIVE that STARTS WITH "Approved" with the
#      bullet marker KEPT -> Approved ("Approved -- no significant issues" is
#      an approval; "- Approved the earlier fix, but ..." is a list item, so
#      a comment whose only "Approved" is such a bullet falls to rule 3).
#   3. else -> SIGN-OFF NOT IN LAST LINES (exit 3)
#   Prose that quotes a verdict mid-sentence never starts a line after
#   normalisation, so it never counts. A prose line like "Issues found last
#   round are all addressed." costs one review round; that is the accepted
#   direction.
#
# Head binding: the comment must be updated after the newest SUCCESSFUL
# `review` check-run on the PR head commit started (per-commit query with
# filter=all, since the default returns one run per name and a later
# cancelled attempt would hide the good one), and no `review` run may be
# queued or in progress. A run cancelled by concurrency or skipped for a
# missing label is "completed" too and posts nothing; a PR that edits the
# workflow itself gets a successful run with no comment (handled upstream by
# the skill's Step 1.0). Verified on same-repo and fork PR heads.
#
# The accepted comment author is the workflow's own assert-step pattern,
# ^(claude|github-actions)(\[bot\])?$, so a token change there cannot make
# CI pass while this poll rejects forever.
# Requires GNU coreutils (date -u -d), gh, jq. Comments are fetched with
# --paginate and merged locally (gh 2.46 has no --slurp); gh output is
# captured before jq so a mid-pagination failure is visible.
set -u
REPO=${REPO:-open-astro/AlpacaBridge}
PR=${1:?usage: pr_verdict.sh <PR-number> [--oneshot]}
case "$PR" in ''|*[!0-9]*) echo "pr_verdict.sh: PR must be a number, got '$PR'" >&2; exit 2;; esac
ONESHOT=0; [ "${2:-}" = "--oneshot" ] && ONESHOT=1
TICK=${TICK:-180}; DEADLINE=$(( $(date +%s) + ${BUDGET:-1800} )); FAILS_MAX=${FAILS_MAX:-5}
fails=0; reject=""   # counts consecutive API and timestamp failures (either kind); a tick that reaches
                     # the gates, including "no comment yet", resets it, since the API is evidently fine

# shellcheck disable=SC2016  # jq program: $lines/$tail/$verdict are jq variables
VERDICT_JQ='[.[] | select((.user.login | test("^(claude|github-actions)(\\[bot\\])?$"))
                          and (.body | gsub("[*#>_-]"; "") | test("(✅️? *)?Approved|(⚠️? *)?Issues +found"; "i")))]
            | last | select(. != null)
            | (.body | split("\n")
               | . as $raw
               | reduce .[] as $l ({fence: false, out: []};          # drop fenced code: a quoted verdict is not a sign-off
                   if ($l | test("^\\s*(```|~~~)")) then .fence |= not
                   elif .fence then . else .out += [$l] end)
               | if .fence then $raw else .out end                  # parity guard: an odd fence count would hide the sign-off, so fall back to every line
               | map(select(test("\\S")))
               | map(gsub("[*_]"; "") | sub("^[\\s#>]+"; "") | sub("^(?i)verdict:\\s*"; "") | sub("^[\\s#>]+"; "") | sub("\\s+$"; ""))
               | map(select(test("^[\\s-]*$") | not))) as $signoffs   # bullet marker kept ("- Approved the fix, but" is prose); a --- rule is dropped
            | ($signoffs | map(sub("^[\\s-]+"; "") | sub("^(?i)verdict:\\s*"; "") | sub("^[\\s#>-]+"; "")) | map(select(. != ""))) as $lines
            | ($signoffs | .[-5:]) as $tail
            | (if   ($lines | any(test("^(⚠️? *)?Issues +found"; "i")))    then "⚠️ Issues found"
               elif ($tail  | any(test("^(✅️? *)?Approved\\b"; "i")))      then "✅ Approved"
               else "SIGN-OFF NOT IN LAST LINES" end) as $verdict
            | "\(.updated_at) \($verdict)\n\(.body)"'

not_ready() {
  if [ "$ONESHOT" = 1 ]; then echo "NOT READY: $reject" >&2; exit 1; fi
  echo "$(date -u +%H:%M:%SZ) not ready: $reject" >&2   # per-tick trail, so a timeout is diagnosable from the log
}

while [ "$(date +%s)" -lt "$DEADLINE" ]; do
  [ "$ONESHOT" = 1 ] || sleep "$TICK"
  reject=""
  if ! raw=$(gh api --paginate "repos/$REPO/issues/$PR/comments?per_page=100") \
     || ! c=$(printf '%s' "$raw" | jq -r -s "add | $VERDICT_JQ") \
     || ! head_sha=$(gh api "repos/$REPO/pulls/$PR" --jq .head.sha) \
     || ! runs=$(gh api --paginate "repos/$REPO/commits/$head_sha/check-runs?per_page=100&filter=all") \
     || ! run_started=$(printf '%s' "$runs" | jq -r -s 'map(.check_runs[]) | [.[] | select(.name == "review" and .conclusion == "success") | .started_at] | max // empty') \
     || ! pending=$(printf '%s' "$runs" | jq -r -s 'map(.check_runs[]) | [.[] | select(.name == "review" and (.status == "queued" or .status == "in_progress"))] | length'); then
    fails=$((fails + 1)); reject="API/jq failure ($fails in a row)"
    echo "POLL: $reject for PR $PR (see above)" >&2
    [ "$fails" -ge "$FAILS_MAX" ] && exit 2
    not_ready; continue
  fi
  # a tick that reaches the gates resets the failure counter: here for "no comment", below once the timestamps parse
  if [ -z "$c" ]; then fails=0; reject="no bot comment yet"; not_ready; continue; fi
  v_epoch=$(date -u -d "${c%% *}" +%s 2>/dev/null)
  r_epoch=""; [ -n "$run_started" ] && r_epoch=$(date -u -d "$run_started" +%s 2>/dev/null)
  if [ -z "$v_epoch" ] || { [ -n "$run_started" ] && [ -z "$r_epoch" ]; }; then
    fails=$((fails + 1)); reject="unparseable timestamp (verdict '${c%% *}', run '$run_started'; $fails failures in a row)"
    echo "POLL: $reject" >&2
    [ "$fails" -ge "$FAILS_MAX" ] && exit 2
    not_ready; continue
  fi
  fails=0
  if [ "${pending:-0}" -gt 0 ]; then
    reject="a review run is still queued/in progress on head $head_sha"
  elif [ -z "$run_started" ]; then
    reject="no successful review check-run on head $head_sha yet"
  elif [ "$v_epoch" -lt "$r_epoch" ]; then
    reject="newest verdict (${c%% *}) predates the review run on head $head_sha (started $run_started)"
  else
    echo "$c"
    case "${c#* }" in "SIGN-OFF NOT IN LAST LINES"*) exit 3;; esac
    exit 0
  fi
  not_ready
done
# Recovery by rejection: "no bot comment yet" -> label / relabel (skill Step 1.1-1.2);
# "no successful review check-run" or "predates the review run" -> the run failed or was
# cancelled: relabel or `gh run rerun`; "still queued/in progress" -> poll again;
# "API/jq failure" / "unparseable" -> transient, poll again once.
echo "TIMEOUT after $(( ${BUDGET:-1800} / 60 )) minutes; last rejection: ${reject:-none}" >&2; exit 1
