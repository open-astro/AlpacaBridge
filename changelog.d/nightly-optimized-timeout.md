### Fixed (tooling)
- **The nightly optimized test job no longer times out** (CI, `nightly-build.yml`): `test-optimized` gets a 75 minute bound instead of 40. The Release compile of `AlpacaHTTP/tests/test_routing.cpp` took 28 minutes on 2026-11-23 (16 to 20 before), so run 45176256685 was cancelled at 40 minutes with 4242 of 5775 tests passed and none failed, and no nightly `.deb` was published.
