# Rotator Sync offset persistence

Status: accepted

## Context

ASCOM IRotatorV4 `Sync` sets an offset between `Position` and `MechanicalPosition`, and the offset must survive a driver restart and a device reboot. The WandererAstro rotator and the ZWO CAA kept it in memory and reset it to 0 on every connect, so `Position` fell back to the mechanical angle after a reconnect (issue #777, SP-19a). Neither device stores the offset itself.

## Decision

One helper, `alpacacore::util::RotatorSyncOffsetStore` (`AlpacaCore/include/alpacacore/util/rotator_sync_offset_store.h`), keeps the offsets in a JSON object in `config/rotator_sync_offsets.json`, relative to the working directory. Key: the driver `get_unique_id()` (ZWO: read after the serial number is resolved at connect). Value: the offset in degrees.

- `Sync` saves the new offset (temp file, then rename; other keys kept; one mutex for all calls).
- Connect loads it; no entry, a missing file or a corrupt file gives 0.
- A storage failure logs a WARNING and never fails `Sync` or Connect.
- AlpacaCore has no JSON library, so the helper reads and writes only a flat object of numbers.
- Tests set the path with `RotatorSyncOffsetStore::set_default_path()` and restore it; factory signatures do not change.

## Rules

The rules a change is checked against. Each names where it applies and what checks it; the reasoning is in the Decision above.

1. A rotator `Sync` saves its offset through `util::RotatorSyncOffsetStore`, keyed by `get_unique_id()`, and Connect loads it. Scope: `AlpacaCore/src/vendors/wandererastro/wandererastro_rotator_driver.cpp`, `AlpacaCore/src/vendors/zwo/zwo_rotator_driver.cpp`. Checked by: `AlpacaCore/tests/test_rotator_sync_offset_store.cpp`, review.
2. A storage failure logs a WARNING and never fails `Sync` or Connect; a missing or corrupt file reads as 0. Scope: `AlpacaCore/src/core/util/rotator_sync_offset_store.cpp`. Checked by: `AlpacaCore/tests/test_rotator_sync_offset_store.cpp`.
3. The offset never passes through the device config or the router. Scope: `AlpacaHTTP/src/http/router.cpp`. Checked by: review.

## Alternatives rejected

- A field in the device config (`registered_devices.json`): the offset would pass through the router, `sanitize_device_config` and a config write on every `Sync`, and mixes a runtime calibration with user settings.
- A constructor parameter for the path: changes every factory and the router for a test-only need.
- Writing the offset to the device: neither device has a field for it.

## Consequences

- The ZWO CAA change has no hardware-free test (the driver has no SDK seam); the helper test and review cover it until the seam slice lands.
- Moving the state file or changing the unique id orphans a saved offset; the rotator then reads 0 until the next `Sync`.
- The Wanderer key is the device number: deleting a Wanderer rotator and adding a different one at the same number inherits the old offset.
- To reverse: remove the load and save calls in the two drivers and delete the helper; nothing else reads the file.

## Links

- Issue #777 (SP-19a).
