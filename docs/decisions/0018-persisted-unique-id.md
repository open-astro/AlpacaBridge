# Persisted UniqueID per configured device

Status: proposed

## Context

The Management API's configured-devices list gives every device a `UniqueID`. ASCOM says it should be "a GUID for native Alpaca devices" ([AlpacaConfiguredDevice.UniqueID](https://ascom-standards.org/library/html/P_ASCOM_Alpaca_Discovery_AlpacaConfiguredDevice_UniqueID.htm)). Clients use it to recognise a device they have seen before, across restarts and across a change of device number.

AlpacaBridge serves it from each driver's `get_unique_id()` (`DeviceRegistry`, `AlpacaCore/src/management/device_registry.cpp`). On `main` 313cf184, 25 vendor drivers build that string from the device number (for example `"ASTROASIS_FOCUSER_" + std::to_string(device_number_)` in `astroasis_focuser_driver.cpp`). Two AlpacaBridge hosts on one LAN both report `SkyWatcher_0`, and renumbering a device changes its ID. Other drivers use a hardware serial (ZWO `ZWO_SN_<serial>`, ToupTek), and the ZWO camera stores a generated `ZWO_UID_<16 hex>` in its config when the camera has no serial (#914, #1166). A device that failed to load is listed with an empty `UniqueID`.

The rotator sync offset store is keyed by `get_unique_id()` ([decision 0008](0008-rotator-sync-offset-persistence.md)), so the ID is also an on-disk key.

Decided 2026-10-21 on issue #1470 (item 1): every configured device gets a GUID stored in its config, and existing devices get a new GUID once on upgrade.

## Decision

- **One ID per configured device.** Each entry in the persisted device config carries a `uniqueId` field holding a random (version 4) UUID in its canonical lowercase text form. It is created when the device is first configured and never changes while the entry exists. Editing the entry, renumbering it or replacing the hardware behind it keeps it. Removing the entry ends it; a device added later gets a new one.
- **The server owns it.** A `uniqueId` a client sends to `configuredevice` is ignored: a new entry gets a fresh GUID and an edited entry keeps its stored one. `sanitize_device_config` keeps the field for every device type.
- **Every reader sees the same value.** The router passes the stored GUID to the driver it constructs, and `get_unique_id()` returns it, so `configureddevices`, logs and the rotator offset store all use one ID. A device that failed to load is listed with its stored GUID, not an empty string.
- **Hardware identity stays separate.** A serial number or SDK index still selects which physical device a driver binds to. It no longer forms the `UniqueID`.
- **Upgrade.** On the first start of a version with this rule, every entry without a `uniqueId` gets one and the config is written back once. The ZWO camera's stored `ZWO_UID_` value is replaced like any other ID. The same step renames each rotator sync offset entry from the old ID to the new GUID, which is why issue #1471 lands before issue #1473 ([decision 0008](0008-rotator-sync-offset-persistence.md), Consequences).

## Rules

The rules a change is checked against. Each names where it applies and what checks it; the reasoning is in the Decision above.

1. Every persisted device entry has a server-generated version 4 UUID in `uniqueId`, kept across edits and renumbering, and a client-supplied value is ignored. Scope: `AlpacaHTTP/src/http/router.cpp`. Checked by: review, until issue #1471 adds the config round-trip cases.
2. `get_unique_id()` returns the GUID the router passed in; no driver builds it from the device number, an index or a serial. Scope: `AlpacaCore/src/vendors/**`. Checked by: review, until issue #1471 adds a contract-sweep case.
3. `configureddevices` lists a device that failed to load with its stored GUID. Scope: `AlpacaHTTP/src/http/router.cpp`. Checked by: review, until issue #1471 adds the case.
4. The upgrade step that assigns GUIDs also renames the rotator sync offset entries, in the same release. Scope: `AlpacaHTTP/src/http/router.cpp`, `AlpacaCore/src/core/util/rotator_sync_offset_store.cpp`. Checked by: review, until issue #1471 adds the migration case.

## Alternatives rejected

- **Keep the device-number IDs.** They collide across hosts and change on renumbering, which is what the ID exists to prevent.
- **Derive the ID from a hardware serial where one exists.** It follows the camera rather than the configured device, half the supported hardware has no serial, and a swapped camera would silently inherit another device's identity in a client.
- **Hash the host name and device number.** Stable across restarts, but it still changes on renumbering, and a renamed host changes every ID.
- **Keep old IDs for existing devices and use GUIDs only for new ones.** Two ID schemes would stay in the code indefinitely, and the existing ones keep colliding.

## Consequences

- Every existing device reports a new `UniqueID` once, after the upgrade. A client that stored the old ID sees a new device and must be pointed at it once. The changelog entry for issue #1471 goes under Breaking changes.
- Restoring an old config backup brings back the GUIDs it held; copying one config to a second host gives both hosts the same GUIDs, so a cloned SD card should be re-configured.
- The ZWO camera's own generated-ID code becomes redundant and is removed with issue #1471.
- No driver on `main` follows this record yet; issue #1471 implements it.

## Links

- Issues [#1470](https://github.com/open-astro/AlpacaBridge/issues/1470) (item 1), [#1471](https://github.com/open-astro/AlpacaBridge/issues/1471); earlier ZWO work [#914](https://github.com/open-astro/AlpacaBridge/issues/914), [#1166](https://github.com/open-astro/AlpacaBridge/issues/1166).
- [Decision 0008](0008-rotator-sync-offset-persistence.md) (offset store keyed by the ID).
- `.claude/skills/ascom-alpaca-protocol/references/management-and-discovery.md` (UniqueID requirements).
