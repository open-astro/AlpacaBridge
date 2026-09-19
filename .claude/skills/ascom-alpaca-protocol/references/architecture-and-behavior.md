# Architecture and Behavioral Contract

## System boundary

ASCOM prevents every application from needing custom logic for every vendor. The driver owns vendor USB, serial, TCP, SDK, timing, and recovery details; applications own observing workflows expressed through the standard interface.

An **Alpaca Device** is a network service at an IP endpoint. It can expose one or many logical **ASCOM Devices**, each implementing a standard interface such as Camera or Telescope. Keep the network service, logical device, and physical hardware conceptually separate.

## Layers

Keep these responsibilities distinct:

1. **HTTP/Alpaca transport:** routing, parsing, transactions, HTTP status, negotiation, serialization.
2. **ASCOM device:** interface semantics, capabilities, validation, public state, asynchronous lifecycle, standard errors.
3. **Vendor adapter:** translation to SDK calls or protocol messages.
4. **Transport/SDK:** raw I/O, native-handle lifetime, timeouts, and vendor failures.

Do not expose vendor SDK objects, error codes, packets, or mutable raw state through the public ASCOM boundary.

## Core guarantee

Every public member must return normally only when its answer or accepted operation is correct and trustworthy. Otherwise return an Alpaca error.

- Do not use `-1`, zero, empty strings, `false`, stale caches, or special enums as hidden failure indicators unless that exact value is specified as a legitimate result.
- Treat getters, setters, methods, and completion properties as fallible.
- A successful setter means the value was applied according to the interface contract.
- Keep lower-level causes in logs or diagnostic detail, but make the primary message explain the failed ASCOM operation.

## State truthfulness

Public state describes the logical operation, not an inconvenient raw-sensor delay. Once an initiating method returns, all directly related state must already describe the operation as started.

For example, after `OpenShutter()` returns, a dome cannot report both `ShutterClosed` and `Slewing=false` merely because the controller takes 500 ms to energize. Publish the opening/in-progress state before returning, then reconcile it with hardware feedback.

Cached values require an explicit validity policy. After communication loss or mechanical failure, invalidate affected properties rather than serving them indefinitely.

`DeviceState` is an aggregation of operational properties. Do not assume it is an atomic cross-property snapshot unless the current Master Interface explicitly requires that. Each included item must nevertheless have the correct name and type and be consistent with its individual getter at the time it is obtained.

## Capabilities and unsupported members

- A `CanXxx` value is a promise about an inherent capability, not a transient busy or connection indicator.
- Return true only if the corresponding member is implemented and works under its documented conditions.
- The corresponding endpoint remains present when capability is false and returns the specified not-implemented or invalid-operation error.
- Do not conceal a partial implementation behind a false capability.

## Validation and units

- Validate knowable ranges, enums, units, array shapes, and cross-field constraints before issuing vendor commands.
- Use public units from the Master Interface, never vendor-native units.
- Reject non-finite numbers unless explicitly permitted.
- Distinguish an invalid parameter from a valid request that current device state prevents.
- Respect documented precedence where multiple failures apply; do not let connection checks mask an invalid parameter if the interface requires validation first.

## Self-protection

Refuse requests that would damage the device or materially degrade it: collision-prone slews, destructive limits, pointing-model corruption, unsafe thermal targets, or conflicting motion. Base the refusal on verified constraints, not arbitrary restrictions, and return an actionable error.

## Multiple clients

- Do not assume exactly one application exists.
- Do not let one client disconnect hardware still legitimately used by another when the host tracks client ownership.
- Serialize operations that cannot safely overlap while permitting safe independent reads.
- Publish coherent transitions without promising an atomic multi-property snapshot the interface does not require.
- Define cancellation and conflicting commands from the standard, not a device-specific client ritual.

## Other implementations

An in-tree driver may demonstrate project structure and reusable infrastructure. A vendor driver, INDI/INDIGO implementation, or decompiled protocol can suggest private framing, checksums, or command bytes. Treat those as hypotheses to verify against hardware. Never inherit public ASCOM capabilities, state machines, errors, endpoint behavior, or timing assumptions without independently checking the official contract.

