#!/bin/bash
# Pins the verdict contract and the head-binding gates of scripts/pr_verdict.sh
# against a stub `gh`. No network. Run: scripts/tests/pr_verdict_test.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd); SCRIPT=$HERE/../pr_verdict.sh
STUB=$(mktemp -d); trap 'rm -rf "$STUB"' EXIT
cat > "$STUB/gh" <<'STUBEOF'
#!/bin/bash
# Scenario via $SCEN, comment body via $BODY, comment time via $UPD. `date` is real.
case "$*" in
  *"/comments"*) [ "$SCEN" = nocomment ] && { echo '[]'; exit 0; }
     printf '[{"user":{"login":"github-actions[bot]"},"updated_at":"%s","body":%s}]' "${UPD:-2026-09-10T10:00:00Z}" "$(printf '%s' "$BODY" | jq -Rs .)";;
  *"/pulls/"*)   echo "abc123";;
  *"/check-runs"*) case "$SCEN" in
     two_runs)        echo '{"check_runs":[{"name":"review","status":"completed","conclusion":"success","started_at":"2026-09-10T09:00:00Z"},{"name":"review","status":"completed","conclusion":"success","started_at":"2026-09-10T09:30:00Z"}]}';;
     cancelled_after) echo '{"check_runs":[{"name":"review","status":"completed","conclusion":"success","started_at":"2026-09-10T09:00:00Z"},{"name":"review","status":"completed","conclusion":"cancelled","started_at":"2026-09-10T09:45:00Z"}]}';;
     pending)         echo '{"check_runs":[{"name":"review","status":"completed","conclusion":"success","started_at":"2026-09-10T09:00:00Z"},{"name":"review","status":"in_progress","conclusion":null,"started_at":"2026-09-10T10:05:00Z"}]}';;
     none)            echo '{"check_runs":[]}';;
     *)               echo '{"check_runs":[{"name":"review","status":"completed","conclusion":"success","started_at":"2026-09-10T09:30:00Z"}]}';;
     esac;;
  *) exit 1;;
esac
STUBEOF
chmod +x "$STUB/gh"
pass=0; fail=0
check() {  # check <name> <scen> <upd> <body-printf> <expect-rc> <expect-first-line-substring>
  local out rc
  local body; body=$(printf '%b' "$4")
  out=$( PATH="$STUB:$PATH" SCEN=$2 UPD=$3 BODY="$body" bash "$SCRIPT" 282 --oneshot 2>&1 ); rc=$?; out=${out%%$'\n'*}
  if [ "$rc" = "$5" ] && [[ "$out" == *"$6"* ]]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL $1: rc=$rc want $5; out='$out' want *'$6'*"; fi
}
A='✅ Approved'; I='⚠️ Issues found'
check "bare approval"                 ok 2026-09-10T10:00:00Z "review\n$A"                                   0 "$A"
check "approval with note"            ok 2026-09-10T10:00:00Z "review\n$A -- no significant issues"          0 "$A"
check "bold no-emoji issues"          ok 2026-09-10T10:00:00Z "review\n**Issues found**"                     0 "$I"
check "issues with suffix"            ok 2026-09-10T10:00:00Z "review\n$I (2 blockers)"                      0 "$I"
check "verdict label, heading, quote" ok 2026-09-10T10:00:00Z "r\n> **Verdict:** ### $I"                     0 "$I"
check "VS16 on check mark"            ok 2026-09-10T10:00:00Z "r\n✅️ Approved."                              0 "$A"
check "approval then rule and footer" ok 2026-09-10T10:00:00Z "r\n$A\n---\nfooter\nlink"                     0 "$A"
check "issues buried under footer"    ok 2026-09-10T10:00:00Z "r\n$I\n---\nf1\nf2\nf3\nf4\nf5"               0 "$I"
check "mixed tail is a rejection"     ok 2026-09-10T10:00:00Z "r\n$I\nApproved once the null check is added." 0 "$I"
check "quoted approval in prose"      ok 2026-09-10T10:00:00Z "the $A section says\nend\n1\n2\n3\n4\n5"       3 "SIGN-OFF NOT IN LAST LINES"
check "approval buried under footer"  ok 2026-09-10T10:00:00Z "r\n$A\n---\nf1\nf2\nf3\nf4\nf5"               3 "SIGN-OFF NOT IN LAST LINES"
check "capitalised sign-off"          ok 2026-09-10T10:00:00Z "review\n⚠️ Issues Found"                    0 "$I"
check "caps issues over bullet approved" ok 2026-09-10T10:00:00Z "r\n- Approved the earlier fix, but the new one regresses X.\n⚠️ ISSUES FOUND" 0 "$I"
check "rule and four footer lines"    ok 2026-09-10T10:00:00Z "r\n$A\n---\nf1\nf2\nf3\nf4"              0 "$A"
check "buried issues beats bullet approved" ok 2026-09-10T10:00:00Z "$I\nf1\nf2\nf3\nf4\n- Approved the earlier fix, but see above." 0 "$I"
check "buried suffixed issues"        ok 2026-09-10T10:00:00Z "$I (2 blockers)\nf1\nf2\nf3\nf4\nf5"       0 "$I"
check "bullet approved is not a sign-off" ok 2026-09-10T10:00:00Z "r\n- Approved the earlier fix, but the new one regresses X.\nend"  3 "SIGN-OFF NOT IN LAST LINES"
check "heading approval"              ok 2026-09-10T10:00:00Z "r\n### $A"                                    0 "$A"
check "bold approval"                 ok 2026-09-10T10:00:00Z "r\n**$A**"                                    0 "$A"
check "fenced quoted issues, then approval" ok 2026-09-10T10:00:00Z "r\n\`\`\`\n$I\n\`\`\`\n$A"          0 "$A"
check "fenced approval is not a sign-off" ok 2026-09-10T10:00:00Z "r\n\`\`\`\n$A\n\`\`\`\nend"           3 "SIGN-OFF NOT IN LAST LINES"
check "fenced with language tag"      ok 2026-09-10T10:00:00Z "r\n\`\`\`text\n$I\n\`\`\`\n$A"           0 "$A"
check "unterminated fence swallows the rest" ok 2026-09-10T10:00:00Z "r\n\`\`\`\n$A"                       3 "SIGN-OFF NOT IN LAST LINES"
check "inline-code quoted issues"     ok 2026-09-10T10:00:00Z "- \`$I\` means reject\n$A"                  0 "$A"
check "CRLF body"                     ok 2026-09-10T10:00:00Z "r\r\n$A\r\n"                                   0 "$A"
check "no comment yet"                nocomment 2026-09-10T10:00:00Z "x"                                    1 "NOT READY: no bot comment yet"
check "verdict predates run"          ok 2026-09-10T08:00:00Z "r\n$A"                                       1 "predates the review run"
check "verdict between two runs"      two_runs 2026-09-10T09:10:00Z "r\n$A"                                 1 "predates the review run"
check "success then cancelled run"    cancelled_after 2026-09-10T10:00:00Z "r\n$A"                          0 "$A"
check "re-review pending"             pending 2026-09-10T10:00:00Z "r\n$A"                                  1 "queued/in progress"
check "no successful run"             none 2026-09-10T10:00:00Z "r\n$A"                                     1 "no successful review check-run"
# gh failing: oneshot exits 1 on the first failure; the loop exits 2 after FAILS_MAX
sed 's/gh api/gh-none api/g' "$SCRIPT" > "$STUB/broken.sh"
out=$( PATH="$STUB:$PATH" SCEN=ok BODY=x bash "$STUB/broken.sh" 282 --oneshot 2>&1 ); rc=$?; out=${out%%$'\n'*}
if [ "$rc" = 1 ] && [[ "$out" == *"gh-none"* || "$out" == *"API/jq failure"* ]]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL gh failure oneshot: rc=$rc out='$out'"; fi
out=$( PATH="$STUB:$PATH" SCEN=ok BODY=x TICK=0 BUDGET=5 FAILS_MAX=3 bash "$STUB/broken.sh" 282 2>&1 ); rc=$?; out=${out##*$'\n'}
if [ "$rc" = 2 ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL gh failure loop: rc=$rc out='$out'"; fi
# a persistently unparseable timestamp reaches exit 2 through the shared counter
out=$( PATH="$STUB:$PATH" SCEN=ok UPD="not-a-date" BODY="r\n$A" TICK=0 BUDGET=5 FAILS_MAX=3 bash "$SCRIPT" 282 2>&1 ); rc=$?
if [ "$rc" = 2 ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL unparseable timestamp loop: rc=$rc out='${out##*$'\n'}'"; fi
# the workflow's own author pattern: a comment from claude[bot] is accepted
sed 's/github-actions\[bot\]/claude[bot]/' "$STUB/gh" > "$STUB/gh2" && chmod +x "$STUB/gh2" && mkdir -p "$STUB/alt" && mv "$STUB/gh2" "$STUB/alt/gh"
out=$( PATH="$STUB/alt:$PATH" SCEN=ok UPD=2026-09-10T10:00:00Z BODY="r"$'\n'"$A" bash "$SCRIPT" 282 --oneshot 2>&1 ); rc=$?; out=${out%%$'\n'*}
if [ "$rc" = 0 ] && [[ "$out" == *"$A"* ]]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL claude[bot] author: rc=$rc out='$out'"; fi
# a non-numeric PR argument fails at once, not after 15 minutes of 404s
out=$( bash "$SCRIPT" 28x --oneshot 2>&1 ); rc=$?
if [ "$rc" = 2 ] && [[ "$out" == *"must be a number"* ]]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL non-numeric PR: rc=$rc out='$out'"; fi
echo "pr_verdict_test: $pass passed, $fail failed"; [ "$fail" -eq 0 ]
