### Changed

- **ZWO telescope test fake**, ZWO telescope driver, issue #1596. A stateful `FakeZwoMount` (`AlpacaCore/tests/fake_zwo_mount.h`) answers the AM-mount command dialect over loopback and derives GOTO, park and jog motion from the shared `FakeTaskClock`; its contract is pinned by `AlpacaCore/tests/test_zwo_fake_mount.cpp`, and `AlpacaCore/tests/test_zwo_telescope_clock.cpp` drives each timed wait of the telescope driver (connect retry, poll cadence, pulse window, busy back-off, sync retry, slew timeout, forced Slewing windows) with `advance()`. No driver behaviour changes.
