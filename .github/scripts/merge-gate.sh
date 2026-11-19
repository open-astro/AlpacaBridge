#!/usr/bin/env bash
# Merge gate: a queued pull request merges only when every check on its head
# ran green (a skipped job does not count) and the review bot's latest verdict
# on that head is "✅ Approved". Branch protection requires only some checks;
# this one gate holds the rest.
#
# Usage: merge-gate.sh <owner/repo> <merge group head ref>
# The head ref is refs/heads/gh-readonly-queue/<base>/pr-<N>-<base sha>.
# Needs GH_TOKEN with checks, statuses and pull-requests read.
# scripts/tests/test_merge_gate.py runs it against a fake gh on PATH; every gh
# call returns raw JSON or text, and jq does the selecting here.
set -euo pipefail

repo="$1"
ref="$2"
wait_minutes="${MERGE_GATE_WAIT_MINUTES:-50}"

pr="$(sed -nE 's#.*/pr-([0-9]+)-[0-9a-f]+$#\1#p' <<<"$ref")"
if [ -z "$pr" ]; then
  echo "::error::No pull request number in the merge group ref ${ref}."
  exit 1
fi
read -r sha author < <(gh pr view "$pr" --repo "$repo" --json headRefOid,author | jq -r '"\(.headRefOid) \(.author.login)"')
echo "Pull request #${pr} by ${author}, head ${sha}."

# A check that is still running is waited for, not judged: the queue may start
# while a check no rule requires still runs on the head. filter=latest is per
# check suite, and a PR-body edit or a label starts a new suite, so the newest
# run of each name decides (by id: a queued run may have no started_at yet).
# Skipped runs are dropped first, on purpose: a skipped run did not run, so an
# older run of the same name still decides.
deadline=$(( $(date +%s) + wait_minutes * 60 ))
while :; do
  runs="$(gh api --paginate "repos/${repo}/commits/${sha}/check-runs?filter=latest&per_page=100" \
    | jq -s '[.[].check_runs[] | select(.conclusion != "skipped")] | group_by(.name) | map(max_by(.id))')"
  statuses="$(gh api "repos/${repo}/commits/${sha}/status" | jq '.statuses')"
  pending="$(jq -n --argjson r "$runs" --argjson s "$statuses" \
    '[($r[] | select(.status != "completed") | .name), ($s[] | select(.state == "pending") | .context)]')"
  if [ "$(jq length <<<"$pending")" = 0 ]; then
    break
  fi
  if [ "$(date +%s)" -ge "$deadline" ]; then
    echo "::error::These checks did not finish on ${sha} within ${wait_minutes} minutes: $(jq -r 'join("; ")' <<<"$pending")"
    exit 1
  fi
  echo "Waiting for: $(jq -r 'join("; ")' <<<"$pending")"
  sleep 30
done

bad="$(jq -n -r --argjson r "$runs" --argjson s "$statuses" \
  '($r[] | select(.conclusion != "success" and .conclusion != "neutral") | "\(.name): \(.conclusion)"),
   ($s[] | select(.state != "success") | "\(.context): \(.state)")')"
if [ -n "$bad" ]; then
  echo "::error::Not every check is green on ${sha}:"
  echo "$bad"
  exit 1
fi
echo "Every check that ran on ${sha} is green."

# Dependabot bumps get no bot review; the maintainer reviews them by eye.
if [[ "$author" == *dependabot* ]]; then
  echo "A Dependabot pull request: no bot review to read."
  exit 0
fi

# review-fork is Ara's fork-PR review job; AlpacaBridge has only review.
review="$(jq -c '[.[] | select(.name == "review" or .name == "review-fork")] | max_by(.id)' <<<"$runs")"
if [ "$review" = null ]; then
  echo "::error::No review ran on ${sha}."
  exit 1
fi
# Only a bot verdict written during that review run counts, so a verdict on an
# earlier head, or a human writing "Approved", cannot pass the gate. The
# sign-off is the comment's last line, so a comment that only quotes it is not
# a verdict.
since="$(jq -r '.started_at' <<<"$review")"
verdict="$(gh api --paginate "repos/${repo}/issues/${pr}/comments?per_page=100" \
  | jq -s -r --arg since "$since" \
    '[.[][] | select(.user.login | test("^(claude|github-actions)(\\[bot\\])?$"))
            | select(.updated_at >= $since)
            | (.body | split("\n") | map(select(test("\\S"))) | last // "") as $last
            | select($last | test("^\\s*(✅ Approved|⚠️ Issues found)\\s*$"))
            | {updated_at, verdict: $last}]
     | max_by(.updated_at) | .verdict // ""')"
if [ -z "$verdict" ]; then
  # The review action skips a PR that edits its own workflow, and the review
  # job passes such a PR without a comment; the maintainer reviews it by eye.
  if gh pr diff "$pr" --repo "$repo" --name-only | grep -qxF ".github/workflows/claude-review.yml"; then
    echo "::warning::This PR edits the review workflow and got no review comment. Review it by eye."
    exit 0
  fi
  echo "::error::The review on ${sha} posted no verdict."
  exit 1
fi
if ! grep -qF "✅ Approved" <<<"$verdict"; then
  echo "::error::The latest review on ${sha} is not \"✅ Approved\"."
  exit 1
fi
echo "The latest review on ${sha} is \"✅ Approved\"."
