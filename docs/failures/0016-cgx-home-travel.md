# CGX USB home search driving an axis out of bounds

## Summary

On 2026-10-28 a meridian home route on the bare northern CGX (direct USB, motor
firmware 7.18.2214) timed out at its 180 s deadline with RA more than about 95
degrees west of the counterweight-down home, and the operator heard the motor
bind. The search started from BeyondPole HA -0.5 h, about 82.5 degrees from home.
The same evening a Park left Dec more than about 160 degrees from its switch
after starting about 55 degrees from it, and a FindHome was reported moving the
wrong way. Nothing in the driver watched the encoders during a home search: it
polled `MC_LEVEL_DONE` until the deadline. The CGX home travel guard now sums
each axis's encoder travel during FindHome, Park and the meridian home route and
stops a search that passes 270 degrees, or that has gone more than 90 degrees by
the #2213 re-home point, failing the operation with a kept error.

## Root Cause

Not established. No encoder trace was captured for the failed searches; the
positions above are the operator's observations, not readings. What the source
showed: both home waits checked only the completion flags, and a home command
plus a busy flag were taken as progress. The controller's search is not
documented: the #2213 capture (2026-11-12, ConformU SOPPierTest) shows that a
missed-sensor search can first drive the wrong way fast (1,479,041 counts in
10 s, 3.17 deg/s) to about 115 degrees from home and then crawl back at 0.27
deg/s. Whether the Oct-28 searches missed a sensor, reversed at a limit, or
hit a hard stop is unknown. The fake mount could not move during a home search
at all, so no test could show travel.

## Prevention

- `supervise_home_search()` (`celestron_cgx_telescope_driver.cpp`), called by
  both home waits every 200 ms, reads both encoders first and adds the wrapped
  difference from the previous read (`kHomeTravelBoundDegrees`, 270 deg; never
  reset by the re-home). Past the bound it throws "Celestron CGX home travel
  guard: ..."; the home loop's catch runs the verified cleanup stop and the
  error stays in the motion slot for Slewing (and AtHome for FindHome/Park).
- The #2213 re-home is sent only while the axis has gone 90 degrees or less
  (`kHomeRehomeTravelLimitDegrees`, the bound minus a half turn); otherwise the
  axis is stopped and the operation fails the same way, with no second search.
- Why 270 and 90: a direct home from the farthest slew position travels at most
  180 degrees (Dec; RA at most about 110 with tracking past the meridian), the
  #2213 miss with its re-home about 179 degrees, and it had gone at most 83
  degrees at the 60 s point. No home needs a full turn. Under #2213 alone the
  Oct-28 shape (a fast wrong-way search that never finds its switch) is stopped
  at 60 s after about 190 degrees and searched again, about 380 degrees in all.
- Not covered: an axis bound against a hard stop barely moves its encoder, so
  its sum hardly grows. That case is still bounded only by the #2213 stop at
  60 s, one re-home, and the 180 s deadline. A stall check (busy status with a
  still encoder) needs rig traces showing whether a legitimate search pauses.
- The same code runs over the SkyPortal TCP transport; the encoders are read
  through the same `CelestronCgxProtocol`.

Tests (fake only; no hardware run of the guard yet):

- `Fake Celestron CGX mount - a scripted home search drives its legs, and a runaway drives until stopped`
  (`AlpacaCore/tests/test_celestron_cgx_protocol.cpp`): the fake's home trajectory.
- `Celestron CGX USB Telescope Driver - a runaway home search is stopped at the re-home point, not searched again`
  (FindHome, Park, meridian route): the incident shape; before the guard it was
  re-homed and turned about 381 degrees before "home timed out".
- `Celestron CGX USB Telescope Driver - a re-home's travel counts toward the same home travel bound`.
- `Celestron CGX USB Telescope Driver - the longest legitimate homes pass the travel guard`.
- `Celestron CGX TCP driver - a runaway home search is stopped by the travel guard`
  (`AlpacaCore/tests/test_celestron_cgx_tcp_driver.cpp`).

## Evidence

- The 2026-10-28 incident notes (private rig evidence; the logs contain site
  coordinates): flip dispatched 19:39:11.781 UTC, "meridian home route timed
  out" 19:42:12.123, RA more than 95 degrees west and binding; Park at
  22:50:32.921 timed out with Dec more than about 160 degrees from its switch.
- #2213 and its fix #2440 (the 60 s re-home), with the per-axis home capture.
- Still missing: a bare-mount USB check of the guard, and home traces that show
  how far legitimate searches travel across many starts.
