### Changed

- **GPhoto bulb and mirror lock-up tests wait through the fake task clock** (GPhoto, issue #1587): the bulb hold and file-wait cases in `test_gphoto_fake_sdk.cpp` advance a `FakeTaskClock` from the fake SDK's `drain_events` and `poll_bulb_file_and_download` hooks instead of waiting in real time, and the two stop cases wait for the first hold slice instead of a fixed `sleep_for`; the cases that only an abort ends cap the hold slices and fail on `CHECK_FALSE(hold.past_cap)` instead of spinning; test-only, no production change.
