# Validation and Review

## Contract extraction before coding

For each affected member, write down:

- Master Interface version and member link;
- endpoint path and verb;
- canonical parameters, location, types, and ranges;
- response type and exact keys;
- capability and state prerequisites;
- initiating/completion relationship;
- required standard errors; and
- timing or safety implications.

This prevents implementation behavior from being inferred from another driver.

## Minimum automated coverage

Test each driver without hardware through a fake SDK or transport seam where practical:

1. construction and metadata;
2. successful connect/use/disconnect;
3. failed connect with preserved actionable reason;
4. vendor I/O or SDK failure;
5. unsupported members and truthful capabilities;
6. boundary, out-of-range, malformed, and non-finite values;
7. exact Alpaca error numbers, not merely “an exception occurred”;
8. state-machine transitions, including failure after initiation;
9. repeated and racing connect/disconnect;
10. destruction or disconnect during active work;
11. public values, enums, arrays, and JSON types;
12. HTTP wrong-path, wrong-verb, missing-required-parameter, and unknown-extra-parameter behavior.

Do not design fake devices that answer instantly in every case. Exercise delays, partial reads, timeouts, stale replies, unsolicited data, and cancellation when the real transport can exhibit them.

## Concurrency review

- Identify every thread and the object that owns and joins it.
- Verify SDK/handle lifetime outlives every possible access.
- Check all state flags under the locks that protect the operation they describe.
- Look for check-then-lock races, callback-after-destruction, double close, and stale completion publication.
- Run ThreadSanitizer or the project's equivalent lifecycle stress suite when supported.
- Verify no lock is held across a long vendor call unless serialization explicitly requires it and cancellation remains possible.

## HTTP conformance review

- Enumerate all common and device-specific endpoints from the pinned OpenAPI schema.
- Check both verb and path, not only method name.
- Verify wrong verbs are rejected before device dispatch.
- Verify exact output key casing and numeric types.
- Confirm locale-independent parsing/serialization.
- Confirm HTTP 200 device failures retain the Alpaca envelope.
- Confirm protocol rejection and catastrophic failure use the selected baseline's HTTP status/body rules.
- Verify server transaction IDs remain safe under concurrent requests.

## Real hardware

Mocks cannot establish actual device behavior. On each claimed model and target platform, test:

- cold and warm connection;
- repeated reconnects;
- realistic operation duration;
- abort/halt/disconnect during work;
- cable or network loss where safe;
- hardware limits and unsafe-request refusal;
- reported state throughout motion or exposure;
- process restart and persistent identity/configuration; and
- multiple clients where the host supports them.

Record exact model, firmware, transport, platform/architecture, driver commit, conformance-tool version, date, and result. Do not generalize validation from one model to an entire product family without evidence.

## ConformU

Use the newest compatible ConformU release unless the repository documents a confirmed regression and approved replacement. A pass requires:

- zero errors;
- zero issues and warnings required by project policy;
- every timing class within target; and
- a report produced by the build under review on the declared real hardware.

Read the whole report. A congratulatory summary does not override a separate timing failure. Preserve reports with enough metadata to reproduce them.

For telescopes, follow hardware safety instructions before allowing automated motion. In AlpacaBridge, ConformU runs require a bare mount with no OTA.

## Review severity

Treat these as release blockers:

- a normal response containing an untrustworthy value;
- capability/member contradiction;
- wrong reserved error number;
- false completion after a failed asynchronous operation;
- vendor timing ritual required in clients;
- missing standard endpoint;
- unsafe operation accepted;
- use-after-close, unbounded shutdown, or race-prone connection lifecycle;
- unsupported interface version advertised; or
- claimed hardware support without compliant real-hardware evidence.

Treat documentation/version drift as a defect because it causes future generated drivers to repeat old behavior even when current code is correct.

