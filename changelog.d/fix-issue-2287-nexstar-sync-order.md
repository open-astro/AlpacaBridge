### Fixed

- **Celestron NexStar SyncToCoordinates checks its coordinates first** (Celestron telescope, issue #2287): an RA or Dec out of range answered `NotConnected` while disconnected and `InvalidWhileParked` during a park; it now throws `InvalidValue` in every state.
