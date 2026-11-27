# Getters never wait on device I/O: adoption

The roster of [decision 0016](../decisions/0016-getters-never-wait-on-device-io.md). It is kept out of the record so a PR that
adds or changes a row needs no `ADR authorized` comment; `scripts/check_docs_drift.py` check 23
compares it with the code.

## Adoption

The roster of drivers checked against the Rules of the record, from a full audit on 2026-10-29. A `partial` row names the issue that fixes it. `scripts/check_docs_drift.py` check 23 holds it to the code; a Symbol cell names the lock, or the class it delegates to, of a driver that has no `transition_mutex_`.

Scan: `AlpacaCore/src/vendors/*/*_driver.cpp`
Symbol: `transition_mutex_`

| Driver | Symbol | Source | State | Issue | Notes |
| --- | --- | --- | --- | --- | --- |
| Altair camera | `create_toupcam_family_camera` | `AlpacaCore/src/vendors/altair/altair_camera_driver.cpp` | adopted |  |  |
| Astroasis focuser |  | `AlpacaCore/src/vendors/astroasis/astroasis_focuser_driver.cpp` | adopted |  | served from `PolledState` |
| Bisque telescope |  | `AlpacaCore/src/vendors/bisque/bisque_telescope_driver.cpp` | adopted | #1838 | rule 6: both slew forms re-check the link and park after the live `is_tracking()` read (#1883) |
| Celestron CGX telescope |  | `AlpacaCore/src/vendors/celestron/celestron_cgx_telescope_driver.cpp` | adopted | #1759 |  |
| Celestron focuser |  | `AlpacaCore/src/vendors/celestron/celestron_focuser_driver.cpp` | adopted |  |  |
| Celestron telescope |  | `AlpacaCore/src/vendors/celestron/celestron_telescope_driver.cpp` | adopted |  |  |
| Gemini flat panel |  | `AlpacaCore/src/vendors/gemini/gemini_flatpanel_driver.cpp` | adopted |  |  |
| Gemini focuser |  | `AlpacaCore/src/vendors/gemini/gemini_focuser_driver.cpp` | adopted | #1840 | served from `PolledState`; MaxStep read once at connect; `TempComp` is read by the poll and served from `PolledState` too (#2695), `set_temp_comp` writing through |
| Gemini PDH switch |  | `AlpacaCore/src/vendors/gemini/gemini_pdh_switch_driver.cpp` | adopted |  |  |
| gphoto camera |  | `AlpacaCore/src/vendors/gphoto/gphoto_camera_driver.cpp` | adopted |  |  |
| iOptron filter wheel |  | `AlpacaCore/src/vendors/ioptron/ioptron_filterwheel_driver.cpp` | adopted |  |  |
| iOptron iEAF focuser |  | `AlpacaCore/src/vendors/ioptron/ioptron_ieaf_focuser_driver.cpp` | adopted |  |  |
| iOptron iFR focuser |  | `AlpacaCore/src/vendors/ioptron/ioptron_ifr_focuser_driver.cpp` | adopted | #2002 | the I/O lock is the shared `IfrLink` session's, under the status cache |
| iOptron iFR rotator |  | `AlpacaCore/src/vendors/ioptron/ioptron_ifr_rotator_driver.cpp` | adopted | #2002 | the I/O lock is the shared `IfrLink` session's, under the status cache |
| iOptron iGuider camera |  | `AlpacaCore/src/vendors/ioptron/ioptron_iguider_camera_driver.cpp` | adopted |  |  |
| iOptron switch |  | `AlpacaCore/src/vendors/ioptron/ioptron_switch_driver.cpp`, `AlpacaCore/include/alpacacore/util/power_port_switch_driver.h` | adopted |  |  |
| iOptron telescope |  | `AlpacaCore/src/vendors/ioptron/ioptron_telescope_driver.cpp` | adopted | #1829, #2491 | `util::LockGate` drops `mutex_` per exchange; the position, Alt/Az, status and Slewing getters serve the polled frame and send nothing (#2491, [decision 0010](../decisions/0010-snapshot-served-state.md)); the TTL clause stays for the four site getters (SiderealTime, SiteLatitude, SiteLongitude, DestinationSideOfPier): one whose cache lapsed takes the I/O lock for that read |
| OnStep telescope | | `AlpacaCore/src/vendors/onstep/onstep_telescope_driver.cpp` | adopted | | `util::LockGate` drops `mutex_` per exchange |
| OnStep focuser |  | `AlpacaCore/src/vendors/onstep/onstep_focuser_driver.cpp` | adopted | #2223 | served from `PolledState`; the I/O lock is the shared link's (its sequence lock, then its wire lock), and `io_mutex_` orders `Move`, `Halt` and the `TempComp` write |
| OnStep rotator |  | `AlpacaCore/src/vendors/onstep/onstep_rotator_driver.cpp` | adopted | #2221 | served from `PolledState`; the I/O lock is the shared link's (its sequence lock, then its wire lock), and `io_mutex_` orders the moves, `Halt` and `Sync` |
| OnStep switch |  | `AlpacaCore/src/vendors/onstep/onstep_switch_driver.cpp` | adopted | #2222 | served from `PolledState`; the I/O lock is the shared link's (its sequence lock, then its wire lock), and `io_mutex_` orders the writes |
| Player One camera |  | `AlpacaCore/src/vendors/playerone/playerone_camera_driver.cpp` | adopted |  |  |
| Player One filter wheel |  | `AlpacaCore/src/vendors/playerone/playerone_filterwheel_driver.cpp` | adopted |  |  |
| Player One thermal switch |  | `AlpacaCore/src/vendors/playerone/playerone_switch_driver.cpp` | adopted |  |  |
| QHY camera |  | `AlpacaCore/src/vendors/qhy/qhy_camera_driver.cpp` | adopted |  | on the I/O-lock order |
| QHY CFW3 filter wheel |  | `AlpacaCore/src/vendors/qhy/qhy_cfw3_filterwheel_driver.cpp` | adopted |  |  |
| QHY filter wheel |  | `AlpacaCore/src/vendors/qhy/qhy_filterwheel_driver.cpp` | adopted |  |  |
| QHY focuser |  | `AlpacaCore/src/vendors/qhy/qhy_focuser_driver.cpp` | adopted |  | served from `PolledState` |
| Sky-Watcher telescope |  | `AlpacaCore/src/vendors/skywatcher/skywatcher_telescope_driver.cpp` | adopted |  | `util::LockGate` drops `mutex_` per exchange |
| SVBONY camera |  | `AlpacaCore/src/vendors/svbony/svbony_camera_driver.cpp` | adopted |  |  |
| SynScan telescope |  | `AlpacaCore/src/vendors/synscan/synscan_telescope_driver.cpp` | adopted |  | the wrapper's lock is the I/O lock; served from `PolledState` |
| ToupTek camera |  | `AlpacaCore/src/vendors/touptek/touptek_camera_driver.cpp` | adopted |  |  |
| ToupTek filter wheel |  | `AlpacaCore/src/vendors/touptek/touptek_filterwheel_driver.cpp` | adopted |  |  |
| ToupTek focuser |  | `AlpacaCore/src/vendors/touptek/touptek_focuser_driver.cpp` | adopted |  |  |
| ToupTek switch |  | `AlpacaCore/src/vendors/touptek/touptek_switch_driver.cpp`, `AlpacaCore/include/alpacacore/util/power_port_switch_driver.h` | adopted |  |  |
| ToupTek thermal switch |  | `AlpacaCore/src/vendors/touptek/touptek_thermal_switch_driver.cpp` | adopted |  |  |
| WandererAstro box switch |  | `AlpacaCore/src/vendors/wandererastro/wandererastro_box_switch_driver.cpp` | adopted |  |  |
| WandererAstro cover |  | `AlpacaCore/src/vendors/wandererastro/wandererastro_covercalibrator_driver.cpp` | adopted |  | writes with no reply wait under `state_mutex_` (Alternatives rejected) |
| WandererAstro filter wheel |  | `AlpacaCore/src/vendors/wandererastro/wandererastro_filterwheel_driver.cpp` | adopted |  | writes with no reply wait under `state_mutex_` (Alternatives rejected) |
| WandererAstro rotator |  | `AlpacaCore/src/vendors/wandererastro/wandererastro_rotator_driver.cpp` | adopted |  |  |
| WeeWX observing conditions |  | `AlpacaCore/src/vendors/weewx/weewx_observingconditions_driver.cpp` | adopted |  |  |
| ZWO ASIAIR Plus switch |  | `AlpacaCore/src/vendors/zwo/zwo_asiair_plus_switch_driver.cpp`, `AlpacaCore/include/alpacacore/util/power_port_switch_driver.h` | adopted |  |  |
| ZWO ASIAIR switch |  | `AlpacaCore/src/vendors/zwo/zwo_asiair_switch_driver.cpp`, `AlpacaCore/include/alpacacore/util/power_port_switch_driver.h` | adopted |  |  |
| ZWO camera |  | `AlpacaCore/src/vendors/zwo/zwo_camera_driver.cpp` | adopted |  |  |
| ZWO filter wheel |  | `AlpacaCore/src/vendors/zwo/zwo_filterwheel_driver.cpp` | adopted |  |  |
| ZWO focuser |  | `AlpacaCore/src/vendors/zwo/zwo_focuser_driver.cpp` | adopted |  |  |
| ZWO rotator |  | `AlpacaCore/src/vendors/zwo/zwo_rotator_driver.cpp` | adopted |  |  |
| ZWO switch |  | `AlpacaCore/src/vendors/zwo/zwo_switch_driver.cpp` | adopted |  |  |
| ZWO telescope |  | `AlpacaCore/src/vendors/zwo/zwo_telescope_driver.cpp` | adopted |  |  |
| remotealpaca (proxied devices) |  | `AlpacaCore/src/vendors/remotealpaca/remotealpaca_observingconditions_driver.cpp`, `AlpacaCore/src/vendors/remotealpaca/remotealpaca_safetymonitor_driver.cpp`, `AlpacaCore/src/vendors/remotealpaca/remotealpaca_rotator_driver.cpp`, `AlpacaCore/src/vendors/remotealpaca/remotealpaca_filterwheel_driver.cpp`, `AlpacaCore/src/vendors/remotealpaca/remotealpaca_switch_driver.cpp`, `AlpacaCore/src/vendors/remotealpaca/remotealpaca_telescope_driver.cpp`, `AlpacaCore/src/vendors/remotealpaca/remotealpaca_focuser_driver.cpp`, `AlpacaCore/src/vendors/remotealpaca/remotealpaca_dome_driver.cpp`, `AlpacaCore/src/vendors/remotealpaca/upstream_proxy.h` | adopted | #1753 | snapshot-served (ObservingConditions, SafetyMonitor, Rotator, FilterWheel, Switch, Telescope, Focuser, Dome, over the shared `UpstreamProxy`, which holds the state lock, a leaf, and the rule that no upstream call runs under it); commands in `AsyncOperation` ([decision 0023](../decisions/0023-outbound-clients-in-vendor-wrappers.md)), serialized in caller order by the proxy's command order mutex, which a command's caller takes with no other lock held and no command body takes (the Telescope's MoveAxis cap body takes it as a caller for its stop, with no other lock held, and Connect and Disconnect join the caps before the command slot); the I/O lock is the `RemoteAlpacaClient` call mutex |
