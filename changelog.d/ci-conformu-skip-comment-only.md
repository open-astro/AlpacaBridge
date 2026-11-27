### Changed (CI)

- **The fake ConformU run skips comment-only C/C++ edits** (CI, issue #2778): the `runs` job of `conformu-fake.yml` no longer selects a row for a changed C/C++ file whose old and new text differ only in comments or blank lines (`comment_only_change` in `scripts/check_contract_sweep.py`). A one-line comment edit in `AlpacaCore/tests/contract_sweep.h` ran all 33 rows (about 102 runner-minutes) on #2777. The nightly run still runs every row.
