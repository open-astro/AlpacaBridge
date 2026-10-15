### Changed (tests)

- **Gemini focuser stress cases run over the pty-backed fake** (AlpacaCore tests, issue #1067): the lifecycle and destruction `[stress]` cases in `AlpacaCore/tests/test_gemini_concurrency_stress.cpp` build the driver on `FakeGeminiFocuser` instead of an absent port, the lifecycle case proves a connect before and a disconnect after the storm, and a new "racing disconnect is never dropped" case asserts the driver settles disconnected after a connect followed at once by a disconnect.
