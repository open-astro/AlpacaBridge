### Added

- **`Falsified by:` PR-body gate** (CI, `scripts/check_falsified_by.py`, `.github/workflows/pr-body.yml`): every test case a PR adds or renames must be named in the PR body with the production-code mutation that fails it. The `falsified-by` job re-runs when the body is edited, a pre-flight gate reads `PR_BODY_FILE`, and `/submit-pr` drafts the section. Not a required check.
