# WiFi Management API

Stable HTTP/JSON contract for controlling the SBC's WiFi, as shipped in
AlpacaBridge 3.4.0. This is the interface the web portal's WiFi card uses and
the one client apps (e.g. OpenAstro Ara) should build against. Design
background and per-board validation live in `wifi-manager-design.md`.

All endpoints live under `/management/v1/wifi/` on the Alpaca port (default
6800) and return the standard Alpaca envelope:

```json
{ "Value": ..., "ErrorNumber": 0, "ErrorMessage": "",
  "ClientTransactionID": 0, "ServerTransactionID": 42 }
```

`ErrorNumber != 0` means the operation failed and `ErrorMessage` says why.
Requests and responses are `application/json`. The endpoints are
unauthenticated (trusted-LAN model, like all management endpoints) with one
guard: see "Cross-origin protection" below.

## Endpoints

### GET /management/v1/wifi/status

The one call a client needs for its WiFi UI state.

```json
{
  "Available": true,
  "Device": "wlan0",
  "WirelessEnabled": true,
  "State": "connected",
  "ConnectionId": "OpenAstro-AP",
  "ConnectionUuid": "639bae67-...",
  "ApActive": true,
  "Ssid": "OpenAstro-65CD",
  "FrequencyMhz": 5180,
  "SignalPercent": 62,
  "Ip4Address": "172.24.1.1",
  "Capabilities": { "Freq2GHz": true, "Freq5GHz": true, "Ap": true },
  "ScanSees5GHz": true,
  "Country": "US"
}
```

- `Available: false` means no WiFi adapter (or no NetworkManager) — hide the
  WiFi UI entirely. All other fields may be absent in that case.
- `ApActive` is the authoritative "hotspot vs client" flag. Do NOT infer it
  by comparing `ConnectionId` to a well-known name; renamed hotspot profiles
  are supported.
- `Ssid`/`FrequencyMhz`/`SignalPercent` describe the current association
  (own AP when `ApActive`, joined network otherwise); absent when idle.
- **5 GHz gating rule**: offer 5 GHz options when
  `Capabilities.Freq5GHz || ScanSees5GHz`. Some vendor drivers (ASIAIR Plus
  RK3568 `bcmdhd`) under-report capabilities while 5 GHz works.
- `State`: `connected`, `disconnected`, `connecting`, `deactivating`,
  `unavailable`.
- `Country`: ISO 3166-1 alpha-2, or `""` when never set.

### GET /management/v1/wifi/scan

Triggers a scan (best-effort; ~1.5 s) and returns visible networks, strongest
first, deduplicated by SSID, hidden SSIDs omitted:

```json
[ { "Ssid": "HomeNet", "FrequencyMhz": 5200, "SignalPercent": 71,
    "Security": "WPA2" } ]
```

`Security` is `Open`, `WPA`, `WPA2`, `WPA3`, or `WPA2/WPA3` (transition
mode). Scanning while the hotspot is active returns the cached BSS list on
most drivers — results may be stale in AP mode.

### GET /management/v1/wifi/profiles

Saved connections (client networks AND the hotspot profile):

```json
[ { "Id": "HomeNet", "Uuid": "…", "Ssid": "HomeNet",
    "Mode": "infrastructure", "Autoconnect": true, "Priority": 0,
    "Active": false } ]
```

`Mode` is `infrastructure` (client) or `ap` (hotspot). Passphrases are never
returned by any endpoint.

### PUT /management/v1/wifi/profiles

Create or update a client profile: `{ "Ssid": "HomeNet", "Passphrase":
"secret123", "Autoconnect": true, "Priority": 0 }`.

- `Passphrase` empty or omitted: open network for a NEW profile; "keep the
  existing secret" when the profile already has one. A secured profile can
  never be silently converted to open — delete and re-add instead.
- Passphrase must be 8–63 chars when present; SSID 1–32 bytes.
- Updating matches by SSID (client profiles only).

### DELETE /management/v1/wifi/profiles/{uuid}

Forget a saved network. Refuses to delete any hotspot (`ap`-mode) profile so
a device can't be stranded unreachable; hotspot config goes through `ap`.

### PUT /management/v1/wifi/connect

`{ "Uuid": "…" }` — activate a saved profile now. Returns
`{"Connecting": true}` immediately; the association proceeds asynchronously.
**The response races the network switch** — see "Connection-drop pattern".

### GET /management/v1/wifi/ap

```json
{ "Configured": true, "Ssid": "OpenAstro-65CD", "Band": "a",
  "Channel": 36, "Autoconnect": true, "Active": false,
  "Ip4Address": "172.24.1.1" }
```

`Configured: false` when no hotspot profile exists yet. The hotspot serves
DHCP on the fleet-wide subnet `172.24.1.0/24`; the portal is always at
`http://172.24.1.1:6800/` while the hotspot runs.

### PUT /management/v1/wifi/ap

`{ "Ssid": "…", "Passphrase": "…", "Band": "a"|"bg", "Channel": 36,
"Enabled": true }`

- Creates or updates the hotspot profile and immediately activates
  (`Enabled: true`) or deactivates (`Enabled: false`) it. `Enabled` also
  sets autoconnect, so an enabled hotspot returns after reboot.
- `Band`: `"a"` = 5 GHz, `"bg"` = 2.4 GHz. `Channel: 0` lets
  NetworkManager pick. Empty `Passphrase` keeps the existing secret.
- A brand-new hotspot profile requires a passphrase (8–63 chars).

### PUT /management/v1/wifi/radio

`{ "Enabled": true }` — WiFi radio on/off (NetworkManager
`WirelessEnabled`). Some images ship with the radio soft-disabled; a client
setting up WiFi for the first time should enable it here first.

### GET / PUT /management/v1/wifi/country

`{ "Alpha2": "US" }` — the wireless regulatory domain. Setting it applies
immediately (nl80211) and persists across reboots (re-applied at daemon
startup, before the hotspot autoconnects). Required before 5 GHz hotspot
operation in some regions/drivers; prompt for it during first-time setup
when `status.Country` is `""`. After changing it, legal AP channels change —
refresh any channel pickers.

## Cross-origin protection

Every request except `GET` that carries a browser `Origin` header not
matching the request's `Host` is rejected with HTTP 403. That covers the
state-changing methods these endpoints accept (`PUT`/`POST`/`DELETE`) and
anything unrecognised, which the server treats as an unknown method and the
guard refuses before the endpoint's own method check runs. This
blocks drive-by CSRF from malicious websites open on a LAN browser. It does
not affect native clients (no `Origin` header is sent — Ara over HTTP is
unaffected) or the same-origin web portal. One device endpoint takes the same
guard: `PUT`/`POST /api/v1/telescope/{n}/utcdate`, because on an NTP-less host
a UTCDate write steps the system clock (see the Clock section above). Every
other device setter is unguarded.

Since issue #348 this is not specific to the WiFi endpoints: every
state-changing management endpoint carries the same guard — `synctime`,
`restart`, `shutdown`, `configuredevice`, `removedevice`, `loglevel`, the
`description` PUT and `DELETE /management/v1/logfiles/<name>`. The rejection
message names the endpoint, and the 403 body echoes the `ClientTransactionID`
the request sent.

The guard compares the request's `Origin` against the request's own `Host`,
which stops a page served from an attacker-controlled origin. It does not
stop DNS rebinding, where the attacker's hostname resolves to the device and
both headers agree; see issue #392.

## Connection-drop pattern (important for clients)

Any operation that changes what the radio is doing (`connect`, `ap` with
`Enabled`, joining after a profile save) can drop the very link the client
is using — e.g. a phone on the hotspot telling the device to join home WiFi.

The robust client flow:

1. Warn the user which network the device is moving to.
2. Fire the request; treat a timeout/socket error after acceptance as
   expected, not as failure.
3. Re-locate the device on the new network: Alpaca UDP discovery
   (port 32227), mDNS hostname, or `172.24.1.1` when the hotspot is/returns
   active.
4. Poll `status` until it reflects the new state.

Wired ethernet connections are never affected by any of these operations.

## Related: clock sync API

Not WiFi, but the companion field-setup call (shipped 3.3.0) that clients
like Ara typically use in the same onboarding flow — an internet-less SBC
has no NTP, so the client's clock becomes the time source:

- `GET /management/v1/synctime` — `Value` = the server's current Unix epoch
  (seconds, UTC). Poll it to show a live server clock or detect drift
  against the client's clock.
- `POST/PUT /management/v1/synctime` with `{"Epoch": 1786298276}` (or
  `{"Value": …}`) — sets the SBC's system clock. Rejected outside the
  sanity range 2000-01-01..2100-01-01 UTC. Send the client's epoch captured
  at request time; add half the observed round-trip if you want to be exact.
  Since 3.6.0 this also takes the cross-origin guard above: a browser-based
  client posting from a different origin gets HTTP 403 before the body is
  read. `GET` is unaffected, so a live-clock poll works from any origin.

Same Alpaca envelope, same trusted-LAN model as the WiFi endpoints.

Since 3.6.0 the manual call is rarely needed: a client's
`PUT /api/v1/telescope/{n}/utcdate` (NINA, SkySafari and PHD2 send it on
connect) steps the SBC clock itself whenever the kernel reports the clock
undisciplined (`adjtimex` `STA_UNSYNC`, the permanent state of an SBC without
NTP or RTC). It never overrides NTP/chrony/GPS, applies the same 2000-2100
window, ignores sub-second deltas, and logs every step with the delta and the
client address. `GET /management/v1/description` reports the state:
`ClockSynchronized` (kernel-disciplined), `ClockSource` (`ntp` | `client` |
`rtc` | `none`) and `SyncSystemClockFromClients`; `PUT` the last one as a boolean to
opt out (persisted as `sync_system_clock_from_clients` under `server:` in the
config file). A telescope connecting while the clock is `none` always logs a
WARN. On an `rtc` host the line is an INFO only while something can still
correct the clock: it goes back to a WARN if `SyncSystemClockFromClients` is
off, and also if a client's `UTCDate` write or a Sync Time press (`POST
/management/v1/synctime`) has already been refused (no `CAP_SYS_TIME`), because
then nothing in the service can set the clock at all
and the message says to set it from outside instead. A Sync Time press (`POST /management/v1/synctime`)
counts as a client step: `ClockSource` reads `client` afterwards.

`rtc` means the kernel loaded system time from a hardware RTC at boot: the
`/sys/class/rtc/rtc*` device whose `hctosys` reads 1, whose `since_epoch` is
after 2020-01-01 (a battery-less Raspberry Pi 5 RTC reads 2000-01-01 and does
not count). It is a statement about where the clock came from, not about how
accurate it is: nothing on an NTP-less host verifies or rewrites the RTC, so
it may still be wrong or drifting, and the telescope connect line says so.
A working RTC reads `none` whenever the kernel was not the one that loaded it:
a userspace `hwclock --hctosys` under a non-systemd init, or a kernel without
`CONFIG_RTC_HCTOSYS`. `rtc` is a new value on a published field, so an older
client that switches on `ClockSource` should treat any unknown value as
"not NTP".

## Feature detection

- Old server (pre-3.4.0): the routes return "Endpoint not found" —
  treat as feature absent.
- 3.4.0+ without a wifi adapter: `status` answers with
  `{"Available": false}` — feature present, hardware absent.
