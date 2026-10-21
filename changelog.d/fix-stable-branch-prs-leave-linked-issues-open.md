### Fixed (tooling)

- **Issues named by a merged `stable/**` PR are now closed** (CI, issue #983): `.github/workflows/close-linked-issues.yml` runs `scripts/close_linked_issues.py` for a merged PR into `stable/**` and closes the issues its body names with a closing keyword; GitHub does this only for PRs into `main`. The parser is covered by `--self-test`, which CI runs.
