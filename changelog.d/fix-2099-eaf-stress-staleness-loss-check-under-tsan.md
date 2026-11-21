### Fixed

- **ZWO EAF staleness-loss stress case no longer depends on scheduling**, ZWO EAF focuser tests, issue #2099. The case now drives one undisturbed staleness loss (three failing reads, the fake clock past `util::kLinkStalenessBound`, one more read) and requires the `lost: status fault stood` warning before the storm, so `losses.load() > 0` holds on a starved host. Test-only: `AlpacaCore/tests/test_zwo_concurrency_stress.cpp`.
