### Changed (docs)
- **AGENTS.md split into shared topic files** (agent instructions): AGENTS.md had grown back from 100k to 144k bytes since the #523 split and is loaded whole into every agent session, so its cross-vendor sections now live verbatim in seven files under `.github/instructions/` (driver-lifecycle, ascom-contract, serial-autodetect, testing, ci-gates, packaging-release, http-integration), each with a `.claude/rules/` adapter and a row in `docs/agent-instructions.md`; AGENTS.md keeps the core rules, a routing table and one-line summaries of the rules every change follows (11.4k bytes). Links into the moved sections are repointed.

### Added (tooling)
- **AGENTS.md size budget** (`scripts/check_docs_drift.py` check 21): the docs-drift job fails when AGENTS.md exceeds 15,000 bytes. Check 18 now reads the CI job roster from the ci-gates instructions, and check 17 reads error-code pins from the ascom-contract and testing instructions as well.
