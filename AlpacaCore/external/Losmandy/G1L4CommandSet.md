# Gemini Level 4 (G1/L4) Command Set

Source: https://gemini-2.com/G1L4CommandSet.html

---

## Overview

The Gemini system supports a subset of the Meade LX200 command set and extends it with additional Gemini-specific commands.
This document describes the **Gemini Level 4, Version 1.0 Serial Interface Command Set**.

Values shown in `< >` must be replaced with actual values.  
Alternative characters are shown in `{ }`.  
All commands terminate with `#`.

---

## Acknowledgements & Startup

| Command | Return | Description |
|-------|--------|-------------|
| `0x06` (ACK) | `B#` | Initial startup message (L4) |
|  | `b#` | Waiting for startup mode selection |
|  | `S#` | Cold start in progress |
|  | `G#` | Startup complete |

### Startup Mode Selection

| Command | Action |
|-------|--------|
| `bC#` | Cold Start |
| `bW#` | Warm Start |
| `bR#` | Warm Restart |

---

## Synchronization

```
:Cm# 
```
Returns selected object name or `No object!#`

```
:CM#
```
Same as `:Cm#` but does not affect model parameters.

---

## Focus Control

| Command | Action |
|-------|--------|
| `:F+#` | Focus in |
| `:F-#` | Focus out |
| `:FQ#` | Stop focusing |
| `:FF#` | Fast |
| `:FM#` | Medium |
| `:FS#` | Slow |

---

## Get Commands

### Position & Time

```
:GA#   Altitude
:GD#   Declination
:GR#   Right Ascension
:GH#   Hour Angle
:GS#   Sidereal Time
:GL#   Local Time
:GC#   Date
```

Formats:
- High precision: `±dd:mm:ss#`
- Low precision: `±dd°mm#`

---

### Site & Location

```
:Gg#   Longitude
:Gt#   Latitude
:GG#   UTC Offset
```

---

### System Info

```
:GB#   Display brightness
:Gm#   Side of meridian (E/W)
:GV#   Software version
```

---

## Home & Power States

| Command | Description |
|-------|-------------|
| `:hP#` | Move to home |
| `:hC#` | Move to startup |
| `:hN#` | Sleep |
| `:hW#` | Wake |
| `:h?#` | Status |

---

## Movement & Slewing

```
:MA#     Slew to target
:Q#      Stop all motion
:Qn#     Stop north
:Qs#     Stop south
:Qe#     Stop east
:Qw#     Stop west
```

---

## Guiding & Precision Moves

```
:Ma<dir><arcsec>#   Precision guide move
:Mi<dir><steps>#    Motor step move
:Mg<dir><time>#     Timed guide move
```

---

## Rate Selection

| Command | Rate |
|-------|------|
| `:RC#` | Center |
| `:RG#` | Guide |
| `:RM#` | Find |
| `:RS#` | Slew |

---

## Set Commands

### Coordinates

```
:Sa±dd*mm#   Set altitude
:Sd±dd*mm#   Set declination
:Srhh:mm.ss# Set RA
```

### Time & Date

```
:SLhh:mm:ss# Set local time
:SG±hh#      Set UTC offset
:SEhh:mm:ss# Set alarm
```

### Location

```
:Sg±ddd*mm#  Set longitude
:St±dd*mm#   Set latitude
```

---

## Site Management

```
:SM# :SN# :SO# :SP#   Set site names
:Wn#                 Select site
```

---

## Observing Log

```
:OC#   Clear log
:ON#   Object name
:OI#   Select object by catalog
```

---

## Native Gemini Commands

Native commands provide access to advanced configuration not available via LX200.

### Read Parameter
```
<<id>:<checksum># <value><checksum>#
```

### Write Parameter
```
><id>:<value><checksum>#
```

Native parameters include:
- Mount configuration
- Encoder setup
- Gear ratios
- PEC data
- Safety limits
- Tracking rates

(Refer to the official Gemini documentation for the full parameter table.)

---

## Notes

- Compatible with LX200 clients
- Extended Gemini-only features available
- Often accessed via ASCOM Alpaca or ASCOM COM drivers

---

**Generated from:** https://gemini-2.com/G1L4CommandSet.html
