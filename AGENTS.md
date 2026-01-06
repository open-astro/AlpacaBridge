# Agent Instructions

This repository uses Cursor rules as the source of truth for agent workflows.
Read and follow these before making changes:

- `AlpacaCore/.cursor/rules/rules.mdc`
- `AlpacaCore/.cursor/rules/driver_build.mdc`
- `AlpacaCore/.cursor/rules/driver_test.mdc`
- `AlpacaHTTP/.cursor/rules/rules.mdc`

Additional project notes:
- ZWO ROI sizing rules: width must be a multiple of 8 and height a multiple of 2 after binning. Keep requested sizes for Alpaca, align effective sizes down for SDK calls, and pad outputs if needed.
- ZWO dew heater is exposed as an Alpaca Switch device (not a camera action) and is camera-dependent.
- ST4 pulse guiding should be enabled only when the SDK reports `has_st4_port`.
- ConformU logs live under `AlpacaCore/conformu/`; Windows logs are prefixed with `W-` for comparison.
- On macOS, the ZWO SDK links against libusb even if the camera appears in System Report.
- When building a driver, also build the test targets so they can be exercised via `run_all_tests.cmd` and `run_all_tests.sh`.
