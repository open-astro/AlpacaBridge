# Task clock: adoption

The roster of [decision 0005](../decisions/0005-task-clock.md). It is kept out of the record so a PR that
adds or changes a row needs no `ADR authorized` comment; `scripts/check_docs_drift.py` check 23
compares it with the code.

## Adoption

The roster of drivers on the clock. It covers driver files; a protocol wrapper's I/O timeouts are not driver waits (rule 6). `scripts/check_docs_drift.py` check 23 fails when a driver names `TaskClock` without a row, or a row disagrees with its source.

Scan: `AlpacaCore/src/vendors/*/*_driver.cpp`
Symbol: `TaskClock`

| Driver | Source | State | Issue | Notes |
| --- | --- | --- | --- | --- |
| Sky-Watcher telescope | `AlpacaCore/src/vendors/skywatcher/skywatcher_telescope_driver.cpp` | adopted | #751, #917 | UTC anchors stay off the clock ([decision 0001](0001-skywatcher-pointing-clock.md)); position model clock in #1405 |
| Celestron telescope | `AlpacaCore/src/vendors/celestron/celestron_telescope_driver.cpp` | adopted | #923 | |
| SynScan telescope | `AlpacaCore/src/vendors/synscan/synscan_telescope_driver.cpp` | adopted | #1403 | |
| iOptron telescope | `AlpacaCore/src/vendors/ioptron/ioptron_telescope_driver.cpp` | adopted | #1223, #1723 | the fault latch stamp (the decision 0009 staleness bound) and its probe interval |
| Bisque telescope | `AlpacaCore/src/vendors/bisque/bisque_telescope_driver.cpp` | adopted | #1162 | |
| Celestron CGX telescope | `AlpacaCore/src/vendors/celestron/celestron_cgx_telescope_driver.cpp` | adopted | #1409, #1983 | continuity deadline and sleeps (#1404); UTC anchor elapsed time and host-discipline re-check (#1983); the GOTO correction lead, an elapsed time added to the UTC a correction aims at (rule 3); the UTC value stays on `system_clock` |
| ToupTek camera | `AlpacaCore/src/vendors/touptek/touptek_camera_driver.cpp` | adopted | #1268 | |
| ToupTek filter wheel | `AlpacaCore/src/vendors/touptek/touptek_filterwheel_driver.cpp` | adopted | #1268 | |
| Player One camera | `AlpacaCore/src/vendors/playerone/playerone_camera_driver.cpp` | adopted | #1279 | |
| QHY camera | `AlpacaCore/src/vendors/qhy/qhy_camera_driver.cpp` | adopted | #1386 | |
| QHY filter wheel | `AlpacaCore/src/vendors/qhy/qhy_filterwheel_driver.cpp` | adopted | #1386 | |
| QHY Q-Focuser | `AlpacaCore/src/vendors/qhy/qhy_focuser_driver.cpp` | adopted | #1310 | on the clock from its first slot |
| SVBONY camera | `AlpacaCore/src/vendors/svbony/svbony_camera_driver.cpp` | adopted | #1282 | |
| gphoto camera | `AlpacaCore/src/vendors/gphoto/gphoto_camera_driver.cpp` | adopted | #1282 | |
| ZWO camera | `AlpacaCore/src/vendors/zwo/zwo_camera_driver.cpp` | adopted | #1328 | |
| Astroasis focuser | `AlpacaCore/src/vendors/astroasis/astroasis_focuser_driver.cpp` | adopted | #1327 | on the clock from its first slot |
| Gemini focuser | `AlpacaCore/src/vendors/gemini/gemini_focuser_driver.cpp` | adopted | #1229 | on the clock from its first slot |
| Gemini flat panel | `AlpacaCore/src/vendors/gemini/gemini_flatpanel_driver.cpp` | adopted | #1255 | |
| Celestron focus motor | `AlpacaCore/src/vendors/celestron/celestron_focuser_driver.cpp` | adopted | #1320 | on the clock from its first slot |
| iOptron iGuider camera | `AlpacaCore/src/vendors/ioptron/ioptron_iguider_camera_driver.cpp` | adopted | | on the clock from its first slot; exposure, stall and first-frame deadlines |
| WandererAstro switch box | `AlpacaCore/src/vendors/wandererastro/wandererastro_box_switch_driver.cpp` | adopted | #730 | link-health timing |
| WandererAstro cover | `AlpacaCore/src/vendors/wandererastro/wandererastro_covercalibrator_driver.cpp` | adopted | #730 | link-health timing |
| WandererAstro filter wheel | `AlpacaCore/src/vendors/wandererastro/wandererastro_filterwheel_driver.cpp` | adopted | #730 | link-health timing |
| OnStep telescope | `AlpacaCore/src/vendors/onstep/onstep_telescope_driver.cpp` | adopted | #1378 | moved with its first slot (the MoveAxis re-send) |
| OnStep focuser | `AlpacaCore/src/vendors/onstep/onstep_focuser_driver.cpp` | adopted | #2223 | on the clock from its first slot (the `PolledState` poll): its period, frame age and fault stamp |
| OnStep rotator | `AlpacaCore/src/vendors/onstep/onstep_rotator_driver.cpp` | adopted | #2221 | on the clock from its first slot (the `PolledState` poll): its period, frame age and fault stamp |
| OnStep switch | `AlpacaCore/src/vendors/onstep/onstep_switch_driver.cpp` | adopted | #2222 | on the clock from its first slot (the `PolledState` poll): its period, frame age and fault stamp |
| ZWO telescope | `AlpacaCore/src/vendors/zwo/zwo_telescope_driver.cpp` | adopted | #1432, #1433 | telemetry, GOTO, pulse and control deadlines; UTC/LST and protocol timeouts stay off the clock |
| iOptron iEAF focuser | `AlpacaCore/src/vendors/ioptron/ioptron_ieaf_focuser_driver.cpp` | adopted | #1590 | status cache age |
| iOptron iEFW filter wheel | `AlpacaCore/src/vendors/ioptron/ioptron_filterwheel_driver.cpp` | adopted | #1723 | status cache age and its fault stamp (the decision 0009 staleness bound) |
| iOptron iFR focuser | `AlpacaCore/src/vendors/ioptron/ioptron_ifr_focuser_driver.cpp` | adopted | #2002 | status cache age, its fault stamp (the decision 0009 staleness bound) and the move hold |
| iOptron iFR rotator | `AlpacaCore/src/vendors/ioptron/ioptron_ifr_rotator_driver.cpp` | adopted | #2002 | status cache age, its fault stamp (the decision 0009 staleness bound) and the move hold |
| ZWO EAF focuser | `AlpacaCore/src/vendors/zwo/zwo_focuser_driver.cpp` | adopted | #1666 | status cache age and its fault stamp (the decision 0009 staleness bound) |
| ZWO dew heater switch | `AlpacaCore/src/vendors/zwo/zwo_switch_driver.cpp` | adopted | #1667 | heater read cache age and its fault stamp (the decision 0009 staleness bound) |
| ZWO EFW filter wheel | `AlpacaCore/src/vendors/zwo/zwo_filterwheel_driver.cpp` | adopted | #1668 | Position cache age and its fault stamp (the decision 0009 staleness bound) |
| ZWO CAA rotator | `AlpacaCore/src/vendors/zwo/zwo_rotator_driver.cpp` | adopted | #1668 | status cache age and its fault stamp (the decision 0009 staleness bound) |
| WeeWX observing conditions | `AlpacaCore/src/vendors/weewx/weewx_observingconditions_driver.cpp` | adopted | #2085, #1984 | feed poll interval wait (#2085) and its fault stamp (the decision 0009 staleness bound, #1984) |
| remotealpaca observing conditions | `AlpacaCore/src/vendors/remotealpaca/remotealpaca_observingconditions_driver.cpp`, `AlpacaCore/src/vendors/remotealpaca/upstream_proxy.h` | adopted | #1753 | upstream poll period wait, the wait for the upstream's Connecting, the fault stamp (the decision 0009 staleness bound) and the snapshot age; the Connecting wait and the poll's clock are held by the vendor's shared `UpstreamProxy` |
| remotealpaca safety monitor | `AlpacaCore/src/vendors/remotealpaca/remotealpaca_safetymonitor_driver.cpp` | adopted | #1753 | as the observing conditions row, through the vendor's shared `UpstreamProxy` (`upstream_proxy.h`) |
| remotealpaca rotator | `AlpacaCore/src/vendors/remotealpaca/remotealpaca_rotator_driver.cpp` | adopted | #1753, #2708 | as the observing conditions row, through the vendor's shared `UpstreamProxy` (`upstream_proxy.h`); `DeviceState`'s `TimeStamp` is the frame's age on the task clock |
| remotealpaca filter wheel | `AlpacaCore/src/vendors/remotealpaca/remotealpaca_filterwheel_driver.cpp` | adopted | #1753, #2706 | as the observing conditions row, through the vendor's shared `UpstreamProxy` (`upstream_proxy.h`) |
| remotealpaca switch | `AlpacaCore/src/vendors/remotealpaca/remotealpaca_switch_driver.cpp` | adopted | #1753, #2709 | as the observing conditions row, through the vendor's shared `UpstreamProxy` (`upstream_proxy.h`); `DeviceState`'s `TimeStamp` is the frame's age on the task clock |
| remotealpaca telescope | `AlpacaCore/src/vendors/remotealpaca/remotealpaca_telescope_driver.cpp` | adopted | #1753, #2710 | as the observing conditions row, through the vendor's shared `UpstreamProxy` (`upstream_proxy.h`); `DeviceState`'s `TimeStamp`, `UTCDate` and `SiderealTime` advance by the frame's age on the task clock; the per-axis MoveAxis cap waits its 60 s window and its verified stop's 250 ms polls on it |
| Player One thermal switch | `AlpacaCore/src/vendors/playerone/playerone_switch_driver.cpp` | adopted | #2088 | status cache age and its fault stamp |
| Gemini PDH switch | `AlpacaCore/src/vendors/gemini/gemini_pdh_switch_driver.cpp` | adopted | #1987 | fault stamp (the decision 0009 staleness bound) |
