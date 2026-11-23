### Security
- **Add a security policy** (docs, issue #2631): `SECURITY.md` explains how to report a vulnerability privately through GitHub, which versions get fixes, and what is in scope.
- **Pin the review bot's Claude Code CLI by sha256** (CI, issue #2632): `claude-review.yml` downloads the 2.1.325 linux-x64 binary directly instead of piping `install.sh` into bash, and checks it against a pinned sha256 on every run, cache hits included.
- **Check every vendored binary against a SHA256 manifest** (CI, issue #2633): `AlpacaCore/external/SHA256SUMS` lists the sha256 of every file git tracks as binary under `AlpacaCore/external/`, and `scripts/check_vendor_binaries.py` (a `static-checks` step and pre-flight gate 2h) fails on a changed, unlisted or removed binary.
