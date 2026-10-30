### Fixed

- **Celestron CGX state-only getters no longer stall behind serial traffic**, Celestron CGX driver, issue #1837. `RightAscension`, `Declination`, `Altitude`, `Azimuth` and `SideOfPier` read the axis positions, and `SyncToCoordinates` runs its settle and label-write exchanges, with the driver state lock released, so a getter such as `GuideRate` or `ApertureDiameter` answers within 150 ms while a position read or a sync waits on the mount. The five coordinate getters still queue behind a running `SyncToCoordinates`. Covered by `AlpacaCore/tests/test_celestron_cgx_getter_wait.cpp`.
