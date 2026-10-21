### Breaking changes

- **A Sky-Watcher `UTCDate` write is refused while the host clock is NTP-synced** (Sky-Watcher direct driver, issue #1472, decision 0001): it throws `PropertyNotImplemented` and changes nothing, where it used to store the client's time for the readback while pointing ignored it. On a host without NTP the write is accepted as before. **After upgrading:** a client that sets the mount's time sees the write refused on an NTP-synced rig; the rig's own clock is used, which is the better one.
- **Sky-Watcher site getters throw until the site is set** (issue #1474, decision 0019): `SiteLatitude`, `SiteLongitude` and `SiteElevation` throw `InvalidOperation` when nothing set them, where they read 0.0. A device created in the web UI always has a site, so only a config saved before #274 is affected.

### Fixed

- **Sky-Watcher `SiderealTime` follows `UTCDate`** (issue #1472, decision 0001): `UTCDate`, `SiderealTime` and pointing read one time source, so a client can explain `SiderealTime` from its own `UTCDate` and `SiteLongitude` reads, as the ASCOM Telescope interface requires. A host that gains NTP after a write moves all three to its clock together.
- **A Sky-Watcher mount connects without a site** (issue #1474, decision 0019): a client that connects first and writes its site after can now connect. Until both coordinates are known, every member that depends on them (`RightAscension`, `Declination`, `Altitude`, `Azimuth`, `SiderealTime`, `SideOfPier`, `DestinationSideOfPier`, the slews and syncs, `Tracking=true`, `MoveAxis`, `PulseGuide`, `RightAscensionRate`, `DeclinationRate`) throws `InvalidOperation` naming what to set, and the connect sends the mount nothing that depends on the hemisphere. Writing the second coordinate makes the mount usable without a reconnect.

### Changed (docs)

- **Decisions 0001 and 0019 and the instructions describe the clock and site rules as built** (docs, issues #1472, #1474): decision 0019 is accepted; both records name their tests; the Sky-Watcher and driver-lifecycle instructions and the `/conformu` skill drop the connect refusal and the readback split.
