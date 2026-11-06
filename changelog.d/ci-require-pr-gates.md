### Changed (tooling)
- **PR-body and decision-record gates can be required on `main`** (CI, `.github/workflows/pr-body.yml`, `.github/workflows/decision-records.yml`, `.github/workflows/adr-merge-queue.yml`): these checks now report in the merge queue, so the ruleset can require them without stalling the queue.
