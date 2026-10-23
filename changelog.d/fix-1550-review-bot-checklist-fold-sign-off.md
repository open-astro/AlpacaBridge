### Fixed

- **Review-bot checklist fold keeps only a real sign-off outside the fold**, CI, issue #1550. The "Fold the decision-record checklist" step now keeps the last line outside `<details>` only when it is `✅ Approved` or `⚠️ Issues found` (with the emphasis the gate accepts); a final checklist line goes inside the fold and is counted. Covered by the `fold_checklist` cases in `scripts/check_adr_scope.py --self-test`.
