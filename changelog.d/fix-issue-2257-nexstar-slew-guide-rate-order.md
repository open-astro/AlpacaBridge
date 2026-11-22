### Fixed

- **Celestron NexStar checks slew coordinates first and refuses a negative guide rate** (Celestron telescope, issue #2257): SlewToCoordinates and SlewToCoordinatesAsync answered `NotConnected` or `InvalidWhileParked` to an RA or Dec out of range; they now throw `InvalidValue` in every state. With an autoguider port, a negative guide rate now throws `InvalidValue` instead of being sent to the mount.
