# ISwitchV3 Interface (ASCOM Master Interfaces 1.0.16)

Source (HTML): https://ascom-standards.org/newdocs/switch.html  
Source (PDF): https://ascom-standards.org/newdocs/ascom-interfaces.pdf  

> Note: This Markdown is a **clean, offline-friendly** rendering of the ISwitchV3 section from the ASCOM Master Interfaces documentation (Release 1.0.16).  
> Formatting has been adapted for Markdown (headings/bullets/code), but the technical meaning is preserved.

---

## Overview

**Class:** `Switch`  
**Base:** `ASCOM.DeviceInterface`  

This interface can be confusing in places; the master document references the “Switch FAQ / help” section for additional explanation.

---

## Methods

### `Action(ActionName: str, ActionParameters: str) -> str`

Invoke a device-specific custom action.

- **ActionName:** a name from `SupportedActions` (case-insensitive).
- **ActionParameters:** list of required arguments or empty string if none are required.
- **Returns:** action response string (meaning defined by driver author).

May raise:
- `MethodNotImplementedException` (if no actions are supported)
- `ActionNotImplementedException` (if action name not supported)
- `NotConnectedException`
- `DriverException`

---

### `CanAsync(Id: int) -> bool`

Flag indicating whether this switch can operate asynchronously.

- **Id:** switch number `0 .. MaxSwitch-1`
- **Returns:** `True` if this switch supports async operations.

May raise:
- `InvalidValueException`
- `NotConnectedException`
- `DriverException`

---

### `CancelAsync(Id: int) -> None`

Cancels an in-progress asynchronous state-change operation.

Notes:
- After cancellation, calls to `StateChangeComplete(Id)` raise `OperationCancelledException` until a new async operation is started and completes.

May raise:
- `InvalidValueException`
- `NotConnectedException`
- `DriverException`

---

### `CanWrite(Id: int) -> bool`

Reports whether the specified switch can be written to (e.g., sensors/limit switches are often read-only).

May raise:
- `InvalidValueException`
- `NotConnectedException`
- `DriverException`

---

### `CommandBlind(Command: str, Raw: bool) -> None` (Deprecated)

Transmit an arbitrary string to the device without waiting for a response.

- **Deprecated since V3** in favor of `Action()` / `SupportedActions`.

---

### `CommandBool(Command: str, Raw: bool) -> bool` (Deprecated)

Transmit an arbitrary string and wait for a boolean response.

- **Deprecated since V3** in favor of `Action()` / `SupportedActions`.

---

### `CommandString(Command: str, Raw: bool) -> str` (Deprecated)

Transmit an arbitrary string and wait for a string response.

- **Deprecated since V3** in favor of `Action()` / `SupportedActions`.

---

### `Connect() -> None` (Non-blocking)

Asynchronously begin connecting to the device. Completion is indicated via `Connecting` becoming `False`.

---

### `Disconnect() -> None` (Non-blocking)

Asynchronously begin disconnecting from the device. Completion is indicated via `Connecting` becoming `False`.

---

### `GetSwitch(Id: int) -> bool`

Return the state of switch `Id` as a boolean.

May raise:
- `InvalidValueException`
- `InvalidOperationException` (temporary condition prevents reading; e.g., unknown state after power-up)
- `NotConnectedException`
- `DriverException`

---

### `GetSwitchName(Id: int) -> str`

Gets the “short” name of the specified switch.

May raise:
- `InvalidValueException`
- `NotConnectedException`
- `DriverException`

---

### `GetSwitchValue(Id: int) -> float`

Return the value of switch `Id` as a float.

- Expected to be between `MinSwitchValue(Id)` and `MaxSwitchValue(Id)` in steps of `SwitchStep(Id)`.

Notes:
- For boolean on/off switches, `GetSwitchValue()` must return `MinSwitchValue` when off and `MaxSwitchValue` when on.
- Some devices can set but not read a switch value; implementations may:
  - set the switch to a known state on connect, **or**
  - throw `InvalidOperationException` until the client sets it once,
  and then return the locally saved last-set state.

May raise:
- `InvalidValueException`
- `InvalidOperationException`
- `NotConnectedException`
- `DriverException`

---

### `MaxSwitchValue(Id: int) -> float`

Returns the maximum value for switch `Id`.

Notes:
- Must be greater than `MinSwitchValue(Id)`.
- For on/off switches, **must** return `1.0`.

May raise:
- `InvalidValueException`
- `NotConnectedException`
- `DriverException`

---

### `MinSwitchValue(Id: int) -> float`

Returns the minimum value for switch `Id`.

Notes:
- Must be less than `MaxSwitchValue(Id)`.
- For on/off switches, **must** return `0.0`.

May raise:
- `InvalidValueException`
- `NotConnectedException`
- `DriverException`

---

### `SetAsync(Id: int, State: bool) -> None`

Asynchronously set a switch to the specified boolean on/off state.

- Non-blocking: returns immediately after successfully starting the state change.
- While running: `StateChangeComplete(Id)` returns `False`.
- After completion: `StateChangeComplete(Id)` returns `True`.

May raise:
- `MethodNotImplementedException` (if `CanAsync(Id)` is `False`)
- `InvalidValueException`
- `NotConnectedException`
- `DriverException`

---

### `SetAsyncValue(Id: int, Value: float) -> None`

Asynchronously set a switch to the specified float value.

- Value must be between `MinSwitchValue(Id)` and `MaxSwitchValue(Id)`.

May raise:
- `MethodNotImplementedException` (if `CanWrite(Id)` is `False` **or** `CanAsync(Id)` is `False`)
- `InvalidValueException`
- `NotConnectedException`
- `DriverException`

---

### `SetSwitch(Id: int, State: bool) -> None`

Set a switch to the specified boolean on/off state.

Notes:
- `GetSwitchValue(Id)` must return:
  - `MaxSwitchValue(Id)` when `State` is `True`
  - `MinSwitchValue(Id)` when `State` is `False`

May raise:
- `MethodNotImplementedException` (if `CanWrite(Id)` is `False`)
- `InvalidValueException`
- `NotConnectedException`
- `DriverException`

---

### `SetSwitchName(Id: int, Name: str) -> None`

Set a switch’s name.

May raise:
- `MethodNotImplementedException` (if the name cannot be set by the client)
- `InvalidValueException`
- `NotConnectedException`
- `DriverException`

---

### `SetSwitchValue(Id: int, Value: float) -> None`

Set a switch’s value to a float between `MinSwitchValue(Id)` and `MaxSwitchValue(Id)`.

May raise:
- `MethodNotImplementedException` (if `CanWrite(Id)` is `False`)
- `InvalidValueException`
- `NotConnectedException`
- `DriverException`

---

### `SetupDialog() -> None`

Launches a configuration dialog for the driver.

- **COM-only:** Alpaca devices should provide configuration via Alpaca HTML endpoints and **should not** implement a SetupDialog endpoint.

---

### `StateChangeComplete(Id: int) -> bool`

Completion property for async operations.

- Returns `True` if the last `SetAsync()` / `SetAsyncValue()` has completed and the switch is in the requested state.

May raise:
- `MethodNotImplementedException` (if `CanAsync(Id)` is `False`)
- `OperationCancelledException` (if cancelled by `CancelAsync(Id)`)
- `InvalidValueException`
- `NotConnectedException`
- `DriverException`

---

### `SwitchStep(Id: int) -> float`

Step size this switch supports (difference between successive values).

Important:
- Must be greater than zero.
- Number of steps can be computed as:

```text
((MaxSwitchValue - MinSwitchValue) / SwitchStep) + 1
```

May raise:
- `InvalidValueException`
- `NotConnectedException`
- `DriverException`

---

## Properties

### `Connected: bool` (Read/Write, write deprecated)

- Writing `Connected` to connect/disconnect is **deprecated** as of Switch V3.
- Use `Connect()` / `Disconnect()` (non-blocking), and use `Connecting` as the completion property.

---

### `Connecting: bool` (Read-only)

- Indicates whether an async connect/disconnect is in progress.
- Completion is when `Connecting` becomes `False` after calling `Connect()` or `Disconnect()`.

---

### `Description: str` (Read-only)

Short description of the device/driver (human-friendly).

---

### `DriverInfo: str` (Read-only)

May be long (hundreds to thousands of characters), intended to display detailed info (version/copyright/etc).

---

### `DriverVersion: str` (Read-only)

String containing only the major/minor version in `n.n` form.

---

### `InterfaceVersion: int` (Read-only)

The version of the **ISwitch** specification supported by the driver.

---

### `Name: str` (Read-only)

Short, human-friendly name of the driver.

---

### `SupportedActions: string[]` (Read-only)

List of custom action names supported by this driver.

Notes:
- Used as a discovery mechanism so clients can know supported actions without invoking them.

---

## See also

- Switch interface HTML: https://ascom-standards.org/newdocs/switch.html
- ASCOM Master Interfaces PDF: https://ascom-standards.org/newdocs/ascom-interfaces.pdf
