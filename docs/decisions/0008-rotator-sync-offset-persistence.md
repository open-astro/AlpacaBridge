# Rotator Sync offset persistence

Status: accepted

## Context

ASCOM IRotatorV4 `Sync` sets an offset between `Position` and `MechanicalPosition`, and the offset must survive a driver restart and a device reboot. The WandererAstro rotator and the ZWO CAA kept it in memory and reset it to 0 on every connect, so `Position` fell back to the mechanical angle after a reconnect (issue #777, SP-19a). Neither device stores the offset itself.

## Decision

One helper, `alpacacore::util::RotatorSyncOffsetStore` (`AlpacaCore/include/alpacacore/util/rotator_sync_offset_store.h`), keeps the offsets in a JSON object in `config/rotator_sync_offsets.json`, relative to the working directory. Key: the driver `get_unique_id()` (ZWO: read after the serial number is resolved at connect). Value: the offset in degrees.

- `Sync` saves the new offset (temp file, then rename; other keys kept; one mutex for all calls).
- Connect loads it; no entry, a missing file or a corrupt file gives 0.
- If `Sync` cannot save the new offset, it throws `DriverException` and keeps the previous offset, so `Position` and the stored offset still agree. ASCOM requires the offset to persist, so a `Sync` that cannot persist it has failed.
- A Connect that cannot read the file logs a WARNING and uses 0; Connect never fails for the offset.
- Removing a rotator (`removedevice`) deletes its entry from the file, so a new device that later takes the same device number starts with `Position` equal to `MechanicalPosition`.
- AlpacaCore has no JSON library, so the helper reads and writes only a flat object of numbers.
- Tests set the path with `RotatorSyncOffsetStore::set_default_path()` and restore it; factory signatures do not change.

The `Sync` failure and the delete on removal were decided 2026-10-21 on issue #1470 (item 3). Before that, a storage failure logged a WARNING and `Sync` still succeeded, and removal left the entry in the file.

## Rules

The rules a change is checked against. Each names where it applies and what checks it; the reasoning is in the Decision above.

1. A rotator `Sync` saves its offset through `util::RotatorSyncOffsetStore`, keyed by `get_unique_id()`, and Connect loads it. Scope: `AlpacaCore/src/vendors/wandererastro/wandererastro_rotator_driver.cpp`, `AlpacaCore/src/vendors/zwo/zwo_rotator_driver.cpp`. Checked by: `AlpacaCore/tests/test_rotator_sync_offset_store.cpp`, review.
2. A `Sync` whose save fails throws `DriverException` and leaves the previous offset in place. A Connect that cannot read the file uses 0 and logs a WARNING; a missing or corrupt file reads as 0. Scope: `AlpacaCore/src/core/util/rotator_sync_offset_store.cpp`, `AlpacaCore/src/vendors/wandererastro/wandererastro_rotator_driver.cpp`, `AlpacaCore/src/vendors/zwo/zwo_rotator_driver.cpp`. Checked by: `AlpacaCore/tests/test_rotator_sync_offset_store.cpp` for the read side; review for the `Sync` side, until issue #1473 adds the test cases.
3. The offset value never passes through the device config or the router. The router's only use of the store is to delete a removed rotator's entry. Scope: `AlpacaHTTP/src/http/router.cpp`. Checked by: review, until issue #1473 adds the removal test case.

## Alternatives rejected

- A field in the device config (`registered_devices.json`): the offset would pass through the router, `sanitize_device_config` and a config write on every `Sync`, and mixes a runtime calibration with user settings.
- A constructor parameter for the path: changes every factory and the router for a test-only need.
- Writing the offset to the device: neither device has a field for it.

## Consequences

- The ZWO CAA change has no hardware-free test (the driver has no SDK seam); the helper test and review cover it until the seam slice lands.
- Moving the state file or changing the unique id orphans a saved offset; the rotator then reads 0 until the next `Sync`.
- The Wanderer key is the device number until issue #1471 gives every device a persisted GUID. Until the delete on removal lands (issue #1473), deleting a Wanderer rotator and adding a different one at the same number inherits the old offset.
- Issue #1471 changes every key. Its migration must rename each stored entry from the old unique id to the new GUID, or every rotator reads 0 after the upgrade. Land #1471 before #1473.
- A client whose `Sync` used to succeed on a read-only or full disk now gets `DriverException`.
- To reverse: remove the load and save calls in the two drivers and delete the helper; nothing else reads the file.

## Links

- Issue #777 (SP-19a); issues [#1470](https://github.com/open-astro/AlpacaBridge/issues/1470) (item 3), [#1471](https://github.com/open-astro/AlpacaBridge/issues/1471), [#1473](https://github.com/open-astro/AlpacaBridge/issues/1473).
