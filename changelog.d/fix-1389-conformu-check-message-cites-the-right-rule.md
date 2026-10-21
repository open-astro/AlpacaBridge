### Fixed

- **The ConformU report check names the place that states its rule** (`scripts/check_conformu_reports.py`, issue #1389): a failing report now points at `/submit-pr` Step 1 (ConformU report validation) and `/driver-build` Step 10 (pass criteria), where it used to cite `AGENTS.md`, which does not state the rule.
