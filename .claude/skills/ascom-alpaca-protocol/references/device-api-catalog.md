# Device API Endpoint Catalog

## Scope and use

This inventory is derived from the pinned Alpaca Device API v1 schema. It lists every standard path suffix and permitted verb but does not replace operation-level OpenAPI schemas or Master Interface semantics. Before implementing a member, inspect its authoritative parameter types, ranges, return type, units, capability gates, and exceptions.

All full routes begin `/api/v1/`. Common routes substitute the canonical lowercase device type for `{device_type}` and the registered number for `{device_number}`.

## Common to all device types

| Endpoint | Verb | Purpose/check |
| --- | --- | --- |
| `action` | PUT | Named nonstandard extension; unsupported names use ActionNotImplemented |
| `commandblind` | PUT | Raw command with no returned device string; support is optional but route exists |
| `commandbool` | PUT | Raw command returning Boolean |
| `commandstring` | PUT | Raw command returning string |
| `connect` | PUT | Platform 7 asynchronous connection initiator |
| `connected` | GET, PUT | Connection state and legacy synchronous setter behavior |
| `connecting` | GET | Completion/in-progress property for Connect/Disconnect transitions |
| `description` | GET | Brief device description |
| `devicestate` | GET | Typed operational property bag; Platform 7 |
| `disconnect` | PUT | Platform 7 asynchronous disconnection initiator |
| `driverinfo` | GET | Driver information |
| `driverversion` | GET | Driver version string |
| `interfaceversion` | GET | Supported Master Interface version for this device type |
| `name` | GET | Device display name |
| `supportedactions` | GET | Exact names accepted by `action` |

Common requirements:

- The route exists even when optional command/action functionality is unsupported.
- `SupportedActions` and `Action` must agree.
- Connection transitions must follow the async rules; do not expose vendor startup delay.
- `DeviceState` item names and value types must match their individual properties. Omit unavailable items rather than inventing values.
- Metadata endpoints may remain available while disconnected when the Master Interface permits it.

## Camera

### Endpoint inventory

| Access | Endpoints |
| --- | --- |
| GET | `bayeroffsetx`, `bayeroffsety`, `camerastate`, `cameraxsize`, `cameraysize`, `canabortexposure`, `canasymmetricbin`, `canfastreadout`, `cangetcoolerpower`, `canpulseguide`, `cansetccdtemperature`, `canstopexposure`, `ccdtemperature`, `coolerpower`, `electronsperadu`, `exposuremax`, `exposuremin`, `exposureresolution`, `fullwellcapacity`, `gainmax`, `gainmin`, `gains`, `hasshutter`, `heatsinktemperature`, `imagearray`, `imagearrayvariant`, `imageready`, `ispulseguiding`, `lastexposureduration`, `lastexposurestarttime`, `maxadu`, `maxbinx`, `maxbiny`, `offsetmax`, `offsetmin`, `offsets`, `percentcompleted`, `pixelsizex`, `pixelsizey`, `readoutmodes`, `sensorname`, `sensortype` |
| GET, PUT | `binx`, `biny`, `cooleron`, `fastreadout`, `gain`, `numx`, `numy`, `offset`, `readoutmode`, `setccdtemperature`, `startx`, `starty`, `subexposureduration` |
| PUT | `abortexposure`, `pulseguide`, `startexposure`, `stopexposure` |

Total device-specific paths: **59**.

### Critical checks

- Advertise Platform 7 Camera interface version 4 when the complete V4 contract is implemented.
- Validate ROI as a coherent set: binning, start coordinates, dimensions, sensor bounds, and asymmetric-bin capability.
- Gain and offset may be numeric ranges or named modes depending on the camera; keep the associated members mutually consistent.
- `StartExposure` initiates acquisition. Publish camera/exposure state before returning and expose completion through `ImageReady` and related state according to the Master Interface.
- `ImageArray`/`ImageArrayVariant` fail until a complete valid image is available; do not return partial or previous frames as current.
- Runtime settings that hardware cannot safely change during exposure must return an error rather than corrupt the frame.
- `AbortExposure` and `StopExposure` are distinct capability-gated behaviors.
- Temperature and cooler properties must not fabricate values when the sensor or SDK cannot report them.
- For binary image transfer, read `imagebytes.md`.

## CoverCalibrator

### Endpoint inventory

| Access | Endpoints |
| --- | --- |
| GET | `brightness`, `calibratorchanging`, `calibratorstate`, `covermoving`, `coverstate`, `maxbrightness` |
| PUT | `calibratoroff`, `calibratoron`, `closecover`, `haltcover`, `opencover` |

Total: **11**.

### Critical checks

- Calibrator and cover are separate functions even when one physical product provides both.
- State enums must describe transition, ready, off/open/closed, unknown, and error states exactly as defined.
- `CalibratorOn` validates brightness against `MaxBrightness`.
- Opening/closing and calibrator changes follow their documented async semantics; do not report the terminal state before hardware confirmation.
- Unsupported cover or calibrator functions remain present and return the specified error.

## Dome

### Endpoint inventory

| Access | Endpoints |
| --- | --- |
| GET | `altitude`, `athome`, `atpark`, `azimuth`, `canfindhome`, `canpark`, `cansetaltitude`, `cansetazimuth`, `cansetpark`, `cansetshutter`, `canslave`, `cansyncazimuth`, `shutterstatus`, `slewing` |
| GET, PUT | `slaved` |
| PUT | `abortslew`, `closeshutter`, `findhome`, `openshutter`, `park`, `setpark`, `slewtoaltitude`, `slewtoazimuth`, `synctoazimuth` |

Total: **24**.

### Critical checks

- Dome rotation and shutter movement may be independent; publish the correct completion/state property for each.
- Immediately after an accepted open/close request, `ShutterStatus` must show the transition, not the old terminal state.
- `Slewing` is a completion property and must surface mid-operation failures.
- Slave behavior and manual slew commands must obey the Master Interface's conflict rules.
- Normalize azimuth only as specified; do not silently normalize invalid input when `InvalidValue` is required.

## FilterWheel

### Endpoint inventory

| Access | Endpoints |
| --- | --- |
| GET | `focusoffsets`, `names` |
| GET, PUT | `position` |

Total: **3**.

### Critical checks

- Advertise Platform 7 FilterWheel interface version 3 when implemented.
- `Names` and `FocusOffsets` arrays correspond position-for-position and match the wheel's slot count.
- Validate requested position before checking or commanding hardware when the contract requires `InvalidValue` precedence.
- During motion, report the standard moving value/state instead of a passing physical slot that could be mistaken for completion.
- Do not report the commanded target as settled until confirmed.

## Focuser

### Endpoint inventory

| Access | Endpoints |
| --- | --- |
| GET | `absolute`, `ismoving`, `maxincrement`, `maxstep`, `position`, `stepsize`, `tempcompavailable`, `temperature` |
| GET, PUT | `tempcomp` |
| PUT | `halt`, `move` |

Total: **11**.

### Critical checks

- Advertise Platform 7 Focuser interface version 4 when implemented.
- Interpret `Move` according to `Absolute`; validate both `MaxIncrement` and `MaxStep` as applicable.
- `IsMoving` is the completion property for moves and must reveal later failure through an error.
- Temperature compensation state affects whether manual movement is permitted; follow the Master Interface.
- `Halt` must stop or cancel safely and leave position/state truthful.

## ObservingConditions

### Endpoint inventory

| Access | Endpoints |
| --- | --- |
| GET | `cloudcover`, `dewpoint`, `humidity`, `pressure`, `rainrate`, `skybrightness`, `skyquality`, `skytemperature`, `starfwhm`, `temperature`, `winddirection`, `windgust`, `windspeed` |
| GET, PUT | `averageperiod` |
| PUT | `refresh` |
| Parameterized GET | `sensordescription`, `timesincelastupdate` |

Total: **17**.

### Critical checks

- Advertise Platform 7 ObservingConditions interface version 2 when implemented.
- Keep every measurement in its specified unit and valid range.
- Unsupported sensors return not implemented; do not return a plausible zero.
- `TimeSinceLastUpdate` refers to the named sensor and must reflect actual sample freshness.
- `AveragePeriod` defines averaging behavior consistently across supported sensors.
- `Refresh` must not falsely imply new sensor data if acquisition failed.

## Rotator

### Endpoint inventory

| Access | Endpoints |
| --- | --- |
| GET | `canreverse`, `ismoving`, `mechanicalposition`, `position`, `stepsize`, `targetposition` |
| GET, PUT | `reverse` |
| PUT | `halt`, `move`, `moveabsolute`, `movemechanical`, `sync` |

Total: **12**.

### Critical checks

- Advertise Platform 7 Rotator interface version 4 when implemented.
- Keep logical `Position`, `MechanicalPosition`, `TargetPosition`, `Reverse`, and `Sync` transformations consistent.
- Normalize angles only as specified.
- `IsMoving` is the completion property; a later motor failure must not look like successful arrival.
- `Move`, `MoveAbsolute`, and `MoveMechanical` have distinct coordinate semantics.

## SafetyMonitor

### Endpoint inventory

| Access | Endpoints |
| --- | --- |
| GET | `issafe` |

Total: **1**.

### Critical checks

- Return `true` only when the monitor can affirm safety under its defined policy.
- Communication loss or compromised sensor data is an error, not automatically safe.
- Do not silently convert unknown into unsafe unless the implementation explicitly documents that policy outside the standard value semantics.

## Switch

### Endpoint inventory

| Access | Endpoints |
| --- | --- |
| GET | `maxswitch`, `canasync`, `canwrite`, `getswitch`, `getswitchdescription`, `getswitchname`, `getswitchvalue`, `minswitchvalue`, `maxswitchvalue`, `statechangecomplete`, `switchstep` |
| PUT | `setasync`, `setasyncvalue`, `setswitch`, `setswitchname`, `setswitchvalue` |

Total: **16**.

### Critical checks

- Advertise Platform 7 Switch interface version 3 when implemented.
- Validate switch IDs against `MaxSwitch` before hardware access.
- Keep Boolean and numeric views consistent with min, max, and step.
- `CanWrite` and `CanAsync` are per-channel capability promises.
- Async setters return after acceptance and use `StateChangeComplete` for completion; synchronous setters complete before returning.
- Do not clamp an invalid value unless the Master Interface explicitly requires it.

## Telescope

### Endpoint inventory

| Access | Endpoints |
| --- | --- |
| GET | `alignmentmode`, `altitude`, `aperturearea`, `aperturediameter`, `athome`, `atpark`, `azimuth`, `canfindhome`, `canpark`, `canpulseguide`, `cansetdeclinationrate`, `cansetguiderates`, `cansetpark`, `cansetpierside`, `cansetrightascensionrate`, `cansettracking`, `canslew`, `canslewaltaz`, `canslewaltazasync`, `canslewasync`, `cansync`, `cansyncaltaz`, `canunpark`, `declination`, `equatorialsystem`, `focallength`, `ispulseguiding`, `rightascension`, `siderealtime`, `slewing`, `trackingrates` |
| GET, PUT | `declinationrate`, `doesrefraction`, `guideratedeclination`, `guideraterightascension`, `rightascensionrate`, `sideofpier`, `siteelevation`, `sitelatitude`, `sitelongitude`, `slewsettletime`, `targetdeclination`, `targetrightascension`, `tracking`, `trackingrate`, `utcdate` |
| Parameterized GET | `axisrates`, `canmoveaxis`, `destinationsideofpier` |
| PUT | `abortslew`, `findhome`, `moveaxis`, `park`, `pulseguide`, `setpark`, `slewtoaltaz`, `slewtoaltazasync`, `slewtocoordinates`, `slewtocoordinatesasync`, `slewtotarget`, `slewtotargetasync`, `synctoaltaz`, `synctocoordinates`, `synctotarget`, `unpark` |

Total: **65**.

### Critical checks

- Advertise Platform 7 Telescope interface version 4 when implemented.
- Right ascension is hours; declination, altitude, azimuth, pier geometry, and most angular values use the Master Interface's degree conventions.
- Validate site, target, axis, rate, and coordinate ranges exactly; do not silently wrap values that require `InvalidValue`.
- Keep synchronous and asynchronous slew members distinct. Async initiators publish `Slewing=true` before returning; completion failure surfaces through `Slewing` and compromised position properties.
- Capability members must agree with each slew, sync, park, home, pulse-guide, move-axis, tracking, and pier-side operation.
- Protect against collisions and impossible destinations before motion where knowable.
- Do not substitute raw mount pier-side values when the ASCOM convention requires a derived pointing state.
- Rates are offsets or absolute values according to each member's definition; do not reuse vendor units blindly.
- `UTCDate`, site data, and sync behavior must not silently corrupt the mount's pointing model.
- ConformU telescope testing can command aggressive motion; follow repository safety rules and use a bare mount where required.

## Counting check

The pinned schema contains **15 common paths** plus these device-specific paths:

| Device type | Paths |
| --- | ---: |
| Camera | 59 |
| CoverCalibrator | 11 |
| Dome | 24 |
| FilterWheel | 3 |
| Focuser | 11 |
| ObservingConditions | 17 |
| Rotator | 12 |
| SafetyMonitor | 1 |
| Switch | 16 |
| Telescope | 65 |

If a later schema changes these counts, update the version record and catalog together.

