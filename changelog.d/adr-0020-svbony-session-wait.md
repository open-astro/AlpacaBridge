### Fixed

- **SVBONY exposure readout ends at the session watchdog**, SVBONY camera driver, decision 0020 rule 4. The frame-read loop now passes through `CaptureControl::wait_for` on every read, so the exposure ends at its watchdog deadline even when no client polls, instead of reading on to its own frame timeout. Covered by `AlpacaCore/tests/test_svbony_fake_sdk.cpp` ("SVBONY Camera - the watchdog deadline ends the readout loop with no getter polling").
