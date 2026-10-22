### Fixed
- **Cache the Claude Code CLI in the review workflow** (CI, issue #1528): restore the CLI from the Actions cache, install it with a longer backoff only on a miss, check that it reports 2.1.302, and hand it to the review action, so a rate-limited installer no longer fails the review.
