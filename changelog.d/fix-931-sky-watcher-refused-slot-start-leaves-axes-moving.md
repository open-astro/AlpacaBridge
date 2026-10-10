### Fixed

- **A Sky-Watcher goto, Park or FindHome that the slew slot refuses to start now stops the axes** (SkyWatcher, issue #968): the refused start had already marked the running slew Superseded, so nothing stopped the axes it was driving and they kept moving while the call failed; the rollback now sends the stop to both axes (`start_slew_body()`), covered by the three `refused ... start leaves the axes stopped` cases in `test_skywatcher_async.cpp`.
