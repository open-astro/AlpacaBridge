# NexStar Protocol Reference

Combined reference for the Celestron NexStar serial protocol, AUX bus commands, and GPS interface. This document merges information from four sources into a single unified reference.

---

## Credits & Sources

- **Celestron** -- Official NexStar Communication Protocol (RS-232 serial commands for the hand controller).
- **Andre Paquette** (andre@paquettefamily.ca) -- NexStar AUX Command Set, Issue 1.0, February 2003. Acknowledgements: Ray St. Denis (protocol discovery, demo software), Mike Zeidler (editing, equipment access), Stacey Sheldon (PIC programming, firmware upgrade decoding), Carlene Paquette (editing, formatting).
- **Mike Swanson** (swanson.michael@usa.net, NexStarSite.com) -- New NexStar GPS Commands (variable rate tracking, tracking mode).
- **Open-source drivers** -- INDI Celestron driver (`drivers/telescope/celestrondriver.cpp`), Ryoko ASCOM Celestron driver, jochym/nexstar-evo, platini2/celestronauxbus.
- **AlpacaBridge project** -- Validated on Celestron CGX-L with NexStar+ HC fw 5.35, MC fw 7.18 (2026).

---

## Overview

NexStar telescopes expose two layers of control:

1. **HC Serial Commands** -- ASCII/binary commands sent at 9600 baud (no parity, 1 stop bit) to the RS-232 port on the hand controller (HC). These are high-level operations (GOTO, Get Position, Sync, etc.) interpreted by the HC.

2. **AUX Bus Commands** -- Binary packets exchanged between internal devices (motor controllers, GPS module, main board) at 19200 baud with hardware flow control (RTS/CTS). These can be accessed from the PC port directly, or relayed through the HC serial port using the pass-through (`P`) command format.

The HC acts as a gateway: it interprets its own command set and proxies AUX commands via the `P` pass-through mechanism. All position values are encoded as fractions of a full rotation in hexadecimal.

**Position encoding:**
- Standard (16-bit): precision = 360/65536 degrees ~ 19.8 arcseconds per unit.
- Precise (24-bit): precision = 360/16777216 degrees ~ 0.08 arcseconds per unit. Only the upper 24 bits of the 32-bit field are used.

**Applies to:** NexStar GPS, NexStar GPS-SA, NexStar iSeries, NexStar SE Series, NexStar GT, CPC, SLT, Advanced-GT, CGE, CGX, CGX-L, and Evolution mounts.

---

## HC Serial Commands (RS-232)

All commands are sent to the HC serial port at 9600 baud, no parity, 1 stop bit. Responses are terminated with `#` (0x23). Software drivers should be prepared to wait up to 3.5 seconds for a response.

### Position Commands

| Command | Char | Format | Response | Version |
|---------|------|--------|----------|---------|
| Get RA/DEC | `E` | `E` | `HHHH,HHHH#` | 1.2+ |
| Get precise RA/DEC | `e` | `e` | `HHHHHHHH,HHHHHHHH#` | 1.6+ |
| Get AZM-ALT | `Z` | `Z` | `HHHH,HHHH#` | 1.2+ |
| Get precise AZM-ALT | `z` | `z` | `HHHHHHHH,HHHHHHHH#` | 2.2+ |

Values are hexadecimal fractions of a full rotation. Example: `34AB,12CE` means DEC = 0x12CE/0xFFFF * 360 = 26.4 degrees.

If the telescope has not been aligned, RA/DEC values are meaningless. AZM-ALT values are relative to power-on position until alignment.

### GOTO Commands

| Command | Char | Format | Response | Version |
|---------|------|--------|----------|---------|
| GOTO RA/DEC | `R` | `RHHHH,HHHH` | `#` | 1.2+ |
| GOTO precise RA/DEC | `r` | `rHHHHHHHH,HHHHHHHH` | `#` | 1.6+ |
| GOTO AZM-ALT | `B` | `BHHHH,HHHH` | `#` | 1.2+ |
| GOTO precise AZM-ALT | `b` | `bHHHHHHHH,HHHHHHHH` | `#` | 2.2+ |

GOTO RA/DEC commands require the telescope to be aligned. The last byte of each 32-bit coordinate is always `00` since encoder resolution does not exceed 24 bits.

### Sync Commands

Center a known object in the eyepiece, then send the Sync command with its RA/DEC coordinates. Improves pointing accuracy for nearby objects.

| Command | Char | Format | Response | Version |
|---------|------|--------|----------|---------|
| Sync RA/DEC | `S` | `SHHHH,HHHH` | `#` | 4.10+ |
| Sync precise RA/DEC | `s` | `sHHHHHHHH,HHHHHHHH` | `#` | 4.10+ |

### Tracking Commands

| Command | Char | Format | Response | Version |
|---------|------|--------|----------|---------|
| Get Tracking Mode | `t` | `t` | `chr(mode)#` | 2.3+ |
| Set Tracking Mode | `T` | `T` + `chr(mode)` | `#` | 1.6+ |

Tracking modes:

| Value | Mode |
|-------|------|
| 0 | Off |
| 1 | Alt/Az |
| 2 | EQ North |
| 3 | EQ South |

**Note:** On CGE and Advanced GT HC versions 3.01-3.04, EQ North = 1 and EQ South = 2 (corrected in later versions).

The tracking rate reverts to the HC default (usually sidereal) after ~20 seconds of inactivity or after pressing arrow buttons. To prevent this, set tracking mode to Off before issuing variable rate commands.

### Slew Commands

#### Variable Rate Slew

Multiply the desired rate in arcseconds/second by 4, then split into high and low bytes:

```
trackRateHigh = (rate_arcsec * 4) / 256
trackRateLow  = (rate_arcsec * 4) % 256
```

Example: 150 arcsec/s -> rate = 600, high = 2, low = 88. Sidereal rate (~15.04 arcsec/s) -> rate = 60.

| Command | Pass-through bytes | Version |
|---------|--------------------|---------|
| Variable rate AZM/RA positive | `P chr(3) chr(16) chr(6) chr(rateHi) chr(rateLo) chr(0) chr(0)` | 1.6+ |
| Variable rate AZM/RA negative | `P chr(3) chr(16) chr(7) chr(rateHi) chr(rateLo) chr(0) chr(0)` | 1.6+ |
| Variable rate ALT/DEC positive | `P chr(3) chr(17) chr(6) chr(rateHi) chr(rateLo) chr(0) chr(0)` | 1.6+ |
| Variable rate ALT/DEC negative | `P chr(3) chr(17) chr(7) chr(rateHi) chr(rateLo) chr(0) chr(0)` | 1.6+ |

Response: `#`

#### Fixed Rate Slew

Rate values 0-9 correspond to HC button rates (0 = stop).

| Command | Pass-through bytes | Version |
|---------|--------------------|---------|
| Fixed rate AZM/RA positive | `P chr(2) chr(16) chr(36) chr(rate) chr(0) chr(0) chr(0)` | 1.6+ |
| Fixed rate AZM/RA negative | `P chr(2) chr(16) chr(37) chr(rate) chr(0) chr(0) chr(0)` | 1.6+ |
| Fixed rate ALT/DEC positive | `P chr(2) chr(17) chr(36) chr(rate) chr(0) chr(0) chr(0)` | 1.6+ |
| Fixed rate ALT/DEC negative | `P chr(2) chr(17) chr(37) chr(rate) chr(0) chr(0) chr(0)` | 1.6+ |

Response: `#`

**Notes:**
- Fixed rate slews at rate 1 or 2 during EQ tracking will not override tracking. Useful for guiding simulation.
- On GT models, fixed rate 9 moves at 3 deg/s instead of maximum rate.
- Issuing slew commands generally overrides tracking mode. Disable tracking first, slew, then re-enable.

### Time/Location Commands

All values are binary, not ASCII.

#### Location Format

8 bytes `ABCDEFGH`:

| Byte | Field |
|------|-------|
| A | Latitude degrees |
| B | Latitude minutes |
| C | Latitude seconds |
| D | Latitude sign (0 = North, 1 = South) |
| E | Longitude degrees |
| F | Longitude minutes |
| G | Longitude seconds |
| H | Longitude sign (0 = East, 1 = West) |

#### Time Format

8 bytes `QRSTUVWX`:

| Byte | Field |
|------|-------|
| Q | Hour (24-hour clock) |
| R | Minutes |
| S | Seconds |
| T | Month |
| U | Day |
| V | Year (century assumed 20) |
| W | GMT offset (if negative: 256 - abs(offset)) |
| X | DST (1 = Daylight Saving, 0 = Standard) |

| Command | Char | Response | Version |
|---------|------|----------|---------|
| Get Location | `w` | 8 bytes + `#` | 2.3+ |
| Set Location | `W` + 8 bytes | `#` | 2.3+ |
| Get Time | `h` | 8 bytes + `#` | 2.3+ |
| Set Time | `H` + 8 bytes | `#` | 2.3+ |

**Example -- Set Location** (33d50'41"N, 118d20'17"W):
```
W chr(33) chr(50) chr(41) chr(0) chr(118) chr(20) chr(17) chr(1)
```

**Example -- Set Time** (3:26:00 PM, April 6 2005, Eastern time -5 UTC, DST on):
```
H chr(15) chr(26) chr(0) chr(4) chr(6) chr(5) chr(251) chr(1)
```

**Note:** Get Time/Location retrieves values from the HC, not from the GPS module. Enter the View Time/Site menu on the HC first to update from GPS.

### Miscellaneous Commands

| Command | Char | Response | Version |
|---------|------|----------|---------|
| Get HC Version | `V` | `chr(major) chr(minor) #` | 1.2+ |
| Get Model | `m` | `chr(model) #` | 2.2+ |
| Echo | `K` + `chr(x)` | `chr(x) #` | 1.2+ |
| Is Alignment Complete? | `J` | `chr(0 or 1) #` | 1.2+ |
| Is GOTO in Progress? | `L` | ASCII `0` or `1` + `#` | 1.2+ |
| Cancel GOTO | `M` | `#` | 1.2+ |
| Get Pier Side | `p` | ASCII `W` or `E` + `#` | GEM only |

#### Model IDs

| Value | Model |
|-------|-------|
| 1 | GPS Series |
| 3 | i-Series |
| 4 | i-Series SE |
| 5 | CGE |
| 6 | Advanced GT |
| 7 | SLT |
| 9 | CPC |
| 10 | GT |
| 11 | 4/5 SE |
| 12 | 6/8 SE |
| 14 | CGX |
| 20 | CGX-L |
| 22 | Evolution |

#### Pier Side (`p` command)

GEM mounts only (CGE, CGX, CGX-L). Returns `W#` when the OTA is on the west side of the pier (normal pointing, counterweight down) or `E#` when on the east side (through-the-pole / counterweight up). Not documented in the original NexStar protocol spec; discovered in INDI driver source (`celestrondriver.cpp:1031`).

ASCOM mapping: `W` → pierEast (0), `E` → pierWest (1). Alt-az mounts return an empty or undefined response.

#### Get Device Version (via pass-through)

```
P chr(1) chr(dev) chr(0xFE) chr(0) chr(0) chr(0) chr(2)
```

Response: `chr(major) chr(minor) #`

Known device IDs for version query: 16 = AZM/RA Motor, 17 = ALT/DEC Motor, 176 = GPS Unit, 178 = RTC (CGE only).

---

## Pass-Through Command Format

The `P` command (0x50) relays AUX bus commands through the HC serial port. This is a fixed 8-byte message:

```
Byte 0: 'P'         (0x50)
Byte 1: msg_len      Number of bytes in the AUX message (cmd_id + data bytes, range 1-4)
Byte 2: dest_id      Target device address (see AUX Bus Device Addresses)
Byte 3: cmd_id       AUX command identifier
Byte 4: data1        Data byte 1 (zero if unused)
Byte 5: data2        Data byte 2 (zero if unused)
Byte 6: data3        Data byte 3 (zero if unused)
Byte 7: resp_len     Number of response bytes expected
```

Response: `<resp_len bytes> '#'`

**Error detection:** If the target device is absent, the command is unknown, or communication fails, one extra garbage byte appears before the `#` terminator. Check for `#` at the expected position; if it is a different character, consume the next character (which should be `#`) and handle the error.

**Example -- Get MC firmware version:**
```
Send:     P chr(1) chr(0x11) chr(0xFE) chr(0) chr(0) chr(0) chr(2)
Response: chr(major) chr(minor) '#'
```

**Timing:** Wait up to 3.5 seconds for a response. Always wait for the complete `#`-terminated response before sending the next command.

---

## AUX Bus Device Addresses

### Original Devices (Andre Paquette, 2003)

| Address | Name | Description |
|---------|------|-------------|
| `0x01` | Main Board | Main / Interconnect Board |
| `0x04` | HC | Hand Controller |
| `0x10` | AZM MC | AZM / RA Motor Controller |
| `0x11` | ALT MC | ALT / DEC Motor Controller |
| `0xB0` | GPS | GPS / Compass Module |

### Hand Controllers

| Address | Constant | Description |
|---------|----------|-------------|
| `0x04` | DEV_HC | NexStar Hand Controller (original) |
| `0x0D` | DEV_HC_PLUS | NexStar+ Hand Controller |
| `0x0E` | -- | StarSense Hand Controller |

### Accessories & Peripherals

| Address | Constant | Description |
|---------|----------|-------------|
| `0x12` | DEV_FOCUSER | Focuser Motor |
| `0x17` | DEV_DEW | Dew Heater Controller |
| `0xB0` | DEV_GPS | GPS Module |
| `0xB2` | DEV_RTC | Real-Time Clock (CGE only) |
| `0xB4` | -- | StarSense Camera |
| `0xB5` | DEV_WIFI | WiFi Module (Evolution) |
| `0xB6` | DEV_BAT | Battery / Power Controller |
| `0xBF` | DEV_LIGHT | Mount Light Controller |

### CGX / CGX-L Specific

| Address | Constant | Description |
|---------|----------|-------------|
| `0x30` | DEV_RA_SW | RA Limit / Index Switch |
| `0x31` | DEV_DEC_SW | DEC Limit / Index Switch |
| `0x32` | DEV_DEC_AG | DEC Autoguider Port |

### Software Source Addresses (not queryable devices)

| Address | Name | Description |
|---------|------|-------------|
| `0x20` | APP | PC application (CPWI, SkyPortal) |
| `0x21` | CFM | Celestron Firmware Manager |
| `0x2F` | SCANNER | AUX Bus Scanner tool |

### Bus Enumeration

To discover which devices are present, send `GET_VER (0xFE)` to each known address. A valid 2-byte response means the device exists; a timeout or extra-byte-before-`#` means absent.

```
P chr(1) chr(device) chr(0xFE) chr(0) chr(0) chr(0) chr(2)
```

Example CGX-L bus scan results:

| Device | Address | Firmware |
|--------|---------|----------|
| NexStar+ HC | 0x0D | 5.35 |
| RA Motor Controller | 0x10 | 7.18 |
| RA Switch | 0x30 | 1.0 |
| DEC Switch | 0x31 | 1.0 |
| DEC Autoguider | 0x32 | 1.0 |

---

## Motor Controller Commands (AUX)

All commands in this section target device `0x10` (AZM/RA) or `0x11` (ALT/DEC) unless noted otherwise. Position values are 24-bit unsigned fractions of a full rotation.

### Complete MC Command Table

| Cmd ID | Name | Tx Data | Response Data | Description |
|--------|------|---------|---------------|-------------|
| `0x01` | MC_GET_POSITION | -- | 24 bits | Get current position |
| `0x02` | MC_GOTO_FAST | 16/24 bits | Ack | GOTO at rate 9 |
| `0x04` | MC_SET_POSITION | 24 bits | Ack | Set current position |
| `0x06` | MC_SET_POS_GUIDERATE | 16/24 bits | Ack | Set positive guide rate |
| `0x07` | MC_SET_NEG_GUIDERATE | 16/24 bits | Ack | Set negative guide rate |
| `0x0B` | MC_LEVEL_START | -- | Ack | Start leveling / home seek |
| `0x0C` | MC_PEC_RECORD_START | -- | Ack | Start PEC recording |
| `0x0D` | MC_PEC_PLAYBACK | 8 bits | Ack | Start (0x01) or stop (0x00) PEC playback |
| `0x0E` | MTR_PECBIN | -- | 8 bits | Current PEC bin number |
| `0x10` | MC_SET_POS_BACKLASH | 8 bits | Ack | Set positive backlash (0-99) |
| `0x11` | MC_SET_NEG_BACKLASH | 8 bits | Ack | Set negative backlash (0-99) |
| `0x12` | MC_LEVEL_DONE | -- | 8 bits | 0xFF = done, 0x00 = in progress |
| `0x13` | MC_SLEW_DONE | -- | 8 bits | 0xFF = done, 0x00 = slewing |
| `0x15` | MC_PEC_RECORD_DONE | -- | 8 bits | 0xFF = done, 0x00 = recording |
| `0x16` | MC_PEC_RECORD_STOP | -- | -- | Stop PEC recording |
| `0x17` | MC_GOTO_SLOW | 16/24 bits | Ack | GOTO at slow variable rate |
| `0x18` | MC_AT_INDEX | -- | 8 bits | 0xFF = at index, 0x00 = not at index |
| `0x19` | MC_SEEK_INDEX | -- | -- | Seek nearest index marker |
| `0x20` | MC_SET_MAXRATE | 16 bits | -- | Set max slew rate (10^-3 deg/s) |
| `0x21` | MC_GET_MAXRATE | -- | 32 bits | Get max slew rate |
| `0x22` | MC_ENABLE_MAXRATE | 8 bits | -- | Enable/disable rate limit |
| `0x23` | MC_MAXRATE_ENABLED | -- | 8 bits | Poll rate limit active |
| `0x24` | MC_MOVE_POS | 8 bits | Ack | Move positive (up/right), rate 0-9 |
| `0x25` | MC_MOVE_NEG | 8 bits | Ack | Move negative (down/left), rate 0-9 |
| `0x26` | MTR_AUX_GUIDE | 2 bytes | -- | Hardware pulse guide |
| `0x27` | MTR_IS_AUX_GUIDE_ACTIVE | -- | 8 bits | 1 = active, 0 = expired |
| `0x38` | MC_ENABLE_CORDWRAP | -- | -- | Enable cord wrap (AZM only) |
| `0x39` | MC_DISABLE_CORDWRAP | -- | -- | Disable cord wrap (AZM only) |
| `0x3A` | MC_SET_CORDWRAP_POS | 24 bits | -- | Set cord wrap position (AZM only) |
| `0x3B` | MC_POLL_CORDWRAP | -- | 8 bits | 0xFF = enabled, 0x00 = disabled |
| `0x3C` | MC_GET_CORDWRAP_POS | -- | 24 bits | Get cord wrap position (AZM only) |
| `0x40` | MC_GET_POS_BACKLASH | -- | 8 bits | Get positive backlash |
| `0x41` | MC_GET_NEG_BACKLASH | -- | 8 bits | Get negative backlash |
| `0x46` | MC_SET_AUTOGUIDE_RATE | 8 bits | Ack | Set autoguide rate |
| `0x47` | MC_GET_AUTOGUIDE_RATE | -- | 8 bits | Get autoguide rate |
| `0xFC` | MC_GET_APPROACH | -- | 8 bits | 0 = positive, 1 = negative |
| `0xFD` | MC_SET_APPROACH | 8 bits | Ack | 0 = positive, 1 = negative |
| `0xFE` | MC_GET_VER | -- | 16 bits | MSB = major, LSB = minor |

### Position & GOTO

**MC_GET_POSITION (0x01)** returns the 24-bit position as a fraction of a full rotation.

```
Get RA position:  P chr(1) chr(0x10) chr(0x01) chr(0) chr(0) chr(0) chr(3)
Get DEC position: P chr(1) chr(0x11) chr(0x01) chr(0) chr(0) chr(0) chr(3)
```

**MC_GOTO_FAST (0x02)** moves to a position at maximum rate (rate 9). Accepts 16 or 24-bit position.

```
GOTO RA (24-bit): P chr(3) chr(0x10) chr(0x02) chr(posH) chr(posM) chr(posL) chr(0)
GOTO DEC (24-bit): P chr(3) chr(0x11) chr(0x02) chr(posH) chr(posM) chr(posL) chr(0)
```

**MC_GOTO_SLOW (0x17)** moves to a position at a slow variable rate. Same format as MC_GOTO_FAST.

**MC_SET_POSITION (0x04)** sets the current position counter without moving.

```
Set RA position: P chr(3) chr(0x10) chr(0x04) chr(posH) chr(posM) chr(posL) chr(0)
```

**MC_SLEW_DONE (0x13)** polls per-axis slew status. More granular than the HC-level `L` command.

```
Poll RA slew:  P chr(1) chr(0x10) chr(0x13) chr(0) chr(0) chr(0) chr(1)
Poll DEC slew: P chr(1) chr(0x11) chr(0x13) chr(0) chr(0) chr(0) chr(1)
```

Response: 0xFF = slew complete, 0x00 = still slewing. Axes are independent -- when one returns 0xFF, only continue polling the other.

**Caution:** Polling MC_SLEW_DONE too frequently (back-to-back) can cause the MC to miss its destination and keep rotating.

### Fixed/Variable Rate Slewing

**MC_MOVE_POS (0x24) / MC_MOVE_NEG (0x25)** -- Fixed rate slew using HC-equivalent rates 0-9 (0 = stop).

```
Start RA move right at rate 5: P chr(2) chr(0x10) chr(0x24) chr(5) chr(0) chr(0) chr(0)
Stop RA move:                  P chr(2) chr(0x10) chr(0x24) chr(0) chr(0) chr(0) chr(0)
Start DEC move up at rate 3:   P chr(2) chr(0x11) chr(0x24) chr(3) chr(0) chr(0) chr(0)
```

**MC_SET_POS_GUIDERATE (0x06) / MC_SET_NEG_GUIDERATE (0x07)** -- Variable rate guide/tracking.

With 24-bit data: value is an absolute rate.
With 16-bit data: special values:

| Value | Rate |
|-------|------|
| 0xFFFF | Sidereal |
| 0xFFFE | Solar |
| 0xFFFD | Lunar |

EQ North tracking example:
```
Stop both axes:
  P chr(3) chr(0x10) chr(0x06) chr(0) chr(0) chr(0) chr(0)
  P chr(3) chr(0x11) chr(0x06) chr(0) chr(0) chr(0) chr(0)
Start sidereal on RA:
  P chr(2) chr(0x10) chr(0x06) chr(0xFF) chr(0xFF) chr(0) chr(0)
```

EQ South uses MC_SET_NEG_GUIDERATE (0x07) instead. Alt/Az tracking updates both axes every ~30 seconds based on current position.

### Home / Level

**MC_LEVEL_START (0x0B)** initiates the leveling or home-seeking process.
**MC_LEVEL_DONE (0x12)** polls completion: 0xFF = done, 0x00 = in progress.

Original NexStar GPS behavior (per Andre Paquette):
- Applicable to ALT only. The telescope slews upward to find the level position.
- Sending MC_LEVEL_DONE to AZM returns 0x80 (not a valid done/not-done value).

CGX/CGX-L behavior (AlpacaBridge validated):
- MC_LEVEL_START works on BOTH axes when hardware home switches (0x30, 0x31) are present.
- MC_LEVEL_DONE returns proper 0x00/0xFF on both axes.

```
Start RA home:   P chr(1) chr(0x10) chr(0x0B) chr(0) chr(0) chr(0) chr(0)
Start DEC home:  P chr(1) chr(0x11) chr(0x0B) chr(0) chr(0) chr(0) chr(0)
Poll RA done:    P chr(1) chr(0x10) chr(0x12) chr(0) chr(0) chr(0) chr(1)
Poll DEC done:   P chr(1) chr(0x11) chr(0x12) chr(0) chr(0) chr(0) chr(1)
```

See the AlpacaBridge Driver Implementation Notes section for important details on serial timing when sending MC_LEVEL_START to both axes via pass-through.

### PEC Index

**MC_SEEK_INDEX (0x19)** seeks to the nearest PEC worm gear index marker.
**MC_AT_INDEX (0x18)** polls whether the axis has reached the index: 0xFF = at index, 0x00 = seeking.

These commands are for the **RA/AZM axis only** (device `0x10`). They find the optical encoder mark on the worm gear, moving the axis a small amount (~2 degrees). Do NOT use these for home-seeking.

```
Seek RA index:  P chr(1) chr(0x10) chr(0x19) chr(0) chr(0) chr(0) chr(0)
Poll RA index:  P chr(1) chr(0x10) chr(0x18) chr(0) chr(0) chr(0) chr(1)
```

### PEC Operations

PEC operates on the **RA axis only** (device `0x10`). It records and plays back corrections for the periodic error of the worm gear. The mount must be in EQ North/South tracking mode.

| Cmd ID | Name | Description |
|--------|------|-------------|
| `0x0C` | MC_PEC_RECORD_START | Start recording PEC data |
| `0x16` | MC_PEC_RECORD_STOP | Stop PEC recording |
| `0x15` | MC_PEC_RECORD_DONE | 0xFF = done, 0x00 = recording |
| `0x0D` | MC_PEC_PLAYBACK | 0x01 = start, 0x00 = stop |
| `0x0E` | MTR_PECBIN | Current PEC bin number |

**PEC workflow:**

```
1. Seek RA index mark:
   P chr(1) chr(0x10) chr(0x19) chr(0) chr(0) chr(0) chr(0)

2. Poll until at index (repeat until response = 0xFF):
   P chr(1) chr(0x10) chr(0x18) chr(0) chr(0) chr(0) chr(1)

3. Start recording (mount must be tracking):
   P chr(1) chr(0x10) chr(0x0C) chr(0) chr(0) chr(0) chr(0)

4. Poll recording completion (one full worm revolution, ~8 minutes):
   P chr(1) chr(0x10) chr(0x15) chr(0) chr(0) chr(0) chr(1)

5. Stop recording:
   P chr(1) chr(0x10) chr(0x16) chr(0) chr(0) chr(0) chr(0)

6. Start playback:
   P chr(2) chr(0x10) chr(0x0D) chr(1) chr(0) chr(0) chr(0)

7. Stop playback:
   P chr(2) chr(0x10) chr(0x0D) chr(0) chr(0) chr(0) chr(0)
```

PEC data persists in the motor controller across power cycles. The current PEC bin can be monitored during recording/playback via MTR_PECBIN (0x0E).

### Pulse Guiding

**MTR_AUX_GUIDE (0x26)** sends a hardware pulse guide command. Available when an autoguider port device (`0x32`) is detected on the AUX bus.

**MTR_IS_AUX_GUIDE_ACTIVE (0x27)** returns 1 while a pulse guide is in progress, 0 when expired.

Parameters for MTR_AUX_GUIDE:
- **Byte 0 (velocity):** Signed byte, percentage of sidereal rate. Positive = North (DEC) or West (RA). Negative = South (DEC) or East (RA). Range: -100 to +100.
- **Byte 1 (duration):** Unsigned byte, time in centiseconds (10ms units). Max 255 = 2550ms.

```
Guide RA west at 50% for 500ms:     P chr(3) chr(0x10) chr(0x26) chr(50)  chr(50)  chr(0) chr(0)
Guide DEC north at 75% for 1000ms:  P chr(3) chr(0x11) chr(0x26) chr(75)  chr(100) chr(0) chr(0)
Guide RA east at 50% for 500ms:     P chr(3) chr(0x10) chr(0x26) chr(-50) chr(50)  chr(0) chr(0)
Poll RA guide active:                P chr(1) chr(0x10) chr(0x27) chr(0)   chr(0)   chr(0) chr(1)
```

### Autoguide Rate

**MC_SET_AUTOGUIDE_RATE (0x46)** / **MC_GET_AUTOGUIDE_RATE (0x47)**

Encoding: `val = desired_percent * 256 / 100`. Decoding: `percent = 100 * val / 256`.

Common values: 50% = 128 (0x80), 10% = 26 (0x1A), 90% = 230 (0xE6).

```
Set RA guide rate to 50%:  P chr(2) chr(0x10) chr(0x46) chr(128) chr(0) chr(0) chr(0)
Get DEC guide rate:        P chr(1) chr(0x11) chr(0x47) chr(0)   chr(0) chr(0) chr(1)
```

### Backlash

**MC_SET_POS_BACKLASH (0x10)** / **MC_SET_NEG_BACKLASH (0x11)** -- Set backlash compensation. Valid range: 0-99.
**MC_GET_POS_BACKLASH (0x40)** / **MC_GET_NEG_BACKLASH (0x41)** -- Get current backlash value.

```
Set RA positive backlash to 5: P chr(2) chr(0x10) chr(0x10) chr(5) chr(0) chr(0) chr(0)
Get RA positive backlash:      P chr(1) chr(0x10) chr(0x40) chr(0) chr(0) chr(0) chr(1)
```

### Cord Wrap

Cord wrap prevents the mount from rotating past a set azimuth position to protect cables. Applicable to **AZM axis only** (device `0x10`).

| Cmd ID | Name | Description |
|--------|------|-------------|
| `0x38` | MC_ENABLE_CORDWRAP | Enable cord wrap |
| `0x39` | MC_DISABLE_CORDWRAP | Disable cord wrap |
| `0x3A` | MC_SET_CORDWRAP_POS | Set cord wrap position (24-bit) |
| `0x3B` | MC_POLL_CORDWRAP | 0xFF = enabled, 0x00 = disabled |
| `0x3C` | MC_GET_CORDWRAP_POS | Get cord wrap position (24-bit) |

When setting the cord wrap position, it should be set to the current AZM position + 180 degrees mod 360 degrees.

```
Enable cord wrap:   P chr(1) chr(0x10) chr(0x38) chr(0) chr(0) chr(0) chr(0)
Disable cord wrap:  P chr(1) chr(0x10) chr(0x39) chr(0) chr(0) chr(0) chr(0)
Set position:       P chr(3) chr(0x10) chr(0x3A) chr(posH) chr(posM) chr(posL) chr(0)
Poll status:        P chr(1) chr(0x10) chr(0x3B) chr(0) chr(0) chr(0) chr(1)
Get position:       P chr(1) chr(0x10) chr(0x3C) chr(0) chr(0) chr(0) chr(3)
```

### Approach Direction

**MC_GET_APPROACH (0xFC)** / **MC_SET_APPROACH (0xFD)** -- Controls which direction the mount approaches a GOTO target from. 0 = positive, 1 = negative.

```
Set RA approach to positive: P chr(2) chr(0x10) chr(0xFD) chr(0) chr(0) chr(0) chr(0)
Get DEC approach:            P chr(1) chr(0x11) chr(0xFC) chr(0) chr(0) chr(0) chr(1)
```

### Max Slew Rate

| Cmd ID | Name | Description |
|--------|------|-------------|
| `0x20` | MC_SET_MAXRATE | Set max rate (16-bit, units of 10^-3 deg/s) |
| `0x21` | MC_GET_MAXRATE | Get max rate (32-bit response) |
| `0x22` | MC_ENABLE_MAXRATE | Enable (0x01) / disable (0x00) rate limit |
| `0x23` | MC_MAXRATE_ENABLED | Poll: 1 = active, 0 = inactive |

### Firmware Version

**MC_GET_VER (0xFE)** returns 2 bytes: MSB = major version, LSB = minor version. This command ID is shared across all device types (MC, GPS, Main board).

```
Get RA MC version:  P chr(1) chr(0x10) chr(0xFE) chr(0) chr(0) chr(0) chr(2)
Get DEC MC version: P chr(1) chr(0x11) chr(0xFE) chr(0) chr(0) chr(0) chr(2)
```

---

## GPS Commands (AUX)

All GPS commands target device `0xB0` (176 decimal). Times are in UTC/GMT.

| Cmd ID | Name | Response | Description |
|--------|------|----------|-------------|
| `0x01` | GPS_GET_LAT | 24 bits | Latitude as signed fraction of 360 degrees |
| `0x02` | GPS_GET_LONG | 24 bits | Longitude as signed fraction of 360 degrees |
| `0x03` | GPS_GET_DATE | 16 bits | Month (byte 1), Day (byte 2) |
| `0x04` | GPS_GET_YEAR | 16 bits | Year as 16-bit integer (e.g., 0x07D3 = 2003) |
| `0x07` | GPS_GET_SAT_INFO | 16 bits | Visible satellites (byte 1), tracked (byte 2) |
| `0x08` | GPS_GET_RCVR_STATUS | 16 bits | Receiver status bitmap (see below) |
| `0x33` | GPS_GET_TIME | 24 bits | Hours, minutes, seconds |
| `0x36` | GPS_TIME_VALID | 8 bits | 0 = no, 1 = yes |
| `0x37` | GPS_LINKED | 8 bits | 0 = not linked, 1 = linked |
| `0xA0` | GPS_GET_COMPASS | 8 bits | Compass heading (8 ordinal values) |
| `0x55` | GPS_GET_HW_VER | 8 bits | Returns 0xAB (Motorola GPS HW version) |
| `0xFE` | GPS_GET_VER | 16 bits | Firmware version (major, minor) |

### Pass-through examples

```
Is GPS linked:    P chr(1) chr(0xB0) chr(0x37) chr(0) chr(0) chr(0) chr(1)
Get latitude:     P chr(1) chr(0xB0) chr(0x01) chr(0) chr(0) chr(0) chr(3)
Get longitude:    P chr(1) chr(0xB0) chr(0x02) chr(0) chr(0) chr(0) chr(3)
Get date:         P chr(1) chr(0xB0) chr(0x03) chr(0) chr(0) chr(0) chr(2)
Get year:         P chr(1) chr(0xB0) chr(0x04) chr(0) chr(0) chr(0) chr(2)
Get time:         P chr(1) chr(0xB0) chr(0x33) chr(0) chr(0) chr(0) chr(3)
Time valid:       P chr(1) chr(0xB0) chr(0x36) chr(0) chr(0) chr(0) chr(1)
Get compass:      P chr(1) chr(0xB0) chr(0xA0) chr(0) chr(0) chr(0) chr(1)
Get GPS version:  P chr(1) chr(0xB0) chr(0xFE) chr(0) chr(0) chr(0) chr(2)
```

### Latitude/Longitude Encoding

24-bit signed fractions of a full rotation. To convert to degrees: `degrees = value / 2^24 * 360`.

Example: GPS_GET_LAT returns `0x20, 0x3E, 0x35` -> 0x203E35 = 2113077 -> 2113077 / 16777216 * 360 = +45.34 degrees (N).

### Compass Heading Values

| Direction | Value |
|-----------|-------|
| N | 0x0B |
| NE | 0x09 |
| E | 0x0D |
| SE | 0x0C |
| S | 0x0E |
| SW | 0x06 |
| W | 0x07 |
| NW | 0x03 |

The HC finds true north by rotating to find the NW->N and NE->N transition points, then slewing to the midpoint.

### Receiver Status Bitmap (GPS_GET_RCVR_STATUS)

| Bits | Field |
|------|-------|
| 15-13 | Fix type: 111=3D, 110=2D, 101=Propagate, 100=Position Hold, 011=Acquiring, 010=Bad Geometry |
| 12-11 | Reserved |
| 10 | Narrow track mode (timing only) |
| 9 | Fast acquisition position |
| 8 | Filter reset to raw GPS solution |
| 7 | Cold start |
| 6 | Differential fix |
| 5 | Position lock |
| 4 | Autosurvey mode |
| 3 | Insufficient visible satellites |
| 2-1 | Antenna sense: 00=OK, 01=OC, 10=UC, 11=NV |
| 0 | Code location: 0=external, 1=internal |

---

## RTC Commands (CGE only)

The CGE mount has a real-time clock (device `0xB2`, 178 decimal) that is separate from the GPS module.

| Command | Pass-through | Response | Version |
|---------|-------------|----------|---------|
| Get Date | `P chr(1) chr(0xB2) chr(0x03) chr(0) chr(0) chr(0) chr(2)` | month, day + `#` | 1.6+ |
| Get Year | `P chr(1) chr(0xB2) chr(0x04) chr(0) chr(0) chr(0) chr(2)` | year_hi, year_lo + `#` | 1.6+ |
| Get Time | `P chr(1) chr(0xB2) chr(0x33) chr(0) chr(0) chr(0) chr(3)` | hours, min, sec + `#` | 1.6+ |
| Set Date | `P chr(3) chr(0xB2) chr(0x83) chr(month) chr(day) chr(0) chr(0)` | `#` | 3.01+ |
| Set Year | `P chr(3) chr(0xB2) chr(0x84) chr(year_hi) chr(year_lo) chr(0) chr(0)` | `#` | 3.01+ |
| Set Time | `P chr(4) chr(0xB2) chr(0xB3) chr(hours) chr(min) chr(sec) chr(0)` | `#` | 3.01+ |

Year is encoded as a 16-bit integer: `year = year_hi * 256 + year_lo`.

---

## HC-Level Extended Commands (firmware 4.21+)

These commands were discovered in newer hand controller firmware versions and are not in the original Celestron RS-232 spec.

### Set Home Position

| Command | Char | Response | Notes |
|---------|------|----------|-------|
| Set Home Position | `n` (0x6E) | `#` | Stores the current position as the home position |

### GOTO Home Position

| Command | Char | Response | Notes |
|---------|------|----------|-------|
| GOTO Home Position | `o` (0x6F) | `#` | Slews to the stored home position |

These commands were found in the INDI Celestron driver source. The `n` command defines the current position as "home" and the `o` command initiates a GOTO to that stored position.

---

## AlpacaBridge Driver Implementation Notes

This section documents what we discovered during actual driver development on a CGX-L mount that corrects or extends the original protocol documentation.

### 1. MC_LEVEL_START / MC_LEVEL_DONE -- Home Operation on CGX/CGX-L

The original AUX doc (2003) says MC_LEVEL_START (0x0B) and MC_LEVEL_DONE (0x12) are "Applicable to ALT only" and describes them as leveling commands for the NexStar GPS scopes.

On CGX-L mounts with hardware home switches (devices `0x30` RA Switch, `0x31` DEC Switch), MC_LEVEL_START works on **both axes** to move to the hardware home position. This is how the INDI Celestron driver implements FindHome.

**Important serial timing:** When using pass-through (P command), you MUST wait for the `#` response from each MC_LEVEL_START before sending the next. Using fire-and-forget corrupts the second command due to leftover response bytes in the serial buffer.

**MC_LEVEL_DONE behavior differences:**
- NexStar GPS (original): MC_LEVEL_DONE on AZM returns 0x80 (not a valid done/not-done value).
- CGX-L: MC_LEVEL_DONE returns proper 0x00 (in progress) / 0xFF (done) on both axes.

**ASCOM/Alpaca integration:** The FindHome operation must be asynchronous -- send MC_LEVEL_START to both axes, return immediately, then poll MC_LEVEL_DONE via the Slewing/AtHome properties.

### 2. MC_SEEK_INDEX / MC_AT_INDEX -- PEC Index, NOT Home

MC_SEEK_INDEX (0x19) and MC_AT_INDEX (0x18) are specifically for finding the PEC worm gear index mark on the **RA/AZM axis only**. They only move the RA axis a small amount (~2 degrees) to find the worm gear optical encoder mark.

Do NOT use these for "Go Home." Sending MC_SEEK_INDEX to the DEC axis has no effect or returns an error.

### 3. Serial Port Auto-Detection

NexStar mounts can be detected by:

1. Send the Echo command `Kx` (where x is any byte). The mount responds with `x#`.
2. Send `V` to get firmware version. Response: `chr(major) chr(minor) #`.
3. Scan `/dev/serial/by-id/` for Prolific (PL2303), FTDI, and CP210x USB-serial adapters -- these are the common NexStar RS-232 cables.

### 4. Pass-Through Command Timing

The official spec says wait up to 3.5 seconds for a response. When sending multiple pass-through commands in sequence, always wait for the complete `#`-terminated response before sending the next command. A 40ms drain is NOT sufficient for pass-through commands that relay through the HC to the motor controller and back.

### 5. Site Location (Get/Set Location)

- Get Location: lowercase `w`, response is 8 bytes + `#`.
- Set Location: uppercase `W` + 8 bytes, response is `#`.
- Format: lat_deg, lat_min, lat_sec, lat_sign (0=N, 1=S), lon_deg, lon_min, lon_sec, lon_sign (0=E, 1=W).

The HC stores location independently from the GPS module. If no GPS is present or the mount has not been aligned, the `w` command still returns whatever was last set via `W` or the HC menu.

### 6. AUX Bus Device Addresses on CGX-L

The following device addresses are not in the original 2003 AUX documentation:

| Address | Description |
|---------|-------------|
| `0x0D` | NexStar+ Hand Controller |
| `0x0E` | StarSense Hand Controller |
| `0x12` | Focuser Motor |
| `0x17` | Dew Heater Controller |
| `0x30` | RA Limit/Index Switch (CGX/CGX-L) |
| `0x31` | DEC Limit/Index Switch (CGX/CGX-L) |
| `0x32` | DEC Autoguider Port (CGX/CGX-L) |
| `0xB4` | StarSense Camera |
| `0xB5` | WiFi Module (Evolution) |
| `0xB6` | Battery/Power Controller |
| `0xBF` | Mount Light Controller |

### 7. Pulse Guiding (MTR_AUX_GUIDE 0x26)

This command is not documented in any original protocol reference. It was discovered in the INDI and ASCOM Celestron drivers.

- Available when the autoguider port device (`0x32`) is detected on the AUX bus.
- Velocity is a signed byte: positive = North (DEC) or West (RA), negative = South or East.
- Duration is an unsigned byte in centiseconds (10ms units), max 255 = 2550ms.
- MTR_IS_AUX_GUIDE_ACTIVE (0x27) returns 1 while a pulse guide is in progress, 0 when expired.

### 8. RA Slew Offset Compensation (CGX-L fw 7.18)

CGX-L firmware 7.18 does not track during a GOTO. The mount stops sidereal tracking when a slew begins and does not resume it until the slew completes. For slews lasting several seconds, this causes a consistent RA undershoot — the sky moves during the slew but the mount doesn't follow.

The AlpacaBridge driver compensates by learning a running-average RA residual across completed slews and pre-biasing subsequent GOTO targets by the accumulated offset. This is the same pattern INDI uses (`SlewOffsetRa`). The residual converges after 3–4 slews and typically stabilizes around 2–4 arcseconds.

### 9. Post-Slew Tracking Restoration (CGX-L fw 7.18)

CGX-L firmware 7.18 does not auto-resume tracking after a GOTO completes. The driver must explicitly re-assert tracking via the HC-level `T` (Set Tracking Mode) command after each slew finishes. Using the per-axis variable rate slew command to resume sidereal tracking works but bypasses the HC's tracking state machine, which can cause the HC display to show "No Track" even though the motor is moving.

The `T` command with the mount's configured tracking mode (EQ-North = 2, EQ-South = 3) is the correct approach and keeps the HC display in sync.

### 10. Pulse Guide Position Hold/Correction Pattern

MC_AUX_GUIDE (0x26) fires the motor controller's autoguider output for a timed pulse, but it does **not** update the HC's internal RA/DEC coordinate model. Reading positions via `e`/`E` (Get RA/DEC) during or immediately after a pulse guide returns values that don't reflect the pulse guide motion.

For ASCOM/Alpaca compliance, the driver must compute and return expected positions during the pulse guide window:
- **Cross-axis hold**: The axis not being guided is frozen at its pre-pulse value for the duration of the pulse plus a grace period, preventing geometric noise (especially severe near the celestial poles where cos(DEC) amplification reaches 10x+ at DEC > 80°).
- **Active-axis correction**: The guided axis returns `baseline + (rate × duration)` as the expected final position, applied as a one-shot correction after the pulse completes.

This pattern matches the approach used by iOptron and ZWO drivers in AlpacaBridge and is necessary to pass ConformU pulse guide tolerance tests at high declinations.
