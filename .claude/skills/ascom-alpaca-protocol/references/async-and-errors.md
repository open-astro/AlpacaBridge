# Asynchronous Operations and Errors

## Two-part operation model

Most long-running ASCOM operations have:

1. an **initiating method** that accepts and starts work; and
2. a documented **completion property** that reports progress and successful completion.

Some historical members are synchronous. Determine the model from the current Master Interface, not the method name.

## Initiating method

Return normally only after parameters and current state are valid, the device accepted the request, work has started or is irrevocably committed to immediate execution, related public state is in-progress, and completion is reasonably expected.

If a known condition prevents starting, return the error from the initiator. Do not report success and defer a known refusal to later polling.

## Completion property

While work continues, report the documented incomplete state. Report complete only after verified success.

If work fails after initiation:

- preserve the failure until observable by the client;
- make the next relevant completion-property read return an Alpaca error, not false completion;
- fail other compromised properties instead of inventing values;
- continue serving unrelated trustworthy information; and
- define when recovery, reconnection, or a later valid command clears the stored failure.

## Timing consistency

Publish transition state before returning from the initiator. Typical expectations include:

- dome opening: opening/in-progress state;
- telescope async slew: `Slewing=true`;
- rotator move: `IsMoving=true` with coherent target;
- focuser move: `IsMoving=true` and mode-consistent target;
- switch async change: `StateChangeComplete=false` until verified.

The driver absorbs raw hardware startup delay and later reconciles logical state with feedback.

## Workers and concurrency

- Avoid high-frequency background polling without a device or client need.
- Bound I/O and shutdown; disconnect/destruction must not hang forever.
- Prevent workers from accessing closed SDK handles or publishing after a newer operation supersedes them.
- Use operation generations, cancellation tokens, or equivalent ownership to reject stale completions.
- Make racing connect/disconnect requests deterministic; no request may silently disappear.
- Wake a blocking vendor operation before joining its worker when aborting or disconnecting.

## Alpaca error transport

A valid, understood request whose ASCOM operation fails returns HTTP 200 with canonical transaction fields, nonzero `ErrorNumber`, a useful non-null `ErrorMessage`, and no misleading success `Value`. Protocol rejection happens before execution and follows the HTTP rules in `http-api-contract.md`.

## Reserved errors

| Meaning | Hex | Decimal |
| --- | ---: | ---: |
| Success | `0x000` | 0 |
| Property or method not implemented | `0x400` | 1024 |
| Invalid value | `0x401` | 1025 |
| Value not set | `0x402` | 1026 |
| Not connected | `0x407` | 1031 |
| Invalid while parked | `0x408` | 1032 |
| Invalid while slaved | `0x409` | 1033 |
| Invalid operation | `0x40B` | 1035 |
| Action not implemented | `0x40C` | 1036 |

Driver-specific errors occupy `0x500` through `0xFFF`. Prefer an accurate reserved error. Keep custom meanings stable and documented.

## Selection rules

- Missing standard implementation: not implemented.
- Unsupported `Action` name: action not implemented.
- Malformed, out-of-range, or semantically invalid parameter: invalid value.
- Required value never established: value not set.
- Required hardware connection absent: not connected.
- Operation conflicts with park or slave state: the specific state error.
- Valid request impossible in current state with no more specific reserved condition: invalid operation.
- Vendor communication failure: actionable driver-specific error unless the Master Interface specifies another mapping.

Do not reduce every failure to `NotConnected`, `InvalidOperation`, or generic driver error. Do not expose only a raw SDK number.

## Messages

The primary message should name the ASCOM operation, state the immediate reason, and suggest the next operator action when clear. Preserve deeper detail in logs or optional diagnostics; clients must not need to parse an inner exception to learn the main failure.

