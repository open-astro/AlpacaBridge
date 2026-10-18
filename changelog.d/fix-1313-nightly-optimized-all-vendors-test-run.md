### Added (CI)

- **Nightly optimized all-vendors test run** (CI, issue #1313): the `test-optimized` job in `nightly-build.yml` builds all vendors as `Release` and runs the full `run_all_tests.sh` suite, with a 40 minute bound; it runs beside the `.deb` build and a red suite fails the run.
