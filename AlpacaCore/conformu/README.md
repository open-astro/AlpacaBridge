# ConformU Test Results
<img src="https://www.openastro.net/wp-content/uploads/2026/01/AlpacaBridge.png" alt="AlpacaBridge logo" width="420">

This folder contains ConformU test results for AlpacaCore drivers.

## Overview

ConformU (ASCOM Universal Device Conformance Checker) is used to verify that AlpacaCore drivers comply with the ASCOM Alpaca API specification. All drivers listed in [SUPPORTED-DRIVERS.md](../../SUPPORTED-DRIVERS.md) have been tested and verified using ConformU.

## Structure

Test results are organized by vendor/driver:
- Each subdirectory contains ConformU test output files for a specific driver
- Files are named `Linux-arm64.txt` (or `Linux-arm64-<transport>.txt` for drivers tested over multiple connection types, e.g. USB vs Wi-Fi)
- AlpacaBridge is arm64-only; amd64/x64 ConformU reports are no longer produced or retained

## Current Test Results

- **iOptron** - Telescope/Mount driver test results
- **ZWO** - Camera driver test results (ASI174MM Mini, ASI290MM Mini, ASI462MM, ASI662MC, ASI2600MC Pro, ASI2600MM Pro) and Switch driver test results (Dew Heater for ASI2600MC Pro, ASI2600MM Pro)

## Notes

- All drivers must pass ConformU verification before being added to the supported drivers list
- Test results are generated using ConformU version 4.1.0 or later
- For more information about ConformU, see the [ASCOM ConformU documentation](https://ascom-standards.org/)

