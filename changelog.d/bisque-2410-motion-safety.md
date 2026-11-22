### Fixed

- **Bisque (TheSkyX) Telescope Driver** (AlpacaCore): motion fails closed (issue #2410, the #2396 pattern). Fake-only evidence; not tested on hardware.
  - The synchronous slew's 120 s deadline now sends `Abort()` before it throws, and Slewing raises the failure.
  - An async slew still in flight 300 s after its `SlewToRaDec` is stopped by the status poll, which runs whether or not a client polls.
  - A MoveAxis not renewed by another nonzero MoveAxis within 60 s is stopped. A disconnect stops a MoveAxis before the socket closes.
  - A `SlewToRaDec` that gets no reply is no longer read as "not slewing": the driver stops the mount and the slew fails. A TheSkyX error reply still fails it with nothing else sent.
  - AbortSlew is verified: `Abort()` is sent again while `IsSlewComplete` reads false, three times at most, and an unverified stop fails.
  - Each of these failures is raised by Slewing until the next motion command or a connect. A stop that fails is tried again 10 s later, never on every tick, and a start is never re-sent.
  - A slew or MoveAxis that a lost link left running is stopped by the next connect before any other command, also after a disconnect or a connect that failed.

### Breaking changes

- **Bisque MoveAxis stops after 60 s** unless the client sends MoveAxis again with a nonzero rate. An async slew is stopped after 300 s. A disconnect stops a running MoveAxis. AbortSlew can now fail with DriverException when TheSkyX still reports the slew after three stops. **After upgrading:** a client that jogs an axis for more than a minute must renew the MoveAxis call within each 60 s.
