---
applyTo: "AlpacaCore/src/vendors/celestron/**,AlpacaCore/include/alpacacore/vendor/celestron/**,AlpacaCore/tests/*celestron*,AlpacaCore/conformu/Celestron/**"
---

### Celestron (NexStar)

Devices: Telescope.

Protocol documentation: `AlpacaCore/external/Celestron/nexstar_protocol_reference.md` (combined reference — RS-232 serial, AUX bus, GPS, and AlpacaBridge driver notes). Original sources preserved in same directory. No external SDK required — uses RS-232 serial communication directly.

Connection types: Serial (RS-232 on hand control base) and Network (WiFi bridge adapters, default TCP port 2000).

- Communication: 9600 baud, no parity, one stop bit. All responses terminated with `#`.
- Position encoding: hexadecimal fraction of a revolution. Standard commands use 16-bit (4 hex digits, ~19.8 arcsec precision), precise commands use 24-bit (6 hex digits + "00" padding, ~0.08 arcsec precision). Commands: `E`/`e` for RA/Dec, `Z`/`z` for Alt/Az.
- GOTO: `R`/`r` for RA/Dec, `B`/`b` for Alt/Az. Sync: `S`/`s` (firmware v4.10+).
- Tracking modes: 0=Off, 1=Alt/Az, 2=EQ North, 3=EQ South.
- **Tracking/alignment mode selection**: report alignment from the connected HC model ID (known GEM IDs 5, 6, 13, 14, 20, 23, 24; known Alt-Az IDs 1, 3, 4, 7, 9–12, 15–19, 21, 22, 25). For EQ tracking, choose north/south from the cached SiteLatitude; Alt-Az mounts use mode 1. Unknown model IDs must not be silently called GermanPolar. The HC does not report wedge state for fork mounts, so Alt-Az is their nominal/default alignment classification. CGE/Advanced GT HC firmware 3.01–3.04 uses EQ North=1 and EQ South=2; the standard 2/3 values apply to later firmware.
- Model IDs from `"m"` command: 1=GPS, 3=i-Series, 4=i-Series SE, 5=CGE, 6=Advanced GT, 7=SLT, 9=CPC, 10=GT, 11=4/5 SE, 12=6/8 SE, 14=CGEM, 20=Advanced VX, 22=Evolution.
- Slew commands use pass-through (`P`) to motor controllers: device 16 = AZM/RA motor, device 17 = ALT/DEC motor. Fixed rates 1–9 (0 to stop), variable rates encoded as arcsec/sec × 4 in high/low bytes.
- Timeouts: NexStar spec says up to 3.5 seconds worst case for pass-through commands. Driver uses 5-second default.
- Time/Location: binary format (not ASCII). Timezone stored as hour offset (256-zone for negative). Location sign: 0=North/East, 1=South/West.
- **Home**: Mounts with hardware home switches (CGX, CGX-L, CGE Pro) use `MC_LEVEL_START` (0x0B) on both axes followed by polling `MC_LEVEL_DONE` (0x12). FindHome is asynchronous — send commands, return immediately, poll via Slewing/AtHome. Note: `MC_SEEK_INDEX` (0x19) / `MC_AT_INDEX` (0x18) are for PEC worm gear index (RA only), NOT home. See protocol reference for details.
- **Side of pier**: GEM mounts report pier side via the HC `p` command (`W` → pierWest, `E` → pierEast). This is a direct query — no hour angle inference needed.
- **Pulse guiding**: Uses native MC_AUX_GUIDE (0x26) hardware command via AUX bus pass-through. The firmware times the pulse internally — no sleep, encoder snapshotting, or sync calls required. Cross-axis is frozen at pre-pulse value during the guide window; active axis returns computed `baseline + (rate × duration)` as a one-shot correction to avoid ConformU tolerance failures at high declinations where cos(DEC) amplification causes geometric noise.
- Motion initiators are serialized across slews, park, FindHome, PulseGuide, MoveAxis, AbortSlew, Unpark and disconnect. `FindHome` refuses while parking; `Unpark` participates in the same handoff lock. Abort cleanup reaps its slew worker even when a hardware stop throws.
- **Adaptive RA slew offset**: Driver learns a running average of RA undershoot across slews and pre-biases subsequent slews to compensate for the CGX-L's no-tracking-during-goto behavior (matches INDI's `SlewOffsetRa` pattern).
- **Post-slew tracking restoration**: Re-issues the top-level `T` set-tracking-mode command rather than a per-axis variable-rate passthrough, keeping the HC's internal tracking state coherent with the LCD readout.
- **Site/time write skip when aligned**: `SiteLatitude`, `SiteLongitude`, and `UTCDate` writes are silently skipped (log warn, return success) when the mount is aligned, matching INDI's UpdateLocation/UpdateTime pattern. Writing these after alignment corrupts the HC's pointing model. Preserves ConformU property round-trip tests.
- **Pier-safety gate**: Accepts either a successful `SyncToCoordinates` in the current driver session OR HC-reported alignment (`J` command). HC workflow: power on → Switch Position → Location → Last Alignment → "CGX-L Ready".
- ConformU 4.3.0 validated for **Celestron CGX-L** on Linux arm64 with 0 errors and 0 issues.
- The NexStar serial protocol is nearly identical to SynScan — both derive from the same Celestron protocol family. The driver implementation follows the same pattern but with separate namespace and branding.
