### Fixed

- **An approved review whose checklist keeps the `VERDICT:` placeholder is posted**, review workflow, issue #2592. `scripts/check_adr_scope.py` now accepts an optional `VERDICT:` (any case) between `rule N:` and the verdict in a Decision-records checklist line (`rule 3: VERDICT: pass — reason`), the skeleton's placeholder word the review agent sometimes keeps, so the workflow's checklist step no longer throws away an approved review over it. A `VERDICT:` with no verdict, or a verdict with no reason, still fails.
