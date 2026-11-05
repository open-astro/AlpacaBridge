# Outbound clients in vendor wrappers

Status: accepted

## Context

AGENTS.md kept AlpacaCore free of "HTTP/REST/sockets/JSON transport code" so that the device layer could never grow a second server, a second router or a second configuration surface beside AlpacaHTTP. Two things strained the wording. The WeeWX driver fetches a weather feed over HTTP from inside its vendor directory, and [decision 0007](0007-lan-surface-threat-model.md) item NS-10 tracks the hardening that fetch still needs. Issue #1753 asks for a `remotealpaca` vendor: an Alpaca client that connects to an upstream Alpaca server and serves its devices through AlpacaBridge as proxied devices, the first one being the ASCOM Alpaca Simulators. Both are outbound clients inside a vendor wrapper. Neither listens, routes or serves anything. The rule was protecting against the server, not against a vendor whose wire happens to be HTTP.

## Decision

- **AlpacaCore runs no server.** It listens on no socket, owns no router, serves no page and parses no inbound request. That is the whole of the old guardrail, stated as what it protects.
- **An outbound client inside a vendor wrapper is that vendor's wire protocol**, the same as a serial port or a USB SDK. It lives in `src/vendors/<vendor>/` and `include/alpacacore/vendor/<vendor>/`, is reached through a seam under [decision 0015](0015-sdk-seams.md), and is subject to [decision 0007](0007-lan-surface-threat-model.md) as an outbound surface: bounded response body, `http` and `https` only on the request and every redirect, a per-call timeout.
- **One HTTP client seam.** `util::HttpClient` is the one interface an AlpacaCore vendor uses for HTTP: a real libcurl implementation, a scripted fake and a locked decorator, per 0015. WeeWX moves onto it; the `remotealpaca` client is built on it. No vendor calls libcurl directly.
- **A proxied device never points at this server.** An upstream whose host and port resolve to one of this server's own listening addresses is refused when configured and again at Connect if the resolution changed. The error names the loop. AlpacaCore has no view of the server's bind addresses, so AlpacaHTTP passes them to the vendor factory when it builds a proxied device; a wildcard bind (`0.0.0.0` or `::`) means every local interface address and every loopback address counts as self.
- **Proxied devices obey the other records as native devices do.** Their getters are served from a polled snapshot of the upstream ([decision 0010](0010-snapshot-served-state.md)) so they never wait on the upstream ([decision 0016](0016-getters-never-wait-on-device-io.md)); their link follows [decision 0009](0009-link-loss-and-relink-policy.md) with `util::PolledLinkHealth`, a refused connection being a loss at once; commands run in a `util::AsyncOperation`. The vendor wrapper parses the upstream's JSON replies with a vendor-local parser inside the wrapper; no JSON type crosses the wrapper boundary into a driver interface, config, the catalog or device state, which is the bound [decision 0013](0013-designs-to-avoid.md) section 4 draws. Upstream `ErrorNumber` and `ErrorMessage` pass through unchanged, prefixed with the upstream host; a transport failure maps to `NotConnected` when the link is lost and `DriverException` otherwise. The management `UniqueID` is this server's own, never the upstream's; `DeviceState` is the upstream's with this server's link-health fields added.
- **Slices for #1753.** The `util::HttpClient` seam and the WeeWX migration (closes 0007 NS-10); the remote client library with a loopback fake server; `remotealpaca` ObservingConditions behind `ALPACACORE_ENABLE_REMOTEALPACA`; SafetyMonitor, where a lost link never reads safe; then one PR per device type. Upstream discovery over UDP is a later slice, not part of these.

## Rules

The rules a change is checked against.

1. AlpacaCore has no server: no listening socket, no router, no inbound request parsing. Scope: `AlpacaCore/src/**`, `AlpacaCore/include/**`. Checked by: review (a `listen`, `bind` or `accept` call, or an HTTP request parser, under AlpacaCore; `scripts/check_layering.py` checks includes and CMake only and does not cover this).
2. An AlpacaCore vendor reaches HTTP only through `util::HttpClient`; no vendor file names libcurl directly once the seam has landed. Scope: `AlpacaCore/src/vendors/**`. Checked by: review (`git grep -l curl_easy AlpacaCore/src/vendors` names a vendor file).
3. An outbound client bounds its response body, allows only `http` and `https` on the request and every redirect, and times out every call. Scope: `AlpacaCore/src/vendors/**`, `AlpacaCore/src/core/util/*.cpp`. Checked by: review (an unbounded write callback, a missing protocol allow-list, a call with no timeout), and the seam's unit tests once it lands.
4. A proxied device refuses an upstream that resolves to this server's own listening address, at configure time and at Connect. Scope: `AlpacaCore/src/vendors/**`. Checked by: review (a `remotealpaca` factory or connect path with no self-address check), and the vendor's unit tests once it lands.
5. A proxied device's `UniqueID` is this server's own and its upstream error numbers pass through unchanged. Scope: `AlpacaCore/src/vendors/**`. Checked by: review (a `remotealpaca` driver that returns the upstream's id or re-maps its error numbers), and the vendor's unit tests once it lands.

## Alternatives rejected

- The client in AlpacaHTTP behind an interface handed down to the vendor: keeps the old wording but inverts the call flow `AlpacaHTTP -> AlpacaCore driver -> vendor`, and every vendor test would need an AlpacaHTTP fake.
- A separate client library target shared with AlpacaHTTP's software-update check: a third layer for one seam; the update check stays on its own curl call under 0007.
- The vendor id `alpaca`: collides with the product's own name in config, logs and the web UI.
- Dropping the guardrail altogether: the server-side half still holds and is what the rule was for.

## Consequences

- WeeWX stops being a tension with the architecture rule and becomes the precedent; its NS-10 hardening lands with the seam migration.
- The `remotealpaca` vendor can start, one slice at a time, with its contract fixed here rather than in each PR.
- Every future vendor whose wire is HTTP (a weather API, a power box with a REST interface) follows the same seam and the same 0007 bounds.
- The call flow `AlpacaHTTP -> AlpacaCore driver -> vendor` keeps its meaning: AlpacaCore holds no server. The layering gate (`scripts/check_layering.py`) is unchanged; the no-server rule is review-checked.

## How to reverse

Restore the two AGENTS.md lines this record reworded, mark this record `superseded`, and move the WeeWX fetch and the `remotealpaca` client to AlpacaHTTP behind an interface the vendors receive. The 0015 seam row for `HttpClient` is then retired with them.

## Links

- Issue #1753 (the `remotealpaca` vendor); #1984 (WeeWX link loss, the first `PolledLinkHealth` HTTP adopter).
- [LAN surface threat model](0007-lan-surface-threat-model.md) item NS-10; [SDK seams](0015-sdk-seams.md); [Snapshot-served state](0010-snapshot-served-state.md); [Getters never wait on device I/O](0016-getters-never-wait-on-device-io.md); [Link loss and relink policy](0009-link-loss-and-relink-policy.md).
- `CONTEXT.md`: native device, proxied device, upstream server.
