### Added (tests)

- **Cross-origin device PUT case** (decision 0007 rule 2): `test_routing.cpp` sends a cross-origin, a same-origin and a no-Origin PUT to telescope `slewtocoordinatesasync`, camera `startexposure`, focuser `move`, switch `setswitch`, `connected` and `action`, and checks the 403 never reaches the driver.

### Added (CI)

- **A weekly check fails an unfinished Adoption row whose issue is closed** (CI, issue #2716): `scripts/check_docs_drift.py --adoption-issues` asks GitHub for the state of the issue each `partial` or `planned` Adoption row names, and `link-check.yml` runs it every week.

### Changed

- **iOptron async slews supersede instead of joining**, iOptron telescope driver, decision 0006 rule 3, issue #2716. A newer `SlewToCoordinatesAsync` or `SlewToAltAzAsync` no longer cancels and joins the older slew body before it starts its own: its motion claim supersedes that body, which checks the claim under the operation lock before it sends anything, so an older GOTO either reached the mount before the newer claim or never does (open-astro#768). `AbortSlew`, disconnect and the destructor still cancel and join the slot. Covered by `AlpacaCore/tests/test_ioptron_async_slew_failure.cpp`.

### Changed (tests)

- **Decision-record checks now fail on the breaks they name**: tests, decisions 0002, 0005 and 0025. `test_software_update` requires the update polkit rule's only `YES` to be the body of the `start` verb branch, `test_thread_join` fails on a raw `.join()` in `server.cpp`, and two Sky-Watcher rate-check cases step the fake clock instead of sleeping.

### Changed (docs)

- **Decision records agree with the code and with each other again** (docs, issue #2716): an audit of the 27 records fixed contradictions in 0005, 0006, 0010, 0016, 0021 and 0027, stale text in 0007, 0009, 0020, 0023 and 0024, and Adoption rows that named closed issues; decision 0027 is `accepted`; 0022 rule 2 now says an abort whose stop failed leaves the hold window armed; 0009 names a TCP peer hangup as a lost link (issue #2718) and marks the rows that keep no `LinkLostStamp` as `partial` (issue #2717); the README no longer allows `Scope: **` in 0013.

### Fixed

- **SVBONY exposure readout ends at the session watchdog**, SVBONY camera driver, decision 0020 rule 4, issue #2716. The frame-read loop now passes through `CaptureControl::wait_for` on every read, so the exposure ends at its watchdog deadline even when no client polls, instead of reading on to its own frame timeout. Covered by `AlpacaCore/tests/test_svbony_fake_sdk.cpp` ("SVBONY Camera - the watchdog deadline ends the readout loop with no getter polling").
- **Player One switch and PW filter wheel, ToupTek AAF focuser, AFW filter wheel and thermal switch** (AlpacaCore, decision 0009, issue #2716): while a device's link fault is latched, the management listing and web UI now show the fault text instead of an empty one, so a connected device that stopped answering shows as faulted, not healthy. The text clears with the next good reply.
- **A device Name never keeps a bracket** (AlpacaCore, decision 0024 rule 1, issue #2716): `util::without_qualifiers()` now removes a `(...)` qualifier that has no space before it and cuts the Name at an unclosed `(`, so an OnStepX product description such as `EQ6(belt)` gives `OnStep EQ6`; the gphoto camera Name gets the same guarantee.

### Security

- **Every Device API PUT now carries the cross-origin guard** (AlpacaHTTP, decision 0007 rule 2, issue #2716). A PUT to `/api/v1/<type>/<n>/<method>` whose `Origin` host differs from `Host` gets HTTP 403 with `ErrorMessage` "Cross-origin device method requests are not allowed" before the driver runs; before, only the wrong-verb path, `UTCDate` and the three `Site*` setters were guarded, so a slew, exposure, switch or `Connected` PUT reached the driver. Requests with no `Origin` header (NINA, ConformU, other native clients) and same-origin web UI requests are unchanged, and GETs are never refused. The `UTCDate` and `Site*` refusals now say "device method" instead of naming the member.
