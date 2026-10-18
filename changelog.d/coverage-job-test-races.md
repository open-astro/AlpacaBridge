### Fixed
- **Sidereal time keeps its fraction of a second on six mount drivers** (SynScan, Bisque, OnStep, iOptron, Celestron NexStar, ZWO; issue #1298): each driver's local sidereal time cut the UTC clock to whole seconds, so `SiderealTime`, and every hour angle and park target computed from it, moved in one-second steps and could read up to one second of RA (15 arcseconds) behind. The helpers now use the full-precision clock, as the Celestron CGX driver already did.

### Fixed (tests)
- **Coverage-job flakes in three timing-bound tests** (tests, issues #1298 and #976): the ToupTek thermal-poller case now waits for the poller tick's last SDK read rather than its first, the two SynScan SetPark sidereal-shift cases bound the park RA by the wall time they measured between SetPark and Park instead of expecting an exact `r40000000` (which only held because the driver's sidereal time moved in whole seconds, fixed above), and the QHY CFW3 goto-versus-disconnect race waits on a new `FakeQhyCfw3::quiescent()` before it reconnects, since counting the goto right after the join could miss a byte still on its way to the fake. Together these were 21 of the 28 coverage-only failures seen from 2026-10-12 to 2026-10-18.

### Changed (CI)
- **Pull requests no longer wait for the coverage job** (CI, issue #1301): the gcovr coverage job took longest (767 s, against 432 s for `build-vendors`), and it gates nothing, since it has no threshold and is not a required check. It now runs on pushes to `main` and `stable/**` and on manual runs. The `clang-tidy` job restores `build-vendors`' compiler cache for its compile-database build (same configure, same build directory). The `cppcheck` job caches its 2.17.1 source build, rebuilds whenever the restored binary does not report the pinned version, and saves it from pushes to `main`, so pull requests skip the roughly 60 s build.

### Fixed (build)
- **iOptron builds without libgpiod v2 again** (AlpacaCore): with the iMate PowerBox switch compiled out, `create_switch()` in `ioptron_catalog.cpp` never used `device_number`, and `-Werror=unused-parameter` stopped the build. CI always builds libgpiod, so it never saw this.
