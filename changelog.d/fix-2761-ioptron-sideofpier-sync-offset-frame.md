### Fixed

- **iOptron SideOfPier and DestinationSideOfPier after a Sync**, iOptron telescope driver, issue #2761. `SideOfPier` now names the side of the mount's own position, and `DestinationSideOfPier` the side of the RA the GOTO sends (the client RA less the Sync offset). Both used to mix the Sync-shifted frame with the mount's, so a Sync near the meridian could name a side the mount would not use. Tests: `iOptron SideOfPier - after a +1 h Sync it names the side of the mount's own position` and `iOptron DestinationSideOfPier - after a +1 h Sync it names the side the GOTO lands on`.
