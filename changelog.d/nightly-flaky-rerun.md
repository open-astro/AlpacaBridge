### Fixed (tooling)

- **Nightly test jobs no longer fail on a scheduler race** (`run_all_tests.sh`, `nightly-build.yml`). With `CTEST_RERUN_FAILED=1`, which both nightly test jobs now set, the cases that failed in the `-j 2*nproc` pool run once more alone and serially, and that rerun is the verdict. Five of the last seven nightly ASan + UBSan runs went red on a different timing-dependent case each time, after three per-case fixes; a sanitizer finding still fails alone. PR CI keeps the single oversubscribed run.
