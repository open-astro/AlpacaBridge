### Fixed

- **iOptron site getters and DeviceState answer at once with a fault latched**, iOptron telescope driver, issue #2677. With the poll's fault latched, SiderealTime, SiteLatitude, SiteLongitude, DestinationSideOfPier and DeviceState waited on the operation lock behind the poll's probe, up to one `:GLS#` reply timeout, before refusing. They now refuse with `DriverException` under the state lock ([decision 0016](docs/decisions/0016-getters-never-wait-on-device-io.md) rule 1). Covered by `AlpacaCore/tests/test_ioptron_polled_state.cpp`.
