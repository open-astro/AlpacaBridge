### Fixed
- **iOptron Telescope Driver** (AlpacaCore): an asynchronous GoTo is reported finished only once the mount has really landed. If the mount reports stopped away from its target, Slewing stays true until two position reads half a second apart agree, and a GoTo that came to rest more than 10 arcminutes from its target now raises an error instead of reporting success. Issue #2396.
- **iOptron Telescope Driver** (AlpacaCore): a MoveAxis left running for 60 seconds with no new MoveAxis, and an asynchronous GoTo still slewing after 300 seconds, are now stopped by the driver even when no client is polling, and Slewing reports the error. Issue #2396.

### Breaking changes
- **iOptron MoveAxis stops after 60 seconds without a new MoveAxis**: a client that starts one MoveAxis and holds it for more than a minute now sees the mount stop and Slewing raise an error. **After upgrading:** a client that needs a longer move sends MoveAxis again before 60 seconds pass. Issue #2396.
