### Breaking changes

- **ZWO AM: a Park the mount ignores now reports a failure instead of AtPark true** (AlpacaCore, issue #2097): on an AM3, AM5 or AM7 whose Park does not move the mount while `:Gps#` answers not parked, `AtPark` and `Slewing` now raise `Park failed` about 5 s after Park, where they used to report the mount parked. A client that relied on `AtPark` after such a Park now gets an error; the mount was never parked, so check it before treating it as safe. The AM5N, and a mount that does not answer `:Gps#`, keep the previous behaviour.

### Fixed

- **ZWO AM: Park now parks a tracking AM5** (AlpacaCore, issue #2097): an AM5 on firmware 1.8.8 ignores `:hP#` while tracking is on, so Park did nothing from a tracking state, which is how clients usually call it. Park now turns tracking off (`:Td#`) before `:hP#`; on the rig the AM5 then parked from Dec 65 in about 6 s. A parked mount does not track, so Tracking reads false after Park as before.
- **ZWO AM: AtPark no longer reports parked when Park did nothing** (AlpacaCore, issue #2097): the driver took a still mount about 5 s after `:hP#` as parked on every model, even while `:Gps#` said not parked. It now infers a park from stillness only on the AM5N, after motion was seen, or when `:Gps#` is unavailable; otherwise the Park fails and AtPark and Slewing keep the error until the next command, AbortSlew or a reconnect. For the client-visible change, see Breaking changes.

### Added (tests)

- **ZWO Park cases** (issue #2097): four `[park]` cases in `test_zwo_telescope_clock.cpp` (Park stops tracking before `:hP#`; ignored Park on AM3/AM5/AM7 with `:Gps#` 0 and 1; AM5 without a usable `:Gps#`; AM5 Park that moved and then stopped), the AM5 variant of the forced-window Park completion case, and a `FakeZwoMount::set_ignore_park_while_tracking()` knob. The unavailable-`:Gps#` variants of the inferred-completion case now answer `e1`, the AM5N's reply, instead of silence: a silent query leaves the next `:GU#` cut to one byte by the owed-reply fence, which tracking-off Park exposes.
