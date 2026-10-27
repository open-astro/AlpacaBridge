### Fixed
- **HTTP worker pool stays within its configured limits** (AlpacaHTTP, issue #1691): clamp YAML `http.thread_pool_size` to 1–256 through the existing setter, as the environment override does; zero now starts one worker instead of leaving requests unanswered, and `-1` normalizes to 256 instead of failing startup. Valid values and the default remain unchanged.
