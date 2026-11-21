### Fixed

- **Player One camera `Name` no longer calls the SDK** (Player One driver, issue #1994): `Name` on a disconnected camera used to enumerate cameras inside the getter and could wait behind another SDK user. It now reads a name cached at start-up and at connect, kept across disconnect. Covered by `Player One Camera Driver - Name on a disconnected camera never enumerates` and `... Name survives connect and disconnect` in `AlpacaCore/tests/test_playerone_camera.cpp`.
