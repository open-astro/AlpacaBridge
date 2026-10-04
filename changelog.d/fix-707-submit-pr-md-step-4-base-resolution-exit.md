### Fixed (docs)
- **`/submit-pr` Step 4 now says what to report when pre-flight cannot resolve its diff base** (`.claude/commands/submit-pr.md`): `ci_preflight.sh` can exit non-zero before any check runs, so the agent reports that error and the base it tried instead of a failing check.
