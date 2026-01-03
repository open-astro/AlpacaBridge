# ConformU Test Results

This folder contains ConformU test results for AlpacaCore drivers.

## Overview

ConformU (ASCOM Universal Device Conformance Checker) is used to verify that AlpacaCore drivers comply with the ASCOM Alpaca API specification. All drivers listed in [SUPPORTED-DRIVERS.md](../SUPPORTED-DRIVERS.md) have been tested and verified using ConformU.

## Structure

Test results are organized by vendor/driver:
- Each subdirectory contains ConformU test output files for a specific driver
- Test files typically include:
  - `ascom.conformu.*.txt` - Detailed test execution log
  - `conform.report.txt` - Summary report of test results

## Current Test Results

- **iOptron** - Telescope/Mount driver test results

## Notes

- All drivers must pass ConformU verification before being added to the supported drivers list
- Test results are generated using ConformU version 4.1.0 or later
- For more information about ConformU, see the [ASCOM ConformU documentation](https://ascom-standards.org/)

