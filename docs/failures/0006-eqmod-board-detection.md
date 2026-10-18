# EQMOD-style direct support: classic-board detection failures

## Summary

Generalizing the SkyWatcher Wave driver to classic EQ-class boards exposed failures
that the Wave's USB interface hid: the EQM-35 Pro could be missed by discovery or
found successfully and then time out when opened. Board identity and home-sensor
capabilities also needed to be interpreted independently of Wave behavior.

## Root Cause

Wave USB ignores the configured baud rate; the EQM-35 Pro uses a real UART bridge
at 115200. A 9600-only scan missed it, and dropping the successful probe's baud
reopened it at the wrong speed. The third `:e` byte is a mount code, not a firmware
patch version. A successful `:q` response does not imply home-index sensors exist.
Discovery also lacked the shared serial-port ownership checks.

## Prevention

Carry the detected baud into the connection, decode identity by protocol fields,
and gate sensor homing on the feature bit. Claim/release serial ports through the
shared registry and check ownership during probing. The registry narrows discovery
races; it is not an inter-process file lock. Keep board-specific geometry in captured
fake profiles rather than inventing plausible values.

## Evidence

- [Canonical SkyWatcher history](../../.github/instructions/skywatcher.instructions.md),
  “EQ-class Synta boards” and “Alignment with upstream issue #230”; EQM-35 Pro
  hardware observations dated 2026-09-06.
- Regression coverage: `AlpacaCore/tests/test_skywatcher_serial.cpp`,
  `AlpacaCore/tests/test_skywatcher_async.cpp`, and captured profiles in
  `AlpacaCore/tests/fake_skywatcher_board.h`.

This is a record of the support bring-up failures, not a new claim that every
classic EQ board or feature has been hardware validated.
