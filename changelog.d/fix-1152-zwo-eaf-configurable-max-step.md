### Added

- **ZWO EAF focuser: configurable Max Step**, ZWO focuser driver and web UI, issue #1152. The new optional `maxStep` setting (1 to 2000000, blank in the web UI = not set) is written to the unit at connect with `EAFSetMaxStep` when it differs from the stored value, and `MaxStep` then reports it; an equal value is not written and an unset one leaves the stored value alone. The EAF stops moves at its stored max step silently, so a driver-only limit would not have held. A `maxStep` above the unit's `EAFStepRange` refuses the connect with the range in the message, and so does a unit that stores max step 0 with no `maxStep` set (see Breaking changes). Covered by `AlpacaCore/tests/test_zwo_focuser.cpp` (written once, equal not written, unset untouched, above range refused, unavailable range not refusing) and `AlpacaCore/tests/test_catalog_builtins.cpp` (range and absent-stays-absent).

### Breaking changes

- **ZWO EAF focuser with a stored max step of 0 no longer connects until Max Step is set.** The connect now fails with "The EAF reports max step 0. Set Max Step in the focuser's device settings." instead of succeeding and refusing every `Move` with "Focuser position out of range". Upgrade step: open the focuser's device settings in the web UI and enter a Max Step (up to the unit's step range).
