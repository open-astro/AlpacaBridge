### Fixed
- **ZWO AM Telescope Driver** (AlpacaCore): motion fails closed (issue #2408, part of #2396). Tested on the fake mount only, not on hardware.
  - A GOTO still running 15 minutes after the mount accepted it is stopped, for async slews too, and Slewing reports the stop. A synchronous slew that times out now stops the mount.
  - A MoveAxis that is not renewed by a new MoveAxis within 60 s is stopped, and Slewing reports it.
  - AbortSlew checks that the mount stopped and sends the stop again, up to three times, then Slewing reports the failure.
  - A GOTO that comes to rest more than 600 arcsec from its target is reported as a failure instead of a landing.
  - A GOTO start with no reply or an "N/A" reply, or a target write with no reply, is no longer taken as accepted: the driver stops the mount and the slew fails.
  - A GOTO the mount refuses as below the horizon (e5) is no longer sent again after a site and time re-push.
  - A reconnect after a link that dropped during a GOTO or a MoveAxis stops the mount before anything else.

### Breaking changes
- **ZWO AM Telescope Driver** (AlpacaCore): SiteLatitude, SiteLongitude, SiteElevation and UTCDate writes are refused with InvalidOperation while the mount runs a GOTO, a MoveAxis, a Park or a FindHome. A MoveAxis now stops after 60 s unless the client sends MoveAxis again. A GOTO that lands more than 600 arcsec from its target, and a GOTO start the mount does not confirm, now raise through Slewing where they used to read as finished. **After upgrading:** write the site and time while the mount is still, and renew a long MoveAxis at least every 60 s.
