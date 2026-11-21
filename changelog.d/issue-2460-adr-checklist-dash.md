### Fixed

- **An approved review with a dash after a checklist verdict is posted**, review workflow, issue #2460. `scripts/check_adr_scope.py` now accepts an en or em dash after `pass`, `defect` or `n/a` in a Decision-records checklist line (`rule 1: pass — reason`), as it already accepted a colon or a period, so the workflow's assert step no longer refuses to post an approved review over the separator. A verdict with no reason, or with no separator before its reason, still fails.
