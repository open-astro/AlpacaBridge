### Changed

- **Fake ConformU: telescope rows run a shorter PR profile on pull requests**, issue #2568. On a pull request, a telescope row runs ConformU's full-test settings without the extended rate-offset and pulse-guide tests, which took most of its 20 minutes. The run passes only when those two omitted tests are its only configuration alerts. The nightly and dispatched runs keep the full test, and `scripts/check_conformu_reports.py` refuses a committed report from the PR profile.
