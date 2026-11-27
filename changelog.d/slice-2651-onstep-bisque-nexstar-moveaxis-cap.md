### Changed

- **The OnStep, Bisque and NexStar 60 s MoveAxis cap runs on the shared motion guard** (issue #2651, second part; ZWO AM follows): each axis has a slot body, `util::run_motion_deadline()`, and the three drivers use `util::kMaxMoveAxisDuration` as their 60 s. A cap stop the mount ignores is sent three times, then Slewing raises `util::kStopNotVerifiedText` and nothing more goes out for that move; OnStep and NexStar verify each axis alone, Bisque's `DoCommand(10)` verifies both. Tests: the `[goto-safety]` MoveAxis cap cases in `test_onstep_telescope.cpp`, `test_bisque_telescope.cpp` and `test_celestron_async_slew_failure.cpp`.

### Fixed

- **A Celestron NexStar FindHome over a MoveAxis no longer leaves Slewing true** (issue #2651): FindHome clears the jog flags the home took from the jog, so Slewing reads false once the home lands and the cap sends no stop at the jog's 60 s. Test: "Celestron MoveAxis - a FindHome over a jog reads not slewing once home and leaves no cap".
- **An OnStep MoveAxis is stopped at 60 s on firmware that sends no `g` status** (issue #2651): the cap no longer rides on the status re-send body. Test: "OnStep Telescope Driver - a MoveAxis on firmware without status g is still stopped at 60 s".
- **An OnStep or Celestron NexStar MoveAxis whose 60 s cap cannot start stops the axis and fails** (issue #2651): on OnStep the refusal was taken for a newer MoveAxis, because the re-send body's start had moved the generation it compared against, so the jog ran on with no cap and Slewing read true. Tests: "OnStep Telescope Driver - a MoveAxis whose cap cannot start stops the axis and fails", "Celestron MoveAxis - a cap the slot cannot start stops the axis and fails the MoveAxis".
