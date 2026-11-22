### Fixed
- **Merge gate stops on a head with many check runs** (CI, `.github/scripts/merge-gate.sh`, issue #2436): the gate gave the head's whole check-runs JSON to `jq` as one command-line argument. Linux caps one argument at 128 KiB, and 37 check runs are about 153 KB, so `jq` exited with "Argument list too long" and the gate did not judge the PR. The gate now gives the check runs and commit statuses to `jq` on stdin. Its rules do not change.

### Added (tests)
- **Merge gate over the argument cap** (`scripts/tests/test_merge_gate.py`, issue #2436): one test case with 65 check runs of about 4 KB each.
