# Alpaca HTTP and JSON Contract

## Device URL

The full device route is:

`/api/v{api-version}/{device-type}/{device-number}/{command}`

For this baseline the API version is `v1`. All path elements are case-sensitive and lowercase. `device-number` is an unsigned integer unique within a device type; another type may reuse the same number.

Do not invent aliases for standard device types or commands on the documented API surface. Extensions must not make a nonstandard route necessary for standard clients.

## Verbs and parameter location

| Operation | Verb | Parameter location |
| --- | --- | --- |
| Read-only property or query | GET | URL query string |
| Setter or command | PUT | Request body, normally `application/x-www-form-urlencoded` |

A GET must not mutate device state. Reject a wrong verb before attempting the ASCOM operation. Do not translate a wrong verb into an ASCOM `NotImplemented` result.

Use the OpenAPI definition to determine required parameters, types, ranges, and response schema. The standard transaction fields do not replace endpoint-specific validation.

## Casing and tolerant input

- Paths: exact lowercase only.
- GET query parameter names: accept arbitrary casing.
- PUT form parameter names: clients emit the canonical OpenAPI casing; devices should accept casing variations where unambiguous.
- Boolean text: accept `true` and `false` case-insensitively.
- String values: preserve any case rules defined by the relevant Master Interface.
- JSON response keys: exact canonical casing.

Follow the robustness principle:

- Do not reject a request merely because optional `ClientID` or `ClientTransactionID` is absent.
- Do not reject otherwise valid requests merely because they contain unknown extra parameters.
- Do reject missing required operation parameters, duplicate values that create ambiguity, malformed values, invalid device numbers, and invalid paths.

## Locale and numeric representation

- Use `.` as decimal separator.
- Never use thousands separators.
- Parse and serialize independently of host locale.
- Produce RFC 8259 JSON.
- Do not emit NaN or infinity as JSON numbers.
- Preserve integer versus floating-point response types specified by the endpoint.

## Transaction identifiers

`ClientID` identifies the client when supplied. `ClientTransactionID` identifies a client request and is copied into the response. `ServerTransactionID` is an unsigned 32-bit server-generated identifier returned with every HTTP 200 Alpaca response and should support correlation with server logs.

- Treat client-supplied IDs as untrusted input.
- Do not use a transaction ID as authentication.
- Increment or otherwise generate server IDs safely under concurrency.
- Do not accidentally reuse one mutable response object across requests.

## Canonical HTTP 200 envelope

Every HTTP 200 Device API response contains:

| Key | Type | Meaning |
| --- | --- | --- |
| `ClientTransactionID` | unsigned 32-bit integer | Request transaction ID, or the implementation's specified default when absent |
| `ServerTransactionID` | unsigned 32-bit integer | Server transaction ID |
| `ErrorNumber` | signed 32-bit integer | Zero on success; Alpaca error number on ASCOM failure |
| `ErrorMessage` | string | Empty on success; useful primary message on failure |
| `Value` | endpoint-defined | Present only when the operation returns a value |

Use exactly these names and types. Additional endpoint-defined top-level data, such as ImageBytes metadata, follows its own schema.

Success example:

```json
{
  "Value": true,
  "ClientTransactionID": 20,
  "ServerTransactionID": 168,
  "ErrorNumber": 0,
  "ErrorMessage": ""
}
```

ASCOM failure example:

```json
{
  "ClientTransactionID": 23,
  "ServerTransactionID": 55,
  "ErrorNumber": 1025,
  "ErrorMessage": "SiteElevation -400 is below the permitted minimum"
}
```

## HTTP status decision

The HTTP status describes whether the transaction layer understood and attempted the ASCOM operation. It does not by itself mean the device operation succeeded.

| Status | Use | Body |
| --- | --- | --- |
| 200 | Valid, understood request; ASCOM operation was attempted | Alpaca JSON envelope, even when `ErrorNumber` is nonzero |
| 3xx/4xx | Redirected or rejected before ASCOM execution | Protocol-level response; V12 describes 400 as text rather than the normal Alpaca envelope |
| 400 | Invalid URL, method, device number, missing/invalid required parameter, rejected access, or otherwise uninterpretable request | Text error message under the V12 decision model |
| 500 | Catastrophic internal failure prevented normal transaction processing | Text diagnostic; do not use for ordinary device errors |

Decision sequence:

1. Validate URL and HTTP method.
2. Validate device type/number and required parameters.
3. Apply access control and protocol-level rejection.
4. Attempt the ASCOM operation.
5. Return 200 with success or ASCOM error envelope.
6. Use 500 only when normal processing itself failed catastrophically.

If a hosting project intentionally uses a JSON body for 400/500, document the deviation and verify client/ConformU compatibility; do not confuse that extension with the V12 wire requirement.

## Headers

For JSON responses use `Content-Type: application/json` or `application/json; charset=utf-8`. Clients may send `Accept: application/json` and a descriptive `User-Agent`; devices may send `Server`.

Camera ImageBytes content negotiation is separate: the client requests `Accept: application/imagebytes`, and a supporting device responds with `Content-Type: application/imagebytes`. Read `imagebytes.md` before implementing it.

## API version versus InterfaceVersion

- URL API version governs the HTTP presentation contract.
- Device `InterfaceVersion` governs behavior and available members for one ASCOM device type.
- They change independently. A new device-interface version does not automatically require `/api/v2`.
- `/management/apiversions` advertises supported Alpaca API versions; it is intentionally unversioned.

## Security boundary

The Device API schema does not define authentication. Do not assume “local network” means trusted. Avoid placing credentials, tokens, raw secrets, or sensitive vendor diagnostics in client-visible errors. If a product adds authentication, keep it outside the standard response contract and document compatibility implications.

