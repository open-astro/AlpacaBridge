### Fixed

- **ZWO AM Tracking write no longer drops the in-flight poll**, ZWO telescope driver, issue #1531. A `Tracking` write that repeats the value the mount already holds (inside the 2 s fast-path window) is a no-op and now leaves the poll in flight to publish; before, each one bumped the snapshot write epoch, so a client re-sending `Tracking` on every cycle kept `DeviceState` frozen and its `TimeStamp` age growing. Covered by "ZWO Telescope Driver - a same-value Tracking write does not drop the in-flight poll" in `AlpacaCore/tests/test_zwo_telescope.cpp`.
