### Fixed

- **Review bot: a decision-record verdict followed by a period counts** (CI, `scripts/check_adr_scope.py`): the checklist gate now accepts `- <record> rule <n>: pass. <reason>` as well as `pass: <reason>`. Before this, a review that wrote the period was refused as having no checklist lines and never posted, even when its verdict was Approved. This stalled PR #2315 across three review runs. A verdict with no reason after it still does not count. Covered by two new `--self-test` expectations.
