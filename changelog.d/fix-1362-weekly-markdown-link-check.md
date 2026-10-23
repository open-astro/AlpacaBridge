### Added

- **Weekly Markdown link check**, CI, issue #1362. `.github/workflows/link-check.yml` runs lychee every Monday and on manual dispatch: relative links offline, then `http(s)` links (`--accept 200,206,429`, results cached for 14 days). Broken links are listed in the job summary, and `scheduled-failure.yml` now watches the workflow, so a failing run opens one tracked issue that the next green run closes. Hosts that cannot be checked are in `.lycheeignore`, each with a reason. Dead links in `AlpacaCore/README.md` and `AlpacaCore/external/README.md` now point at `docs/`.
