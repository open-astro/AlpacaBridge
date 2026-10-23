# Persisted UniqueID per configured device

Status: accepted

## Context

The Management API's configured-devices list gives every device a `UniqueID`. ASCOM says it should be "a GUID for native Alpaca devices" ([AlpacaConfiguredDevice.UniqueID](https://ascom-standards.org/library/html/P_ASCOM_Alpaca_Discovery_AlpacaConfiguredDevice_UniqueID.htm)). Clients use it to recognise a device they have seen before, across restarts and across a change of device number.

AlpacaBridge serves it from each driver's `get_unique_id()` (`DeviceRegistry`, `AlpacaCore/src/management/device_registry.cpp`). On `main` 313cf184, 25 vendor drivers build that string from the device number (for example `"ASTROASIS_FOCUSER_" + std::to_string(device_number_)` in `astroasis_focuser_driver.cpp`). Two AlpacaBridge hosts on one LAN both report `SkyWatcher_0`, and renumbering a device changes its ID. Other drivers use a hardware serial (ZWO `ZWO_SN_<serial>`, ToupTek), and the ZWO camera stores a generated `ZWO_UID_<16 hex>` in its config when the camera has no serial (#914, #1166). A device that failed to load is listed by `faileddevices` with an empty `UniqueID`.

The rotator sync offset store is keyed by `get_unique_id()` ([decision 0008](0008-rotator-sync-offset-persistence.md)), so the ID is also an on-disk key.

Decided 2026-10-21 on issue #1470 (item 1): every configured device gets a GUID stored in its config, and existing devices get a new GUID once on upgrade.

Amended 2026-10-22 on issue #1507: the Alpaca API Reference (section 5.7, item 5) says that once a UniqueID is assigned to a device it must never change, and item 5b says it is kept across power-down. The first version replaced a stored value that was not a GUID, such as the ZWO `ZWO_UID_`, which broke that rule. A stored value is now kept in any keepable form (see Decision).

## Decision

- **One ID per configured device.** Each entry in the persisted device config carries a `uniqueId` field. The server mints it as a random (version 4) UUID in its canonical lowercase text form when the device is first configured, and it never changes while the entry exists, on edit, restart or upgrade. Editing the entry, renumbering it or replacing the hardware behind it keeps it. Removing the entry ends it; a device added later gets a new one.
- **The server chooses it, and keeps what is stored.** A value is *keepable* (`util::is_keepable_unique_id()`) when it is a version 4 UUID, or 12 to 128 printable ASCII characters that are not a device-number ID (`<PREFIX>_<up to 5 decimal digits>`, the slot IDs the drivers built from the device number, such as `SkyWatcher_0` or `ASTROASIS_FOCUSER_3`). `ZWO_UID_<16 hex>` and `ZWO_SN_<serial>` are keepable. On `configuredevice` the router keeps a `uniqueId` the client sends when it is keepable and no other stored entry holds it; otherwise it mints a GUID. The web UI edits a device by removing the entry and adding it again, so the form sends the stored ID back, and the removal has just freed it. Only an entry with no ID, a device-number ID, or an ID another entry holds gets a new GUID, once. `sanitize_device_config` keeps the field for every device type.
- **Every reader sees the same value.** The router calls `AlpacaDriver::assign_unique_id()` with the stored ID before it registers the driver, and the base class's non-virtual `get_unique_id()` returns it, so `configureddevices`, logs and the rotator offset store all use one ID. A driver keeps a `fallback_unique_id()`, answered only when no ID was assigned, which happens only for a driver a unit test builds directly. A device that failed to load is listed by `faileddevices` with its stored ID, not an empty string.
- **Hardware identity stays separate.** A serial number or SDK index still selects which physical device a driver binds to. It no longer forms the `UniqueID`.
- **Upgrade.** On the first start of a version with this rule, every entry without a keepable `uniqueId` gets a GUID, an ID two entries share is replaced in the later one, and the config is written back once. A keepable stored value, including the ZWO camera's `ZWO_UID_`, is kept as it is. The ZWO catalog does not declare, read or learn the key; the router keeps the stored value and serves it as the `UniqueID`.
- **Rotator offsets follow the persisted ID.** The offset saved under a rotator's old ID moves to the persisted ID (a GUID, or a kept value such as `ZWO_UID_`) once (`RotatorSyncOffsetStore::load_or_adopt()`). An old ID built from the device number or SDK ID names a slot, so it moves at the start that gives the stored entry its first GUID: the router calls `AlpacaDriver::adopt_legacy_state()` on those drivers after it registers them, and on no other, and only when it could write the new GUIDs to the device file. A device configured later already holds a GUID and never adopts, so it cannot inherit what a removed device saved under the same number. The ZWO CAA's serial-based old ID names the body and is built from the serial the connect reads, so it moves at connect. This is why issue #1471 and issue #1473 land together ([decision 0008](0008-rotator-sync-offset-persistence.md)).

## Rules

The rules a change is checked against. Each names where it applies and what checks it; the reasoning is in the Decision above.

1. Every persisted device entry has a keepable `uniqueId`, never changed once stored, across edits, restarts, upgrades and renumbering. A stored or client-sent value is kept when it is keepable and no other entry holds it; only a missing, device-number or duplicate value gets a minted GUID. Scope: `AlpacaHTTP/src/http/router.cpp`. Checked by: `AlpacaHTTP/tests/test_routing.cpp` ("every configured device gets a persisted GUID as its UniqueID", "a stored entry gets a GUID once at load and keeps it across restarts", "a stored entry that fails to load still holds its GUID against a new device", "two configuredevice calls sending one unused GUID do not both keep it", "re-configuring a stored entry at its own number keeps the GUID it sends back"), `AlpacaCore/tests/test_unique_id.cpp` (the keepable rule).
2. The router assigns the stored ID before it registers a driver, and `get_unique_id()` is not virtual; a driver's own ID is only `fallback_unique_id()`. Scope: `AlpacaCore/include/alpacacore/alpacadriver.h`, `AlpacaCore/src/vendors/**`. Checked by: `AlpacaHTTP/tests/test_routing.cpp` (the UniqueID listed equals the stored ID), the compiler (an override of `get_unique_id()` does not build), review.
3. `faileddevices` lists a device that failed to load with its stored ID (since #576 `configureddevices` lists only loaded devices). Scope: `AlpacaHTTP/src/http/router.cpp`. Checked by: `AlpacaHTTP/tests/test_routing.cpp` (the persisted GUID case and the #576 faileddevices case).
4. The router calls `adopt_legacy_state()` only on the drivers of entries the load gave their first GUID. A rotator moves an offset saved under a device-number or SDK-ID old ID only there, and one saved under a serial-based old ID at connect. Scope: `AlpacaHTTP/src/http/router.cpp`, `AlpacaCore/src/vendors/*/*_rotator_driver.cpp`, `AlpacaCore/src/core/util/rotator_sync_offset_store.cpp`. Checked by: `AlpacaHTTP/tests/test_routing.cpp` ("a start asks only the entries it gives a first GUID to adopt their old state"), `AlpacaCore/tests/test_wandererastro_rotator.cpp`, `AlpacaCore/tests/test_zwo_rotator.cpp` (the adopt and "Connect does not adopt" cases), `AlpacaCore/tests/test_rotator_sync_offset_store.cpp`.

## Alternatives rejected

- **Keep the device-number IDs.** They collide across hosts and change on renumbering, which is what the ID exists to prevent.
- **Derive the ID from a hardware serial where one exists.** It follows the camera rather than the configured device, half the supported hardware has no serial, and a swapped camera would silently inherit another device's identity in a client.
- **Hash the host name and device number.** Stable across restarts, but it still changes on renumbering, and a renamed host changes every ID.
- **Keep old IDs for existing devices and use GUIDs only for new ones.** Two ID schemes would stay in the code indefinitely, and the existing ones keep colliding.
- **Replace a stored value that is not a GUID.** The first version did, to have one format. It changes an ID a client already stored, which the Alpaca API Reference forbids (section 5.7, item 5).
- **Ignore every client-sent value.** The web UI's edit is a remove followed by an add, so every edit would mint a new GUID and orphan a rotator's offset.
- **Move every offset at load.** The ZWO CAA's serial-based old key needs the serial, which only the connect reads.
- **Move every offset at connect.** A connect cannot tell the device that held a number at the upgrade from one configured on that number later, so a new rotator on a reused number took over the removed one's offset (review of #1493).

## Consequences

- Every existing device without a keepable stored ID reports a new `UniqueID` once, after the upgrade; one with a stored `ZWO_UID_` keeps it. A client that stored the old ID sees a new device and must be pointed at it once. The changelog entry for issue #1471 goes under Breaking changes.
- Restoring an old config backup brings back the stored IDs it held; copying one config to a second host gives both hosts the same stored IDs, so a cloned SD card should be re-configured.
- A management client that edits a device by removing and re-adding it without sending the stored ID back gets a new one; the web UI sends it.
- A client that sends the ID of a device removed earlier gets it back, since no stored entry holds it; before the next start prunes it, a rotator configured that way also reads the removed rotator's offset. It takes a deliberate client.
- The ZWO camera's catalog no longer generates `ZWO_UID_` values, but a stored one stays. The direct factories that unit tests use still do, for the fallback.
- `prune_unknown_unique_ids()` removes only GUID-shaped offset keys, so the offset of a removed rotator whose kept ID is not a GUID stays in the file.
- An offset saved under a device-number or SDK-ID old ID whose rotator was not configured, or did not load, at the upgrade start is never moved and stays in the file; pruning removes only GUID keys no entry holds.

## Links

- Issue [#1507](https://github.com/open-astro/AlpacaBridge/issues/1507) (never change an assigned UniqueID).
- Issues [#1470](https://github.com/open-astro/AlpacaBridge/issues/1470) (item 1), [#1471](https://github.com/open-astro/AlpacaBridge/issues/1471); earlier ZWO work [#914](https://github.com/open-astro/AlpacaBridge/issues/914), [#1166](https://github.com/open-astro/AlpacaBridge/issues/1166).
- [Decision 0008](0008-rotator-sync-offset-persistence.md) (offset store keyed by the ID).
- `.claude/skills/ascom-alpaca-protocol/references/management-and-discovery.md` (UniqueID requirements).
