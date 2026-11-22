### Fixed
- **SynScan Telescope Driver** (AlpacaCore): a MoveAxis that the client does not renew within 60 s is now stopped by the driver, so a client that dies or loses its link mid-jog no longer leaves the axis running. Each new MoveAxis restarts the 60 s. Slewing reports the stop as an error until the next motion command or a reconnect. Issue #2412.
- **SynScan Telescope Driver** (AlpacaCore): a GOTO, Park or MoveAxis that was running when the link dropped is stopped by the next Connect, before anything else is sent, instead of being forgotten. Issue #2412.

### Breaking changes
- **SynScan Telescope Driver** (AlpacaCore): SiteLatitude, SiteLongitude, UTCDate and SyncToCoordinates/SyncToTarget writes are refused with InvalidOperation while a GOTO, Park or MoveAxis is under way, because they change the handset's pointing maths under the motion. A MoveAxis now stops on its own after 60 s unless the client sends it again. **After upgrading:** write the site, time or Sync after the slew has ended, and renew a MoveAxis that must run longer than 60 s.
