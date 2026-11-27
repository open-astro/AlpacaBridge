### Fixed

- **Gemini focuser fake ConformU report is now the arm64 CI run** (Gemini, issue #2695). The committed `Fake-focuser-fake_gemini_focuser-Linux-arm64.txt` reported `Processor is Intel/AMD 64bit`; it is now the arm64 CI artifact (`Processor is ARM 64bit`). The `TempComp` changelog entry no longer says x64 and lists all seven new cases, and the `state_` comment in `gemini_focuser_driver.cpp` names `TempComp`. No behaviour change.
