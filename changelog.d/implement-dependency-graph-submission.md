### Added (tooling)

- **Dependency graph submission for the C++ build** (CI, issue #1004): `dependency-submission.yml` posts a snapshot from `scripts/dependency_snapshot.py` on each push to `main` covering `debian/control`, the CMake-fetched nlohmann/json and the vendored SDKs in `scripts/dependency_sdk_versions.json`; the `docs-drift` job runs its `--self-test` and a `--check` that fails on an unlisted `AlpacaCore/external/` directory or a stale table entry.
