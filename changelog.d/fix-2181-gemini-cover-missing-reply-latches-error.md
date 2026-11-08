### Fixed

- **Gemini flat panel cover missing reply**, Gemini flat panel driver, issue #2181. A cover open/close whose reply never arrives (read timeout) no longer latches `CoverState` at Error although the cover moved: `CoverState` reads the live `>S#` status, and a failed status read after it is Error. Refusals, unexpected replies and a lost link still report Error until the next cover command. Covered by `AlpacaCore/tests/test_gemini_flatpanel.cpp` (a cover reply that never arrives; after a missing cover reply).
