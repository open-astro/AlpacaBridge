### Changed

- **Fake ConformU: Celestron and Astroasis focusers**, test fakes, issue #2533 (part of #2078). `FakeCelestronFocuser` and `FakeHidTransport` gain an opt-in travel mode: a Move reaches its target over time and a Halt stops it where it is. The fake ConformU factories turn it on and the run list serves both focuser rows; existing tests keep the explicit default. Covered by the new travel and halt cases in `test_celestron_focuser.cpp` and `test_astroasis_focuser.cpp`.
