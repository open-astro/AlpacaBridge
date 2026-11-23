### Fixed

- **Review bot: a decision-record checklist line that keeps the skeleton's `VERDICT:` word still counts.** The review prompt's skeleton line reads `rule <n>: VERDICT: reason`. When the bot filled it in as `rule 3: VERDICT: pass. ...`, `scripts/check_adr_scope.py` found no verdict, and the approved review was never posted. This happened three runs in a row on #2590. The checklist pattern now accepts an optional `VERDICT:` before `pass`, `defect` or `n/a`. A line still needs a real verdict and a reason.
