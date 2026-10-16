### Fixed (tooling)

- **The `Falsified by:` gate no longer flags cases other PRs added after this one opened** (CI, `scripts/check_falsified_by.py`, issue #992). The `falsified-by` job now diffs against the first parent of the checked-out merge commit instead of `pull_request.base.sha`; `--self-test` builds a scratch repo whose base gains a case after the PR branched and shows the old choice flags it and the new one does not, and that a HEAD with no second parent falls back to the merge-base default.
