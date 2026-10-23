### Changed (tooling)

- **The nightly `.deb` waits for the optimized test suite** (`nightly-build.yml`): the `build` job now needs `test-optimized`, so a nightly is published only for a commit whose all-vendors `Release` suite passed. Before, the two jobs ran side by side and a red suite still published: the 2026-10-23 run 39925373472 published the nightly from `25a5f4b7` while `test-optimized` failed. A red suite now leaves the previous nightly and its tag in place, and the next scheduled run tries the same commit again.
