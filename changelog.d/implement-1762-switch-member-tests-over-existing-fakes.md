### Added (tests)

- **Switch member tests over the existing fakes**, Gemini PDH, WandererAstro box and ToupTek thermal switch drivers, issue #1762. Connected cases now call `GetSwitchDescription`, `SetSwitchName`, `StateChangeComplete`, `SetAsync` and `SetAsyncValue` on a writable and a read-only id and on out-of-range ids, in `AlpacaCore/tests/test_gemini_pdh_switch.cpp`, `test_wandererastro_box_switch.cpp` and `test_touptek_thermal_switch.cpp`. Tests only; no driver changes.
