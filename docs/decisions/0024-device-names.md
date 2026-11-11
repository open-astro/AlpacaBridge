# Device names

Status: accepted

## Context

Clients show a device's ASCOM `Name` in their choosers, and the web UI shows it as the device card's title. On `main` ab9a6f8c the drivers built it in different ways (issue #2215):

- Some appended driver internals. The Sky-Watcher direct driver returned `Sky-Watcher EQM-35 Pro (EQMOD, eps: measured)`: `(EQMOD)` told it apart from the SynScan driver's `Sky-Watcher EQM-35 Pro (SynScan)` for the same mount, and the eps source was added by #582 because the web UI had nowhere else to show it. The ASIAIR Plus switch carried its SoC (`(RK3568)`), the Celestron direct mount its transport (`USB`).
- Some returned the bare SDK string. The ZWO focuser, filter wheel and rotator returned `EAF`, `EFW` and `CAA`, the ToupTek wheel and focuser `AFW-M` and `AAF`. An EAFN read `EAF`: `EAF_INFO.Name` is `EAF` on both models, and only `EAFGetType()` says `EAFN` (rig probe 2026-11-11, EAF SDK 1.8.1, firmware 3.8.2).
- Some never read a model that was there to read. OnStep was always `OnStep Telescope`, though OnStepX answers `:GVC#` with its `PRODUCT_DESCRIPTION`.
- The unknown-model fallbacks used `Mount` in some drivers (`ZWO Mount`) and `Telescope` in others (`iOptron Telescope`).

Decided 2026-11-11 on issue #2215.

## Decision

- **Name = brand + model.** A driver's `get_name()` is the device's brand followed by its model and nothing else. Before the driver has read the model it is the brand followed by the product family it serves (`ZWO EAF`, `ToupTek AFW`) or by the ASCOM device type (`Sky-Watcher Telescope`, `WeeWX ObservingConditions`); the type noun for a mount is `Telescope`.
- **The brand is added once.** A model string read from an SDK or a device reaches the name through `util::branded_name()` (`alpacacore/util/device_name.h`), which prefixes the brand when the string does not already start with it, ignoring case: `EAFN` becomes `ZWO EAFN`, `QHY268M` and `ALTAIR178M3` stay as they are. libgphoto2's camera models already start with the camera's maker (`Nikon DSC D5300`), so they need no prefix.
- **SDK qualifiers leave the name.** Some SDKs add a connection mode to the model in parentheses (libgphoto2 names a Sony body in PC-remote mode `Sony Alpha-A7 III (Control)`). `util::without_qualifiers()` drops it from the name, and the Description keeps the model as reported.
- **The rest goes in the Description.** A connection path, protocol, board revision, calibration state or the camera a wheel hangs on goes in `get_description()`, which answers without device I/O. The router adds it to each `configureddevices` entry as `Description`, and the web UI shows it on the device card, so the eps source #582 put in the name is still visible.
- **Two drivers on one device share its name.** A mount reachable over its hand controller and over the EQMOD direct driver is `Sky-Watcher EQM-35 Pro` on both; the Description (`Sky-Watcher SynScan V3/V4 Mount Driver`, `Sky-Watcher EQMOD direct mount, eps: measured`) and the UniqueID tell the two entries apart.
- **One function of a unit keeps its function word.** The iFR focuser-rotator is `iOptron iFR 3" Focuser` and `iOptron iFR 3" Rotator`; a camera's dew heater or thermal switch is the camera model followed by `Dew Heater` or `Thermal Switch` (`ZWO ASI2600MC Pro Dew Heater`), so two cameras' heaters are told apart.
- **OnStep is the brand of OnStep firmware.** The model is OnStepX's `:GVC#` product description, the name the firmware builder gave the controller: `OnStep ` followed by it, through `util::branded_name()` so a description `OnStepX` stays `OnStepX`. Its ` (...)` qualifiers and anything after a comma leave the name. A firmware without `:GVC#` is `OnStep Telescope`.

## Rules

The rules a change is checked against. Each names where it applies and what checks it; the reasoning is in the Decision above.

1. A driver's `get_name()` is the brand followed by the model, with no connection path, protocol, board revision or driver state after it and no `(` or `,` in it; before the model is read it is the brand followed by the product family or the ASCOM device type, and a mount's type noun is `Telescope`. Scope: `AlpacaCore/src/vendors/**`. Checked by: `AlpacaCore/tests/test_contract_sweep.cpp` ("Contract sweep - every Name is brand and model while disconnected", "Contract sweep tier 2 - every Name is brand and model while connected"), `AlpacaCore/tests/test_synscan_async_park.cpp` ("SynScan - an unknown model id is named Sky-Watcher Telescope"), `AlpacaCore/tests/test_skywatcher_async.cpp` ("SkyWatcher - an unknown board is Sky-Watcher Telescope with its code in the Description"), review (a driver not in tier 2 whose connected name appends anything to the model).
2. A model string read from an SDK or a device appears in `get_name()` starting with the brand, prefixed through `util::branded_name()` when it does not, and without the ` (...)` qualifiers an SDK adds for a connection mode (`util::without_qualifiers()`). Scope: `AlpacaCore/include/alpacacore/util/device_name.h`, `AlpacaCore/src/vendors/**`. Checked by: `AlpacaCore/tests/test_contract_sweep.cpp` ("Contract sweep tier 2 - every Name is brand and model while connected"), `AlpacaCore/tests/test_zwo_focuser.cpp` ("ZWO EAF Focuser Driver - the Name is ZWO and the type read at connect"), `AlpacaCore/tests/test_touptek_focuser.cpp` ("ToupTek AAF Focuser Driver - the connected Name is ToupTek and the SDK name"), `AlpacaCore/tests/test_gphoto_fake_sdk.cpp` ("GPhoto camera fake - the Name drops a libgphoto2 qualifier and the Description keeps it"), review (a `get_name()` that returns an SDK name field without the brand check).
3. A camera-attached dew heater or thermal switch is named the camera model followed by its function word once a connect has named the camera, and the brand followed by the function word before that. Scope: `AlpacaCore/src/vendors/zwo/zwo_switch_driver.cpp`, `AlpacaCore/src/vendors/touptek/touptek_thermal_switch_driver.cpp`, `AlpacaCore/src/vendors/playerone/playerone_switch_driver.cpp`. Checked by: `AlpacaCore/tests/test_zwo_switch.cpp` ("ZWO Dew Heater Switch Driver - the Name is the camera model and Dew Heater"), `AlpacaCore/tests/test_touptek_thermal_switch.cpp` ("ToupTek Thermal Switch Driver - the Name is the camera model and Thermal Switch"), `AlpacaCore/tests/test_playerone_switch.cpp` ("Player One Switch Driver - the Name is the camera model and Thermal Switch").
4. Details that are not the brand or model go in `get_description()`, which does no device I/O, and the router lists a non-empty Description as `Description` in each `configureddevices` entry, which the web UI shows. Scope: `AlpacaHTTP/src/http/router.cpp`, `AlpacaHTTP/web/app.js`, `AlpacaCore/src/vendors/**`. Checked by: `AlpacaHTTP/tests/test_routing.cpp` ("configureddevices lists each device's Description"), `AlpacaCore/tests/test_skywatcher_async.cpp` (the eps source in the Description), review (a `get_description()` that waits on the device).
5. The OnStep driver's brand is `OnStep` and its model is OnStepX's `:GVC#` reply, cut at the first comma and without its ` (...)` qualifiers; a reply that is empty or `0`, or a firmware older than 10 (not asked), leaves the name `OnStep Telescope`. Scope: `AlpacaCore/src/vendors/onstep/onstep_telescope_driver.cpp`. Checked by: `AlpacaCore/tests/test_onstep_telescope.cpp` ("OnStep Telescope Driver - the Name is OnStep and the OnStepX product description").

## Alternatives rejected

- **Keep the path in the name (`Sky-Watcher EQM-35 Pro (EQMOD)`).** It tells two entries for one mount apart in a chooser, but it names the driver, not the device, and every other client field (Description, UniqueID) already does that job.
- **A new web-UI field for the eps source only.** It fixes one driver; the Description carries any driver's extra state with one router field.
- **Prefix the brand in each driver by hand.** Some SDK strings carry the brand and some do not, so a hand-written prefix doubles it (`QHY QHY268M`) or misses it; one helper decides the same way everywhere.
- **Hard-code `ZWO EAFN` when the SDK name is `EAF`.** An EAF and an EAFN report the same `EAF_INFO.Name`; only `EAFGetType()` tells them apart.
- **Name OnStep after its controller board (`:GVH#`, `MaxPCB4`).** The board is the electronics, not the mount; the builder's product description is what the owner calls it.

## Consequences

- Client choosers show new names after the upgrade. A client that remembers a device by its name rather than its UniqueID may lose a saved selection once; the UniqueID is unchanged ([decision 0018](0018-persisted-unique-id.md)).
- The EQMOD and SynScan entries for one mount have the same name. The Description and the device number tell them apart in the web UI; a client that shows only names lists two identical entries.
- An OnStepX `PRODUCT_DESCRIPTION` loses its parenthesised parts and anything after a comma in the name: `EQ6 (belt mod), roof` is named `OnStep EQ6`.
- An SDK whose model string starts with the brand in another spelling (`Touptek`) is matched ignoring case, so it is not prefixed again.
- An unknown Sky-Watcher board or SynScan model id is named `Sky-Watcher Telescope`; the direct driver keeps the board code in its Description.

## How to reverse

Revert the drivers' `get_name()` bodies and `util::branded_name()` calls, remove the router's `Description` field and the device card row, and delete the two contract-sweep name cases. Moving the eps source back into the Sky-Watcher name restores #582's display.

## Links

- Issue [#2215](https://github.com/open-astro/AlpacaBridge/issues/2215); the eps source display: [#582](https://github.com/open-astro/AlpacaBridge/issues/582).
- [Persisted UniqueID](0018-persisted-unique-id.md) (what a client should key on instead of the name).
- Owner of the shared rule: [ASCOM contract](../../.github/instructions/ascom-contract.instructions.md#name-is-brand-and-model-all-drivers).
- OnStepX `:GVC#` / `:GVH#`: [command reference](https://github.com/hjd1964/OnStepX/blob/main/docs/COMMAND_REFERENCE.md).
