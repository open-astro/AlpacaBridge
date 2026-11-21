### Added (tests)

- **Telescope contract sweep probes the members #2257 deferred** (tests, issue #2287): SideOfPier, TrackingRate, the target setters, SlewToAltAz(Async), SyncToCoordinates, SyncToAltAz, the rate-offset setters and DestinationSideOfPier are probed with bad arguments while disconnected, connected and parked. A member a driver does not implement keeps today's answer under a permanent exemption; the Sky-Watcher and Celestron NexStar drifts are listed as known failures for their driver PRs.
