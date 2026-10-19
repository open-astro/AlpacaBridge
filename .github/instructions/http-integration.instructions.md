---
applyTo: "AlpacaHTTP/**,AlpacaCore/src/vendors/*/*_fields.h,AlpacaCore/src/vendors/*/*_catalog.cpp,AlpacaCore/src/vendors/*/*_schema.cpp,AlpacaCore/src/catalog/**"
---

## AlpacaHTTP integration, web UI and logging

Shared rules for every vendor, moved verbatim from `AGENTS.md`, which keeps the
core rules and routes here. A rule that applies to a second vendor belongs in
the matching shared file, not in a vendor file.

## AlpacaHTTP Integration Checklist (Required for New Vendor/Device Types)

When adding a new vendor/device type in AlpacaCore, also update AlpacaHTTP:

1. **Catalog descriptor** — declare the vendor/device type in `AlpacaCore/src/vendors/<vendor>/<vendor>_fields.h` (fields), `<vendor>_schema.cpp` (schema, compiled in every build, no vendor header) and `<vendor>_catalog.cpp` (factory, compiled only under `ALPACACORE_ENABLE_<VENDOR>`), and register them in `AlpacaCore/src/catalog/builtin_catalog.cpp`. `Router::register_device_from_config` has no vendor arm any more (the last one, iOptron, moved in #1228): it consults the catalog and answers "not yet supported" for anything else. **Fields are read through `DeviceConfig::get()` / `find()`, never raw JSON** (#388): `contains()` is true for an explicit JSON `null` and `value()` THROWS `type_error` rather than returning the default when the stored value is not convertible, so `{"siteLatitude": null}` — trivial to produce from a client that serialises an unset value instead of omitting the key — came back as an nlohmann type complaint instead of the specific message the field has. The catalog reader treats null as absence and reports a genuinely wrong type as an `AlpacaException` naming the field; a nested `ports[]` list is a `RecordList` read the same way.
2. **No vendor include in `router.cpp`** — the router includes no `<alpacacore/vendor/...>` header (`scripts/check_layering.py` baseline 0); the factory file is the only place the driver header is included.
3. **Config sanitization fields** — a declared, non-secret field is preserved by the catalog's sanitize; an undeclared key is dropped, so declare every persisted field.
4. **Web UI vendor dropdown** — add the vendor to the device-type dropdown in the web frontend (`AlpacaHTTP/web/app.js`) so users can select it.
5. **Web UI vendor-specific form fields** — add any vendor-specific configuration fields (e.g. serial port, camera index, connection type) to the frontend form.
6. **Web UI index fields** — if the vendor connects by an SDK enumeration index, give each index input a **unique vendor-prefixed `name`** and register it in the `INDEX_FIELDS` array so auto-numbering and manual-edit tracking work. Both are mandatory and easy to miss — see "Enumeration index fields" below for the full rationale (a generic `name` silently saves 0; a missing registry entry reuses index 0 on the next device).
7. **Frontend validation** — add any related validation logic in frontend JS.
8. **Build-flag propagation** — ensure `ALPACACORE_ENABLE_<VENDOR>` compile definitions propagate from AlpacaCore to AlpacaHTTP.
9. **Routing/config tests** — add or update tests in `AlpacaHTTP/tests/`.

Vendor registration alone is not enough for HTTP/UI visibility. All nine steps must be completed for a new vendor/device to be fully functional end-to-end.

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
- When editing an existing device, populate the textarea from `config.filterNames` and
  call the instance's `syncSlotsFromTextarea()` so the dropdowns reflect the saved names.

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
  descriptive (`playerone-camera-index`) — `setFormValue`/auto-fill key off `id`, the
  collision is purely about the `name` used in `FormData`. **This applies to EVERY shared
  field name, not just the `*Index` ones** — the `*Id` binding fields
  (`cameraId`/`focuserId`/`filterwheelId`/`rotatorId`) and discriminator selects
  (`switchType`) collide the same way. ToupTek's focuser-id (`touptekFocuserId`), filter-wheel-id
  (`touptekFilterwheelId`), and switch-type (`touptekSwitchType`) are all prefixed for this
  reason; a bare `name="focuserId"` on a non-ZWO input silently returns ZWO's value. When you add
  ANY input whose name matches a field ZWO already uses, prefix it and read the prefixed name in
  the submit handler.
- **Register the field in the `INDEX_FIELDS` array** in `AlpacaHTTP/web/app.js`
  (`fieldId`, `vendor`, `deviceType`, `configKey`, optional `idFieldId`). That one entry
  drives auto-increment (so a second device of the same vendor/type doesn't reuse index 0)
  **and** the manual-edit tracking. The index is scoped per `(vendor, deviceType)` — each
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
- Alpaca-style management responses (including the new logfile endpoints) return HTTP 200 even when `ErrorNumber != 0` — clients must inspect the body, not the HTTP status. One exception: `configuredevice` refusals of a GPIO line, GPIO chip or device path outside the board's allowlist, and a duplicate GPIO line, answer HTTP 400 (issue #765), and so does `POST /management/v1/update/channel` with a body that is not `{"Channel":"stable"|"beta"}` (issue #1103).
