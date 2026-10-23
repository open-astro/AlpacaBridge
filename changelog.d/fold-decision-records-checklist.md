### Changed (CI)

- **The review bot's decision-record checklist is folded** (CI): the comment keeps the `## Decision records` heading but puts the one-line-per-rule checklist in a collapsed `<details>` block whose summary gives the verdict counts, for example "45 rules: 17 pass, 28 n/a". The fold runs after `check_adr_scope.py --checklist` has passed on the unfolded file, the sign-off stays the last line outside the block, and a review with no checklist lines ("none in scope") is posted as written.
