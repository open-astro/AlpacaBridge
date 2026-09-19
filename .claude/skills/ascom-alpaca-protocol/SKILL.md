---
name: ascom-alpaca-protocol
description: Implement, extend, or review ASCOM Alpaca devices and drivers against the official device interfaces and HTTP/JSON protocol. Use for driver architecture, endpoint behavior, asynchronous operations, errors, discovery, management APIs, ImageBytes, conformance, and code review; do not use for application-only code that merely consumes an existing compliant driver, or for a vendor's own serial/SDK wire protocol.
---

# ASCOM Alpaca Protocol Reference

Build a reusable system component, not an adapter for one application. Hide the vendor protocol, timing, state transitions, recovery actions, and hardware quirks behind the standard ASCOM interface. A client that knows only the ASCOM contract must be able to use the device correctly.

## Reference baseline

This skill targets:

- **ASCOM Alpaca API Reference V12**, published **23 February 2026**.
- **Alpaca Device API v1**, OpenAPI **3.1.1**, including the ASCOM Platform 7 interface updates; official schema snapshot verified **18 September 2026**.
- **ASCOM Platform 7 Master Interface Definitions** for device behavior and interface versions.
- **Alpaca Discovery protocol version 1** and **Management API v1**.

Before making version-sensitive changes, read [references/version-and-sources.md](references/version-and-sources.md). If newer official material exists, identify the changed rules before applying it; do not silently mix versions.

## Source precedence

1. The current Master Interface definition governs device behavior, states, units, ranges, capabilities, and required errors.
2. The current Device or Management OpenAPI definition governs endpoint paths, verbs, parameter schemas, and response schemas.
3. The Alpaca API Reference governs the shared HTTP, JSON, discovery, management, versioning, and ImageBytes mechanics.
4. Official templates and libraries provide scaffolding only.
5. Conformance tools detect deviations but do not replace the specification.

Repository-local instructions govern project architecture, build commands, supported platforms, hardware safety, and verified device quirks unless they contradict the official ASCOM contract. When working in AlpacaBridge, read [references/alpacabridge-integration.md](references/alpacabridge-integration.md) before changing code.

Never use another driver or a simulator as the public-behavior baseline. They may contain obsolete contracts or bugs. Another implementation may provide evidence about an undocumented private vendor protocol, and an in-repository driver may demonstrate local code structure, but independently derive every public ASCOM behavior from the official contract.

## Required workflow

1. Identify the device type, advertised `InterfaceVersion`, Alpaca API version, and affected member.
2. Read the Master Interface entry. Record capability gates, ranges, units, state requirements, completion property, and specified exceptions.
3. Read the matching OpenAPI operation. Record path, verb, canonical parameter names, parameter location, response type, and whether `Value` is returned.
4. Trace HTTP parsing, vendor-neutral logic, vendor transport, state publication, error mapping, and serialization.
5. Implement the smallest compliant change. Keep vendor SDK and wire-protocol types below the device-interface boundary.
6. Test success, unsupported capability, invalid value, disconnected state, communication failure, and initial or mid-operation failure.
7. Run focused tests, the complete suite, concurrency checks where supported, and current ConformU tests on real hardware.

## Non-negotiable invariants

- Do the requested operation correctly and return truthful, current state, or return an Alpaca error. Never substitute sentinel values, guesses, stale state, or success flags for an error.
- Do not expose device-specific ordering, sleeps, retry rituals, timing windows, or recovery procedures to clients.
- Reject operations that may damage the device or materially degrade its operation.
- Every capability property must describe what the corresponding member can actually do.
- Unsupported standard members remain present and return the specified error; do not remove their routes.
- If compromised state makes a property unreliable, reading it must fail. Continue returning unrelated information only when it remains trustworthy.
- A normal return is a guarantee: the client may rely on it completely.

## Detailed references

Read only what the task needs:

- Architecture, truthfulness, capabilities, units, and vendor boundaries: [references/architecture-and-behavior.md](references/architecture-and-behavior.md)
- Paths, verbs, casing, JSON, transaction IDs, status codes, headers, locale, and versioning: [references/http-api-contract.md](references/http-api-contract.md)
- Initiating methods, completion properties, concurrency, and error numbers: [references/async-and-errors.md](references/async-and-errors.md)
- Setup pages, management endpoints, discovery packets, ports, UIDs, and multi-device hosts: [references/management-and-discovery.md](references/management-and-discovery.md)
- Complete endpoint inventory by device type and critical semantics: [references/device-api-catalog.md](references/device-api-catalog.md)
- Camera `ImageArray` and `application/imagebytes`: [references/imagebytes.md](references/imagebytes.md)
- Testing, ConformU evidence, and acceptance criteria: [references/validation-and-review.md](references/validation-and-review.md)
- AlpacaBridge instruction precedence and conflict handling: [references/alpacabridge-integration.md](references/alpacabridge-integration.md)

## Final review

Before declaring a driver complete, verify:

1. The advertised interface version and exposed members match the pinned or explicitly updated reference baseline.
2. Paths, verbs, parameters, response keys and types, numeric formatting, IDs, headers, and HTTP status are correct.
3. Related public state changes coherently when an asynchronous operation begins; do not assume `DeviceState` is an atomic snapshot unless the interface requires it.
4. Initial and mid-operation failures surface through the correct method or completion property.
5. Capabilities, unsupported operations, limits, units, and self-protection are truthful.
6. Management and discovery data identify the same devices exposed by the Device API.
7. Multi-client access cannot let one client unexpectedly disrupt another.
8. Tests cover failure paths and real timing, not only happy-path mocks.
9. Current ConformU evidence exists for supported real hardware and target platforms.
