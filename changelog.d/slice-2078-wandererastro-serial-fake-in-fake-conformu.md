### Changed

- **WandererAstro SFW filter wheel in the fake ConformU run**, WandererAstro filter wheel, issue #2597. The test serial fake `FakeSerialStreamer` has an opt-in SFW wheel mode: a move command `2001`-`2008` changes the streamed position to that slot after a set travel time (off by default, so the fixed frame is unchanged). Covered by the two `FakeSerialStreamer` cases in `AlpacaCore/tests/test_wandererastro_filterwheel.cpp`. The SFW row of `scripts/conformu_fake_runs.tsv` is `run`; the cover calibrator and switch rows stay `pending` with the reason named.
