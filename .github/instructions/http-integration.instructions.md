---
applyTo: "AlpacaHTTP/**,AlpacaCore/src/vendors/*/*_fields.h,AlpacaCore/src/vendors/*/*_catalog.cpp,AlpacaCore/src/vendors/*/*_schema.cpp,AlpacaCore/src/catalog/**"
---

## AlpacaHTTP integration, web UI and logging

Shared rules for every vendor, moved verbatim from `AGENTS.md`, which keeps the
core rules and routes here. A rule that applies to a second vendor belongs in
the matching shared file, not in a vendor file.

## AlpacaHTTP Integration Checklist (Required for New Vendor/Device Types)

With the device catalog (decision 0004), adding a vendor or device type is one descriptor and its tests:

1. **Write one descriptor** — `AlpacaCore/src/vendors/<vendor>/<vendor>_fields.h` (fields), `<vendor>_schema.cpp` (schema, compiled in every build, no vendor header) and `<vendor>_catalog.cpp` (factory, compiled only under `ALPACACORE_ENABLE_<VENDOR>`), registered in `AlpacaCore/src/catalog/builtin_catalog.cpp`. `Router::register_device_from_config` has no vendor arm: it consults the catalog. The catalog sanitizes saved config from the declared fields, so declare every persisted non-secret field or it is dropped.
2. **Read fields through `DeviceConfig::get()` / `find()`, never raw JSON** (#388): a JSON `null` counts as absence and a wrong type becomes an `AlpacaException` naming the field.
3. **No vendor include in `router.cpp`** — the router includes no `<alpacacore/vendor/...>` header (`scripts/check_layering.py` baseline 0); the factory file is the only place the driver header is included.
4. **Web UI, what the catalog does not cover** — add the vendor's `<option>` to `#vendor` in `AlpacaHTTP/web/index.html` (the device types that offer it come from the descriptor's `available` flag), and put each vendor-specific form field inside a `data-vendor` / `data-device-type` scope `<div>` with `data-config-key="<catalog field key>"`. `AlpacaHTTP/tests/web/catalog_form_drift.test.js` fails on a form field the catalog does not declare, on a catalog field no form field names, and on a control that does not fit the field's type or role. Editing a device fills every mapped field from the saved config with no `app.js` code (`editAssignments()` in `catalog_schema.js`); a field that needs a conversion carries `data-config-fill="manual"` and its fill goes in `startEditDevice` (`catalog_edit_fill.test.js`).
5. **Index inputs** — an SDK enumeration index input gets a **unique vendor-prefixed `name`** and `type="number"` with a `data-config-key` naming an `EnumerationIndex` field; see "Enumeration index fields" below.
6. **Tests** — add the descriptor's tests and any routing/config tests in `AlpacaHTTP/tests/`; `ALPACACORE_ENABLE_<VENDOR>` must reach AlpacaHTTP (see the packaging rules).

### FilterWheel vendors — required web UI (slot count + filter name pickers)

Every filterwheel vendor's config form MUST include the standard slot UI, not just a
names textarea. It consists of: a **slot-count select listing the manufacturer's actual
wheel lineup** plus a Custom option, the per-slot filter dropdowns (LRGB/narrowband/Sloan
presets + Custom), and the "Advanced: edit filter names as lines" textarea. The whole
widget is one reusable component — instantiate `createFilterwheelSlotUI({...})` in
`AlpacaHTTP/web/app.js` with vendor-prefixed element IDs and copy the markup pattern from
an existing vendor in `index.html`. The component keeps the slot rows and the textarea in
sync; the form submit reads the textarea.

**Single-token shorthand expansion** (shared by `parseFilterNamesInput` in `app.js` and the
`normalize_slot_data_locked` in the ZWO/ToupTek filter-wheel drivers): a lone name with no
delimiters whose length equals the slot count expands to one character per slot (`LRGB` →
`L,R,G,B`). Guard it with **"no lowercase letters"** so ordinary names like `Clear` or
`Ha_NB` that happen to match the slot count are NOT exploded — a bot-review foot-gun. Keep
the JS and C++ conditions in step (both layers run the same rule).

- **When building a new filterwheel driver, ask the user what slot counts the
  manufacturer offers** and put exactly those in the select (with model names in the
  labels where known). Known lineups: ZWO EFW 5/7/8; Player One Phoenix Wheel 5/7/8
  (PW5/PW7/PW8).
- Use unique, vendor-prefixed form field `name`s (e.g. `playerOneFilterwheelIndex`,
  `playerOneFilterNames`) — the
  [Enumeration index fields](#enumeration-index-fields--unique-names--auto-numbering-all-vendors)
  FormData-collision rule.
- When editing an existing device, the edit fill sets the textarea from `config.filterNames`
  through its `data-config-key`; `startEditDevice` then calls the instance's `syncSlotsFromTextarea()` so the dropdowns reflect the saved names.

### Enumeration index fields — unique names + auto-numbering (all vendors)

Many vendors connect by an SDK **enumeration index** (camera/focuser/filterwheel/rotator
index — "which unit on the bus", numbered from 0). Two rules keep these working; a new
vendor that ignores either ships a silently broken form:

- **Each index input MUST have a unique, vendor-prefixed `name`** — e.g.
  `playerOneCameraIndex`, `qhyCameraIndex`, `touptekFocuserIndex`, `geminiFocuserIndex` —
  and the submit handler MUST read that exact name. Hidden vendor sections are **not**
  disabled, so every index input is still in the form's `FormData`. A generic `name` like
  `cameraIndex`/`focuserIndex` collides: `formData.get('cameraIndex')` returns the **first**
  such field in DOM order (ZWO's, which appears first), so the value you typed into a
  later vendor's field is discarded and `0` is saved instead. This is exactly why a
  Player One/QHY/SVBONY/ToupTek camera index could not be changed from 0. ZWO keeps the
  bare `cameraIndex`/`focuserIndex`/`filterwheelIndex`/`rotatorIndex` names (it's the
  canonical first block); **every other vendor must prefix**. Element `id`s can stay
  descriptive (`playerone-camera-index`) — the edit fill and auto-fill key off `id`, the
  collision is purely about the `name` used in `FormData`. **This applies to EVERY shared
  field name, not just the `*Index` ones** — the `*Id` binding fields
  (`cameraId`/`focuserId`/`filterwheelId`/`rotatorId`) and discriminator selects
  (`switchType`) collide the same way. ToupTek's focuser-id (`touptekFocuserId`), filter-wheel-id
  (`touptekFilterwheelId`), and switch-type (`touptekSwitchType`) are all prefixed for this
  reason; a bare `name="focuserId"` on a non-ZWO input silently returns ZWO's value. When you add
  ANY input whose name matches a field ZWO already uses, prefix it and read the prefixed name in
  the submit handler.
- **Give the input `data-config-key`** naming its catalog field, inside a `data-vendor` / `data-device-type` scope `<div>`, so the form drift check finds it.
- **Make it a catalog index field.** A `type="number"` input whose `data-config-key` names an
  `EnumerationIndex` field of the descriptor is an index field; `app.js` derives the list with
  `deriveIndexFields()` (`catalog_schema.js`), so there is no table to edit. That derivation
  drives auto-increment (so a second device of the same vendor/type doesn't reuse index 0)
  **and** the manual-edit tracking. It pairs the input with the descriptor's `DeviceId` field,
  else its `PortPath` field, that has the same `appliesWhen`: a value typed there pins a
  specific unit, so no index is imposed. The index is scoped per `(vendor, deviceType)` — each
  SDK enumerates from 0 independently, so a ZWO camera and a Player One camera are both
  index 0. This is distinct from the Alpaca **device number** (auto-assigned per device
  type, vendor-agnostic, and what clients address). Serial/network devices (port path or
  host) have no index and belong in neither place.
- **Declare EVERY persisted field in the vendor's catalog schema, per device type (`sanitize_device_config` in `router.cpp` keeps exactly the declared fields)**
  (bit us three times: ZWO ASIAIR PWM ports, ToupTek `switchType`, ToupTek AFW filter fields).
  `sanitize_device_config` is a strict allowlist. It keeps the vendor-agnostic keys it names
  (`vendor`, `deviceType`, `deviceNumber`, the site and timing keys) and, for everything else,
  exactly the fields the device's catalog descriptor declares in
  `AlpacaCore/src/vendors/<vendor>/<vendor>_fields.h` (`sanitize_fields_json`; a `Secret` field
  is never written). An undeclared key is silently dropped on save, so the config round-trips
  lossily and the setting reverts (a filter-wheel binding resets to index 0, custom filter names
  vanish, PWM toggles revert). When the factory in `<vendor>_catalog.cpp` reads a new config key,
  you MUST declare that field in `<vendor>_fields.h` for every device type that reads it: the
  descriptor is per (vendor, device type), so a field declared only for the camera is dropped
  from the focuser's saved config. Cross-check the keys each factory reads against its
  descriptor's field list; they must match. There is no vendor branch in
  `sanitize_device_config` and no registration arm to edit any more (the last, iOptron, moved
  in #1228).

## Logging, Threading, and Errors

- Use AlpacaCore logging sink flow; do not use ad-hoc stdout/stderr logging in runtime paths.
- Avoid global mutable state; protect shared state with mutexes.
- Use `AlpacaException` for error paths; AlpacaHTTP maps exceptions to Alpaca error responses.
- **Disconnect paths must be exception-safe** (also in the concurrency checklist): in ref-counted SDK wrappers, erase the usage bookkeeping **before** the SDK close call so a throwing close (device unplugged) cannot leave a zero-count entry that turns later closes into no-ops; in driver `set_connected(false)`, clear driver state (`connected_`, handle/id, cached info) **before** the SDK close so a throw cannot trap the driver half-connected. The PlayerOne PW + ZWO EFW/EAF/CAA wrappers and drivers are the template.
- AlpacaHTTP must return Alpaca-style JSON envelopes and stable error mapping behavior.
- On-disk logging writes daily files `alpacabridge-YYYY-MM-DD.log` to `logging.directory` (default `/var/log/AlpacaBridge`, per-config override, env `ALPACAHTTP_LOG_DIRECTORY`). The sink falls back to `$XDG_STATE_HOME/AlpacaBridge/logs` (or `~/.local/state/AlpacaBridge/logs`) when the configured path is not writable. systemd unit uses `LogsDirectory=AlpacaBridge`; the deb postinst pre-creates the directory for non-systemd starts. There is no in-memory log buffer — `/management/v1/logs` reads today's daily file directly from disk.
- Retention: `logging.retention_days` (default 90, 0 = forever, env `ALPACAHTTP_LOG_RETENTION_DAYS`) auto-deletes daily files whose embedded date is older than `today − retention_days`. Pruning runs once on startup and again on day-rollover inside the file sink. Today's active file is never pruned.
- Web portal exposes `GET /management/v1/logfiles`, `GET /management/v1/logfiles/{name}[?download=1]`, and `DELETE /management/v1/logfiles/{name}`. Filenames are validated against the daily pattern to prevent path traversal. `util::read_log_file` enforces a 10 MiB per-request cap; web viewer warns and suggests download above 5 MiB.
- Log level set via `POST/PUT /management/v1/loglevel` is persisted to `config/runtime_state.json` and reapplied on the next start (overrides `default.yaml`'s `logging.level`). Delete that file to fall back to the YAML default. Persistence failures are logged at WARNING and never block the API response.
- Alpaca-style management responses (including the new logfile endpoints) return HTTP 200 even when `ErrorNumber != 0` — clients must inspect the body, not the HTTP status. The exceptions (the `configuredevice` hardware refusals, issue #765, and the update channel body check, issue #1103, both HTTP 400) and their reasons are listed under Deliberate departures in [the LAN surface threat model](../../docs/decisions/0007-lan-surface-threat-model.md); a new one goes there first.
