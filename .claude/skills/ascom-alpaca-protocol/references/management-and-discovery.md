# Management, Setup, Discovery, and Identity

## Surfaces

An Alpaca host normally exposes:

- Device API: `/api/v1/{device-type}/{device-number}/{command}`
- Main browser setup page: `/setup`
- Device setup page: `/setup/v1/{device-type}/{device-number}/setup`
- Supported API versions: `/management/apiversions`
- Host description: `/management/v1/description`
- Configured devices: `/management/v1/configureddevices`
- UDP discovery responder

Setup pages are browser-facing configuration surfaces and do not replace the JSON Management API.

## API versions

`GET /management/apiversions` has no version segment so it remains usable when later API versions exist. Under this baseline it returns an integer array containing `1`. A host that supports multiple API versions returns each supported version.

Do not confuse this list with per-device `InterfaceVersion`.

## Description

`GET /management/v1/description` describes the Alpaca host as a whole, not one logical ASCOM Device. Follow the Management OpenAPI schema exactly. Keep server name, manufacturer, version, and location information truthful and stable enough for client presentation.

## Configured devices

`GET /management/v1/configureddevices` returns one entry per exposed ASCOM Device. Each entry identifies at least:

- device name;
- canonical ASCOM device type;
- device number used in Device API routes; and
- globally unique `UniqueID`.

The returned inventory must agree with registered Device API routes. Do not advertise unavailable devices or omit a route that the list promises.

## UniqueID requirements

Each logical ASCOM Device has its own persistent ASCII UID.

- Derive it from a space of at least 48 bits.
- Identical hardware units must still receive different UIDs.
- Never reuse a UID for another device or type.
- Return the same UID on every network interface.
- Retain it across restarts, power cycles, address changes, and ordinary configuration edits.
- Avoid deriving it solely from mutable display names, device numbers, IP addresses, or USB enumeration order.

Clients use the UID to rediscover a known device after its network address changes.

## Discovery v1

The default UDP discovery port is **32227**. It must be configurable for unusual deployments but should normally work without user adjustment.

The v1 discovery request is exactly 16 ASCII bytes:

`alpacadiscovery1`

A valid response is a JSON object sent by unicast to the requesting client:

```json
{"AlpacaPort": 6800}
```

`AlpacaPort` is the TCP port where Management and Device APIs are available. Use the source IP of the response plus this port; do not place an IP address inside the response.

## IPv4

Clients broadcast the discovery message to UDP port 32227. Devices listen for broadcasts, validate the message, and unicast the response. IPv4 support is recommended for broad compatibility.

## IPv6

IPv6 discovery uses link-local multicast address `ff12::a1:9aca` on the discovery port. A supporting device joins that multicast group, validates messages, and unicasts its response. IPv6 is optional under V12.

## Port sharing

Multiple Alpaca processes on one host must be able to share the discovery port. Configure the platform-equivalent reuse options:

- Windows: `SO_REUSEADDR`
- Linux/macOS: `SO_REUSEADDR` and `SO_REUSEPORT`

Do not require root privileges merely to participate in discovery.

## Discovery flow

1. Client broadcasts or multicasts the v1 discovery message.
2. Device validates it and unicasts `AlpacaPort`.
3. Client queries `/management/apiversions` and `/management/v1/configureddevices`.
4. Client identifies logical devices and their UIDs.
5. Client invokes Device API routes at the responding address and advertised port.

Test discovery on hosts with multiple interfaces, multiple responders, shared port users, and changing IP addresses. Deduplicate logical devices by persistent UID, not by transient address alone.

