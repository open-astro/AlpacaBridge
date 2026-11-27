# Hold window: adoption

The roster of [decision 0022](../decisions/0022-hold-window.md). It is kept out of the record so a PR that
adds or changes a row needs no `ADR authorized` comment; `scripts/check_docs_drift.py` check 23
compares it with the code.

## Adoption

The roster of telescope drivers whose slew and pulse deadlines are `util::HoldWindow`s. `scripts/check_docs_drift.py` check 23 holds it to the code.

Scan: `AlpacaCore/src/vendors/*/*telescope_driver.cpp`
Symbol: `HoldWindow`

| Driver | Source | State | Issue | Notes |
| --- | --- | --- | --- | --- |
| Sky-Watcher telescope | `AlpacaCore/src/vendors/skywatcher/skywatcher_telescope_driver.cpp` | adopted | #1798 | pilot (#1815); slew window and per-axis pulse windows |
| iOptron telescope | `AlpacaCore/src/vendors/ioptron/ioptron_telescope_driver.cpp` | partial | #1831 | slew window and pulse windows (rules 1, 4; rule 2 except the clear on landing); landing (the poll's `advance_motion_locked()` and the Park/FindHome motion end, #2491) still waits on the 5 s window and does not clear it (rules 2 and 3) until a rig shows `:GLS#` reports the GOTO at once; the poll stamps each frame with `status_sampled_at` and `get_slewing()` stays true until a frame sampled after the window closed is published (at most one poll) |
| Bisque telescope | `AlpacaCore/src/vendors/bisque/bisque_telescope_driver.cpp` | partial | #2719 | slew window only, no pulse deadline (rules 1, 2: armed before `SlewToRaDec` goes out and put back as it was when TheSkyX refuses it; cleared on AbortSlew, connect, disconnect and the SlewToCoordinates wait's landing); landing (the status poll's `slew_poll_due_locked()` and the retarget guard `own_slew_in_flight()`) asks `IsSlewComplete` only once the 8 s window has closed (rule 3), so an async slew lands no sooner than 8 s, until a TheSkyX run shows `IsSlewComplete` reports a slew at once |
| Celestron telescope | `AlpacaCore/src/vendors/celestron/celestron_telescope_driver.cpp` | partial | #1833 | slew window, per-axis pulse windows and the pulse position holds and corrections (rules 1, 4); rule 2 except the clear on landing (an AbortSlew whose stop fails keeps every window armed, and Slewing raises the stored stop failure); landing in SlewToCoordinates and Park (`poll_hardware_slewing()`) still waits on the 8 s window and clears it no sooner (rules 2 and 3), while the async GOTO body already lands on the mount's `L`, until a rig shows `L` reports a GOTO at once |
| ZWO telescope | `AlpacaCore/src/vendors/zwo/zwo_telescope_driver.cpp` | partial | #1834 | slew window and one pulse window per axis (rules 1, 4); rule 2 except the clear on landing; landing (GOTO completion and the forced stop flag in `refresh_cached_values()`, Park and FindHome completion) still waits on the 5 s / 2 s window and does not clear it (rules 2 and 3) until a rig shows the first `:GU#` after `:MS#`, `:hC#` and `:hP#` reports motion |
| OnStep telescope | `AlpacaCore/src/vendors/onstep/onstep_telescope_driver.cpp` | adopted | #1835 | slew window and per-axis pulse windows |
| SynScan telescope | `AlpacaCore/src/vendors/synscan/synscan_telescope_driver.cpp` | partial | #1836 | slew window (8 s, or `kRefineStartForce` for a landing re-GOTO) and per-axis pulse windows (rules 1, 4); rule 2 except the clear on landing (a failed AbortSlew stop or MoveAxis send keeps a superseded GOTO's window: that GOTO went out and may still be running); landing (`poll_hardware_slewing_locked()`, which the slew, refinement and Park waits use) still waits on the window and does not clear it (rules 2 and 3), so no GOTO or Park lands sooner than 8 s, until a rig shows `L` reports a GOTO at once; `position_override_until_`, the served-position override after a sync or a pulse, is a position cache, not a Slewing or IsPulseGuiding window, and stays a raw time point |
| remotealpaca telescope | `AlpacaCore/src/vendors/remotealpaca/remotealpaca_telescope_driver.cpp` | adopted | #2710 | the MoveAxis cap's 60 s deadline only (`util::run_motion_deadline()`); no slew or pulse hold window: a slew, Park, FindHome or MoveAxis writes `Slewing` true and a PulseGuide `IsPulseGuiding` true into the polled frame ([decision 0010](../decisions/0010-snapshot-served-state.md) rule 2), and the next poll serves the upstream's own answer, so nothing lands on a window (rule 3) |
