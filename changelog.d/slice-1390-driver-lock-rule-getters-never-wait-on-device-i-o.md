### Changed
- **Driver lock rule: getters never wait on device I/O** (docs, issue #1390): `AGENTS.md` and the driver lifecycle instructions no longer tell drivers to hold `mutex_` across SDK calls. The driver `mutex_` is a short state lock, device transactions run under one per-link I/O lock, and the camera `with_camera`/`with_handle` calls over short SDK calls stay allowed.

### Added (docs)
- **Decision 0016, getters never wait on device I/O** (issue #1390): the rule, the lock order, the camera exception, the known deviations in Celestron and Bisque, and how to reverse it.
