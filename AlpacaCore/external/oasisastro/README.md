# Astroasis Oasis Focuser -- reverse-engineered USB HID protocol

No vendor SDK is used by `AlpacaCore/src/vendors/astroasis/`. The device
speaks a proprietary USB HID protocol, not the serial MyFocuserPro2 protocol
the "Gemini"-branded focusers in this repo use. This document is the
protocol reference recovered from the vendor's ASCOM driver installer
(`Astroasis_Oasis_Focuser_ASCOM_2.0.2.1_Setup.exe`, an Inno Setup package)
and is kept for future maintenance; the installer itself is a vendored,
unmodified copy of the vendor's public download, not a modified/redistributed
SDK (no SDK binary is linked into AlpacaBridge).

## How this was recovered

1. `innoextract` on the installer yields `ASCOM.OasisFocuser.Focuser.dll`
   (a .NET/Mono ASCOM wrapper) and `OasisFocuser.dll` / `OasisFocuser64.dll`
   (the native SDK, Windows x86/x64 PE, statically linking `hidapi`).
2. `ilspycmd` decompiles the .NET wrapper to near-source C#, giving the full
   `AOFocuser*` P/Invoke signatures, structs (`AOFocuserStatus`,
   `AOFocuserConfig`), and how ASCOM `IFocuserV3` properties map to SDK calls.
3. `pefile` + `capstone` (Python) disassemble the native x86-64 DLL's exported
   `AOFocuser*` functions and their internal HID transaction helper to
   recover the wire format below. No GUI disassembler (e.g. Ghidra) was used
   -- this device's Pi host has 2GB RAM, too little for comfortable headless
   analysis, so raw capstone listings were read by hand instead.

## USB identification

- VID:PID = `338F:A0F0` (from `AOFocuserScan` -> `hid_enumerate(0x338F, 0xA0F0)`)
- Standard USB HID device; no vendor driver needed on Linux, binds to the
  kernel's generic `hidraw`/`usbhid` drivers. AlpacaCore talks to it via
  `hidapi`'s hidraw backend (`libhidapi-dev` / pkg-config `hidapi-hidraw`).

## Wire format

65-byte HID reports (report ID 0):

```
Request:  [reportId=0][cmd][len][payload...]         (padded to 65 bytes)
Response: [cmd echo][responseLen][responsePayload...] (padded to 65 bytes)
```

Multi-byte integer payloads are big-endian (`htonl`/`ntohl`), **except** the
connect handshake's `0x11` command, whose 4-byte nonce is sent in host byte
order -- confirmed by disassembly, not a transcription slip. A response
whose echoed `cmd` or `responseLen` doesn't match what the caller expected is
a hard error in the vendor SDK (`AO_ERROR_INVALID_PARAMETER`); it does not
just log and continue.

**Verified against real hardware (Astroasis Oasis Focuser, 2026-07-22):** the
frame format, `0x11`/`0x36`/`0x37`/`0x32` commands, and the `0x30` GetConfig
`maxStep` field (see table below) all work exactly as documented here. Two
mistakes from the first disassembly pass were caught and fixed by testing
against the device directly (opcode/length bytes are easy to transpose by
hand when reading raw capstone output):
- `0x10` (second handshake step) takes **no payload** and returns a **4-byte**
  response (not a 4-byte zero payload expecting a 1-byte ack, as first
  written down). On real hardware this response echoed back the `0x11`
  nonce byte-for-byte -- likely a session-token confirmation, though it could
  also be the cached protocol/firmware version the vendor SDK uses to pick
  the GetStatus/GetConfig response layout; not confirmed which.
- GetConfig's (`0x30`) response payload leads with `AOFocuserConfig.mask`
  (4 bytes, observed as `0xFFFFFFFF` = the SDK's own `MASK_ALL` constant),
  **not** `maxStep` -- `maxStep` is the second field, at payload offset 4.

## Command table

| Command | Direction / payload | Notes |
|---|---|---|
| `0x01` | no request payload -> 32-byte response | Sent during `Scan()` only, to identify/match a candidate device. Not needed for a direct-path connect. |
| `0x11` | 4-byte **host-byte-order** payload (a tick-count-like nonce) -> 1-byte ack | First connect-handshake step. Gets a 1000ms timeout (the only command that does -- every other command uses 100ms). Verified working on real hardware. |
| `0x10` | no request payload -> 4-byte response | Second connect-handshake step. On real hardware, the response echoed back the `0x11` nonce exactly. |
| `0x32` | no request payload -> 14-byte (older firmware) or 60-byte (newer firmware) response | GetStatus. See field layout below (verified on real hardware for the 14-byte layout: position/moving/temperatureInt tracked correctly through a live move). Firmware selects the format; the vendor SDK caches which one from a protocol-version field populated during connect (not fully reverse-engineered -- this driver instead tries the 14-byte layout first and falls back to 60 bytes on a length mismatch). |
| `0x30` / `0x3a` | no request payload -> 18-byte / 40-byte response | GetConfig, same older/newer-firmware split as GetStatus. Response payload is `[mask:4][maxStep:4][...]` -- `maxStep` at offset 4 verified on real hardware (returned 80000, a plausible focuser range). Full layout per SDK version in "GetConfig layout" below (issue #2082). |
| `0x36` | 4-byte BE target position -> 1-byte ack | MoveTo (absolute). Verified working on real hardware -- a +50/-50 step round trip landed back on the exact starting position. |
| `0x37` | no payload -> 1-byte ack | StopMove / Halt. Sent successfully on real hardware (not tested mid-move, since the moves above completed before the poll caught them in progress). |
| *(none)* | -- | Close: the vendor SDK sends no protocol command, it just tears down the OS HID handle. |

GetStatus (0x32) 14-byte response field layout (older firmware; matches the
`AOFocuserStatus` struct's declared field order exactly):

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 (BE) | raw internal-sensor ADC code | -> `temperatureInt` via the thermistor curve below |
| 4 | 4 (BE) | raw external-sensor reading, or `0x80000000` sentinel | -> `temperatureExt`; sentinel means "no external probe" (matches .NET's `== int.MinValue` check) |
| 8 | 1 | `temperatureDetection` | nonzero = external probe present |
| 9 | 1 | `moving` | nonzero = focuser in motion |
| 10 | 4 (BE) | `position` | |

The newer/60-byte firmware format additionally reports `stallDetection`,
`heatingOn`, `heatingPower`, and `dcPower` (exact trailing-field byte offsets
not fully mapped -- low priority, none of these are ASCOM `IFocuserV3`
properties).

### GetConfig layout (issue #2082)

Read from the 2.1.0.1 installer (`Astroasis_Oasis_Focuser_ASCOM_2.1.0.1_Setup.zip` from
astroasis.com/download, sha256 `68c2703fb40147c7a986e2f8ebbd7a96397aef525e7f5eb8cdb54045b3bc35a4`, fetched
2026-11-17): the `AOFocuserGetConfig` path of `OasisFocuser64.dll` (pefile + capstone, export at RVA `0x1eb0`, body
at `0x2e10`) and the `AOFocuserConfig` field order from the .NET wrapper's metadata tables (`dnfile`). The SDK picks
the request from the protocol version it cached at connect (a 32-bit word at device-struct offset `0x124`), not by
trying one and falling back:

| Protocol version word | Request | Response payload length |
|---|---|---|
| `<= 0x10000300` | `0x30` | 18 bytes |
| `> 0x10000300` | `0x3a` | 40 bytes |

Both lengths are the ones 2.0.2.1 used; 2.1.0.1 adds no third request and no longer response. Our driver's checks
(`0x30` expecting 18, then `0x3a` expecting 40 on an `InvalidValue` mismatch) accept every response the 2.1.0.1 SDK
can receive, so no shipping firmware is rejected by length. That holds for the reply lengths only: on newer
firmware the driver still sends `0x30` first, which the SDK never does (see the last open question below).

Payload offsets (big-endian multi-byte fields; the SDK converts with `ntohl`/`ntohs`):

| Offset | Size | `AOFocuserConfig` field | 18-byte (`0x30`) | 40-byte (`0x3a`) |
|---|---|---|---|---|
| 0 | 4 | `mask` | yes | yes |
| 4 | 4 | `maxStep` (the only field the driver reads) | yes | yes |
| 8 | 4 | `backlash` | yes | yes |
| 12 | 1 | `backlashDirection` | yes | yes |
| 13 | 1 | `reverseDirection` | yes | yes |
| 14 | 1 | `speed` | yes | yes |
| 15 | 1 | `beepOnMove` | yes | yes |
| 16 | 1 | `beepOnStartup` | yes | yes |
| 17 | 1 | `bluetoothOn` | yes | yes |
| 18 | 1 | `stallDetection` | -- (SDK sets 0) | yes |
| 19 | 4 | `heatingTemperature` | -- (SDK sets 0) | yes |
| 23 | 1 | `heatingOn` | -- (SDK sets 0) | yes |
| 24 | 1 | `usbPowerCapacity` | -- (SDK sets 0) | yes |
| 25 | 2 | `rhcStep1` | -- | only when the version word is `>= 0x10020000` |
| 27 | 2 | `rhcStep2` | -- | as above |
| 29 | 2 | `rhcStep3` | -- | as above |
| 31 | 1 | `rhcStepFactor` | -- | as above; below `0x10020000` the SDK sets the three steps to 0 and the factor to 4 |
| 32 | 8 | (not read by the SDK) | -- | -- |

So the remote-hand-controller fields that 2.1.0.1 adds (`RHC_STEP1..3`, 0-65535, and `RHC_STEP_FACTOR`) sit inside
the existing 40-byte payload, in bytes the 2.0.x SDK did not read; they did not lengthen it. Which firmware release
reports which version word was not read here: the word is cached during connect, and the mapping of firmware 1.4.0
(the current download) to `>= 0x10020000` is an inference from the gate, not checked on a device.

### Temperature conversion

Internal board sensor (raw 12-bit ADC code, 0-4095, NTC thermistor B=3380K
calibrated at 25 degC / 298.15K):

```
clamp raw to [1, 4094]
ratio    = (4095 - raw) / raw
T_kelvin = 3380.0 / (ln(ratio) + 11.336575508117676)   // 11.336575... = 3380/298.15
T_celsius = T_kelvin - 273.15
```

External probe (DS18B20-style digital sensor, 1/16 degC per raw count):

```
T_celsius = raw * 0.0625
```

Both formulas, including the exact float/double constants, were read
directly out of the DLL's data section -- they have not yet been checked
against a real temperature reading.

## Open questions / TODO

- Exact byte offsets for `stallDetection`/`heatingOn`/`heatingPower`/`dcPower`
  in the 60-byte GetStatus response.
- `AOFocuserSetConfig`, `AOFocuserGetVersion`, `AOFocuserGetSerialNumber`,
  `AOFocuserFactoryReset`, `AOFocuserClearStall`, Bluetooth name get/set, and
  firmware upgrade were not reverse-engineered (none are required by ASCOM
  `IFocuserV3`).
- The `0x11` handshake nonce doesn't appear to be validated -- an arbitrary
  test value (`0xAABBCCDD`) was accepted and echoed back by `0x10` on real
  hardware.
- The temperature conversion formulas (thermistor curve for the internal
  sensor, affine scale for the external probe) have not been checked against
  a reference thermometer -- only that they produce a plausible-looking
  number (~41 degC for the internal board sensor).
- GetStatus's newer/60-byte firmware branch and GetConfig's newer/`0x3a`
  branch are unverified on a device -- the test unit's firmware used the
  older/shorter layout for both. The `0x3a` layout above is read from the
  2.1.0.1 SDK only.
- How a firmware whose version word is above `0x10000300` answers a `0x30`
  GetConfig (the vendor SDK never sends it one): the driver relies on a
  mismatched reply (an `InvalidValue`) to fall back to `0x3a`; a firmware
  that stays silent instead would fail the connect after the timeout, and one
  that answers with a different command echo (a NAK or error byte) makes
  `send_command` throw `DriverException`, so the driver never falls back
  either. Needs a device on newer firmware.

Resolved by hardware testing (2026-07-22): hidapi's Linux hidraw backend
needs no manual report-ID offset on reads for this device (`in[0]` is the
echoed cmd byte directly, as assumed).
