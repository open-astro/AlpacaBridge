### Changed (tooling)

- **SBOM attestation uses `actions/attest`** (`nightly-build.yml`, `release.yml`): `actions/attest-sbom` is deprecated and warned on every nightly build ("actions/attest-sbom has been deprecated, please use actions/attest instead"). Its v4.1.0 is a wrapper that passes the same `subject-path` and `sbom-path` to `actions/attest`, so the step now calls `actions/attest` v4.2.2 directly with the same inputs; the attestation is unchanged.
