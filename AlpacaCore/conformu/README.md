# ConformU Test Results
<img src="../../docs/image/ab.png" alt="AlpacaBridge logo" width="420">

This folder contains ConformU test results for AlpacaCore drivers.

## Overview

ConformU (ASCOM Universal Device Conformance Checker) is used to verify that AlpacaCore drivers comply with the ASCOM Alpaca API specification. All drivers listed in [SUPPORTED-DRIVERS.md](../../SUPPORTED-DRIVERS.md) have been tested and verified using ConformU.

## Structure

Test results are organized by vendor/driver:
- Each subdirectory contains ConformU test output files for a specific driver
- Files are named `Linux-arm64.txt` (or `Linux-arm64-<transport>.txt` for drivers tested over multiple connection types, e.g. USB vs Wi-Fi)
- AlpacaBridge is arm64-only; amd64/x64 ConformU reports are no longer produced or retained

## Current Test Results

One folder per vendor, one subfolder per tested model. See [SUPPORTED-DRIVERS.md](../../SUPPORTED-DRIVERS.md) for the device type, connection, firmware, and ConformU version behind each report.

- **Astroasis** - Oasis Focuser
- **Celestron** - CGX-L telescope (NexStar hand controller)
- **Gemini** - Astro Flat Panel Cover Lite, Astro Automatic FlatPanel v2, Motorized Flat Panel V3 (CoverCalibrator); Astro Focuser Pro (Focuser); Power & Data Hubs Advanced 3 (Switch)
- **iOptron** - HEM27, HAE43, HAE29C, HAE16 telescopes; iCAM178M, iCAM462C, iCAM464C cameras; iEFW-15, iEFW-18 filter wheels; iEAF, iAFS2 focusers; iMate PowerBox switch
- **OnStep** - Generic OnStep telescope
- **Player One** - Ceres 462M, Uranus-C PRO, Mars-C II cameras; PW8 filter wheel; Uranus-C PRO thermal switch
- **QHY** - QHY268C, miniCam8M cameras; miniCam8M CFW filter wheel
- **SkyWatcher** - Wave 100i telescope (direct motor controller, USB and Wi-Fi reports)
- **SVBONY** - SV905C2 camera; SC715C camera (rebadged ToupTek G3M715C, served by the ToupTek driver)
- **SynScan** - Sky-Watcher HEQ5 PRO and EQM-35 Pro telescopes via SynScan V3/V4 hand controller (serial); site coordinates rounded to whole degrees
- **ToupTek** - GPCMOS01200KPF, GPCMOS02000KPA, ATR2600M, GPM662M, ATR585M cameras (also G3M715C, whose report is filed under `SVBONY/SC715C/` after the badge it was validated under); AFW-M filter wheel; AAF focuser; ATR2600M and ATR585M thermal switches; StellaVita PowerBox switch
- **WandererAstro** - WandererCover V4 (CoverCalibrator); SFW36S filter wheel; WandererRotator Mini V2 (Rotator); WandererBox Pro V3 (Switch)
- **WeeWX** - HTTP JSON ObservingConditions source
- **ZWO** - ASI120MM Mini, ASI174MM Mini, ASI290MM Mini, ASI462MM, ASI585MC Pro, ASI662MC, ASI2600MC Pro, ASI2600MM Pro cameras; EFW filter wheel; EAF focuser; CAA rotator; Dew Heater, ASIAIR Pro, ASIAIR Plus (Pi CM4 and RK3568) switches; AM3, AM5N telescopes

## Notes

- **Scrub observing-site coordinates before committing a telescope report.** ConformU reads
  `SiteLatitude` / `SiteLongitude` / `SiteElevation` off the mount and prints them at
  house-level precision, several times each (including the "restored original" and derived
  "Test value" lines). Round latitude and longitude to the nearest degree, keeping the
  hemisphere and rough region so the log stays coherent, and elevation to the nearest 100 m.
  ConformU formats with the machine's locale, so check for a comma decimal separator
  (`+48:03:00,0`) as well as a period. This is a privacy matter for the contributor who ran
  the test, not a correctness one — the assertions the report exists to carry are unaffected.
- All drivers must pass ConformU verification before being added to the supported drivers list
- Test results are generated using ConformU version 4.1.0 or later
- For more information about ConformU, see the [ASCOM ConformU documentation](https://ascom-standards.org/)

