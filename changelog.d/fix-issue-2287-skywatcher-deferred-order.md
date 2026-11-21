### Fixed

- **Sky-Watcher rate offsets refuse NaN; TrackingRate and SyncToCoordinates check their arguments first** (Sky-Watcher telescope, issue #2287): RightAscensionRate and DeclinationRate accepted a NaN rate while connected and applied it to the drive; they now throw `InvalidValue` in every state. A bad TrackingRate answered `NotConnected` while disconnected, and a bad SyncToCoordinates RA or Dec answered `NotConnected` or `InvalidWhileParked`; both now throw `InvalidValue` first.
