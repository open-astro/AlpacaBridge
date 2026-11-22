### Fixed

- **OnStep MoveAxis checks its arguments first** (OnStep telescope, issue #2257): a bad axis or a NaN or out-of-range rate answered `NotConnected` while disconnected and `InvalidWhileParked` on a parked mount; it now throws `InvalidValue` in both states. A good MoveAxis on a parked mount is still refused as parked.
