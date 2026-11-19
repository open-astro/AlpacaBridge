### Added (internal)
- **Shared telescope motion guards** (AlpacaCore, issue #2405): `util/motion_guards.h` holds the protocol-neutral guards that make motion fail closed, with every threshold in `util/motion_policy.h`: a verified stop (resent up to 3 times within 3 s until the status shows no motion and two position reads 500 ms apart agree within 30 arcsec, else `AbortSlew: the mount still reports motion after 3 stops`), the slew deadline (2 x distance / rate + 10 s, at most 300 s) and the MoveAxis cap (60 s, 5 to 300 s), each of which runs the verified stop when it passes. No driver uses them yet, so nothing changes for clients.

### Added (tests)
- **Motion guard cases** (issue #2405): `[motion-guards]` cases for each guard's verified and failed paths on the fake task clock.
