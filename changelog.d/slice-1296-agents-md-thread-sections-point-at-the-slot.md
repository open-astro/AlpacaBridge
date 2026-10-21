### Changed (docs)
- **AGENTS.md thread sections point at the operation slot** (issue #1296): "Threads & shutdown", the Telescope Park section and the GPIO section name `util::AsyncOperation` as the owner of the task thread instead of restating its rules, and a new driver must not hand-roll a task thread. No code change; ConformU not re-run.
