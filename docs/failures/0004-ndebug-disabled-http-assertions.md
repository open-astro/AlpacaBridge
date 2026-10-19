# Release builds disabled HTTP test assertions

## Summary

Hand-written AlpacaHTTP tests could pass without checking their assertions under `-DNDEBUG`; a side-effecting call inside `assert()` could disappear altogether.

## Root Cause

These test programs use plain `main()`, not Catch2. The C library `assert()` macro is compiled out in Release builds. Normal local and CI test builds did not define `NDEBUG`, concealing the gap.

## Prevention

Use the always-on `EXPECT()` macro in `AlpacaHTTP/tests/test_assert.h`. It evaluates the expression once and aborts with source location on failure. Do not place required work inside a check that a build mode can erase.

## Evidence

- Rule: `.github/instructions/testing.instructions.md` (“AlpacaHTTP hand-rolled tests must not use assert”).
- Guard implementation: `AlpacaHTTP/tests/test_assert.h`; representative test use: `AlpacaHTTP/tests/test_routing.cpp`.
- Recurrence check: review or static search for `assert(` in first-party AlpacaHTTP tests; there is no dedicated automated lint gate for this rule yet.
