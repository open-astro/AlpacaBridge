# LAN surface threat model

Status: accepted

## Context

AlpacaBridge listens on every interface (`AlpacaHTTP/src/http/server.cpp:573`, listener bound to `INADDR_ANY`, port 6800 by default) and answers UDP discovery probes on 32227 (`AlpacaHTTP/src/discovery/discovery.cpp`). The host is a single-board computer on an observatory LAN, and often is the LAN: the Wi-Fi manager turns the board into a hotspot, sometimes an open one. ASCOM Alpaca has no authentication and no transport security, so a client that can reach the port can already slew the mount, take frames and change the site. We do not add authentication the protocol does not have.

The audit findings NS-01 to NS-14 read as unrelated defects. Against the code they are one question asked five times: who may reach this surface, what can they do, what is trusted, and what does a hostile or careless caller cost. This record answers it once per surface, states the trust limit, fixes the bounds each surface must keep and maps every finding to where it is handled. Every audit claim was re-read against current main.

Five surfaces: (1) Device API `/api/v1/<type>/<n>/...`; (2) Management API `/management/v1/...`; (3) Web UI `AlpacaHTTP/web/`; (4) Wi-Fi API `/management/v1/wifi/*` over NetworkManager and `debian/alpacabridge.polkit-rules`; (5) Discovery responder, UDP 32227.

## Decision

### Trust statement

- **A LAN host is trusted to operate and configure the server.** Alpaca has no login, so a peer that can open a TCP connection may drive devices, register or remove them, set the log level and the clock, join or leave a Wi-Fi network, stop or restart the service, and start a software update. The limit of that trust is the process: a LAN host is not trusted to make the server unresponsive for other clients (held sockets, oversized input, unbounded work) and cannot reach anything beyond the service privileges.
- **A browser page is not trusted.** A page the operator opens elsewhere can send requests to the board from the operator browser. It is held by `reject_cross_origin_request` (`AlpacaHTTP/src/http/router.cpp:7487`), an opt-in Host allowlist against DNS rebinding, off by default (`http.host_check_enabled`; `reject_disallowed_host`, `router.cpp:1487`, called from `Router::route()` at `1687`; NS-02, issue #392) and framing headers against clickjacking (NS-08, not landed).
- **A device in radio range is not trusted.** On an open hotspot anyone in range is on the LAN. The hotspot must carry a passphrase by default (NS-06); until then radio range equals LAN trust.
- **Localhost gets no extra trust**: a local browser is still a browser.

The guard compares `Origin` with `Host` as strings. It passes every GET and every request with no `Origin` header (curl, NINA, ConformU). That is intended: a browser always sends `Origin` on a cross-origin POST or PUT, and a non-browser client is a LAN host and trusted anyway. It is an origin check, not authentication, and on its own DNS rebinding defeats it, because the rebound page is same-origin under the attacker name; the Host allowlist (`reject_disallowed_host`, applied in `Router::route()` to every method and path before routing when `http.host_check_enabled` is on; `server.cpp:56` passes it `http.allowed_hosts`) refuses that request first. The allowlist is off by default (see Guard coverage). The operator can change both settings from the web UI: `PUT /management/v1/description` (`HostCheckEnabled`, `AllowedHosts`) writes them live and to the `http:` section of the config file. That write is itself an HTTP write to a security control, so it is held by `reject_cross_origin_request`, by the Host check itself when it is on, and by a self-lockout rule: a request that turns the check on or edits the list is refused with 400 when its own `Host` would be refused by the new settings, and nothing changes. A value fixed by `ALPACAHTTP_HOST_CHECK` or `ALPACAHTTP_ALLOWED_HOSTS` cannot be changed over HTTP. With the check off, a rebinding page that passes no cross-origin guard can still turn it off or edit the list, which is the same exposure as the rest of the unguarded surface while the check is off.

### Surface 1: Device API

- **Who can reach it.** Any LAN host; a browser page only through the CORS rules below.
- **What it can do.** Read every property, move hardware (slew, `MoveAxis`, pulse guide, focuser and rotator moves, dome and cover commands), start and abort exposures, set switches (power ports), call `Action` and `CommandBlind`/`CommandBool`/`CommandString` where a driver implements them, read image data.
- **What is trusted and why.** The caller is trusted to operate the device. Parameters are not: each driver validates its ranges and throws `InvalidValue`. The device number is bounded at parse time (`parse_route`, `router.cpp:1746`).
- **Guard coverage.** The verb check (`router.cpp:2415-2438`, guard call at `2429`) calls the origin guard only for a request whose verb the method does not accept, so a forged POST gets 403. A well-formed PUT that reaches a driver is not passed through the guard; only `UTCDate` (`router.cpp:3729`) and the `Site*` PUTs (`3936`, `3957`, `3978`) are. A browser cannot deliver such a PUT cross-origin: PUT needs a CORS preflight and the server never answers one (no `Access-Control-*` header and no OPTIONS handling anywhere under `AlpacaHTTP/`). That protection is absent-by-omission. The Host allowlist (NS-02, issue #392) closes the DNS-rebinding path to it when it is enabled, because a rebound page is refused with 403 before any routing. It is opt-in and off by default (`http.host_check_enabled`, so a router name such as `astropi.lan` is not locked out after an upgrade), so by default the DNS-rebinding path to the unguarded device PUTs is open. The gap is listed under Known gaps.
- **Resource budget.** Header block at most 64 KiB (`kMaxHeaderBytes`, `server.cpp:861`). Body at most 64 KiB (`Request::kMaxBodyBytes`, `AlpacaHTTP/include/alpacahttp/request.h:38`, issue #741): a larger Content-Length is answered `413` from the headers alone, before any body byte is read (`server.cpp:1059-1060`), and refused by `Request::parse()` (`AlpacaHTTP/src/core/request.cpp:127`). URL path at most 2048 bytes (`kMaxRequestPathBytes`, `router.cpp:1394`), refused before any regex (`router.cpp:1666`, issue #711). Worker pool 32 threads and 512 open connections (`AlpacaHTTP/include/alpacahttp/config.h:147`, `154`), 1000 requests per connection (`server.cpp:844`), a total-request deadline, a per-recv timeout and an idle keep-alive deadline (`server.cpp:804`, `960-963`, `1254`). There is no per-host cap, so one host can hold every slot (NS-03+05).

### Surface 2: Management API

- **Who can reach it.** Any LAN host; a browser page only for what the guard does not cover.
- **What it can do.** Read and change persisted device configuration, remove devices, change the server description and the `SyncSystemClockFromClients` opt-out, read, download and delete log files, set the log level, set the system clock (`synctime`, needs `CAP_SYS_TIME`), stop (`shutdown`) and restart the service, read server values and the device catalog, and start a software update (`/management/v1/update/status`, `check`, `install`; routed at `router.cpp:1846`, `handle_software_update` at `7783`). `check` makes the board fetch the configured `Packages` index; `install` makes systemd start the root oneshot unit `alpacabridge-update.service`, which runs `apt-get update` and `apt-get install --only-upgrade alpacabridge` against the host's own signed apt sources (`debian/alpacabridge-update.service`, `debian/alpacabridge-software-update`, `docs/software-update.md`). The daemon passes no arguments, and the polkit rule lets the service user start that one unit and nothing else (`debian/alpacabridge-update.polkit-rules`), so a LAN host can make the board upgrade AlpacaBridge as root but cannot choose the package, the source or the command.
- **What is trusted and why.** The configuring host is trusted to choose which device to talk to (source of R1). Field values are not: they pass `sanitize_device_config` (strict allowlist) and typed reads through `config_has()`/`config_get()`. Log file names are matched against the daily-file pattern, so a path cannot leave the log directory.
- **Guard coverage.** Every state-changing handler calls the guard: server description PUT (`router.cpp:2055`, which also guards the clock-sync opt-out), `configuredevice` (`router.cpp:6732`), removal (`6847`), log level (`6989`), log files (`7180`, `7334`), shutdown (`7425`), synctime (`7554`), Wi-Fi (`7668`), software update (`7800`, the `check` and `install` POSTs; the guard passes every GET, so `status` is unguarded), restart (`7856`). New handlers must do the same.
- **Resource budget.** Surface 1 limits apply; log reads are capped at 10 MiB (`util::read_log_file`). `shutdown` (`router.cpp:7408`, detach at `7454`) and `restart` (`7838`, detach at `7881`) detach one thread per accepted request with no cap on how many live at once. Fix shape: a once-flag, so a second shutdown or restart while one is pending answers without spawning a thread (tracked with the NS-14 low-severity items).

### Surface 3: Web UI

- **Who can reach it.** Any LAN host loads the static files (`handle_static_file`, `router.cpp:6432`); the operator browser runs them.
- **What it can do.** Everything the APIs can, with the operator reach. It renders server-supplied strings (device names, descriptions, `LastConnectError`, log lines, Wi-Fi SSIDs from a scan).
- **What is trusted and why.** Nothing from the network is trusted as markup: an SSID and a driver error are chosen by third parties. `AlpacaHTTP/web/app.js` has 28 `innerHTML`-class sites (text search count); none has been audited for an unescaped server- or network-supplied value. This record does not claim the UI is free of script injection; the audit of these sites is tracked with the NS-14 low-severity items (web UI item).
- **Framing.** No `X-Frame-Options`, `frame-ancestors` or CSP is sent, so an attacker page can frame the UI (NS-08).

### Surface 4: Wi-Fi API

- **Who can reach it.** Any LAN host, including one that just joined the hotspot; a page only past the guard (`router.cpp:7668`).
- **What it can do.** Scan, list, join, forget and edit stored profiles, switch the hotspot and edit its SSID, passphrase, band and channel, set the regulatory country. The service user holds six NetworkManager polkit actions (`wifi.scan`, `enable-disable-wifi`, `network-control`, `settings.modify.system`, `wifi.share.protected`, `wifi.share.open`) and two ambient capabilities (`CAP_NET_ADMIN`, `CAP_SYS_TIME`; `debian/alpacabridge.service`). Over the LAN that can disconnect the operator or change the hotspot passphrase, which is the function of the card and why NS-06 is the radio-range control.
- **What is trusted and why.** The caller is trusted to reconfigure networking. Strings are not: an SSID or passphrase reaches NetworkManager only as a typed D-Bus value, never a shell. `handle_wifi` (`router.cpp:7654`) passes them to `WifiManager::save_profile` and `set_ap`, which refuse an SSID outside 1..32 bytes and a non-empty passphrase outside 8..63 before any D-Bus call (`AlpacaHTTP/src/util/wifi_manager.cpp:1071-1072`, `1193-1194`).
- **Resource budget.** One `sd_bus` connection serialized on one mutex (see the wifi-manager instructions). A scan holds it for a fixed 1.5 s `nanosleep` after `RequestScan` (`wifi_manager.cpp:1000-1009`); other Wi-Fi calls queue on it and each holds one of the shared 32 workers.

### Surface 5: Discovery responder

- **Who can reach it.** Any host that can send UDP to port 32227, including the multicast group.
- **What it can do.** Learn that an Alpaca server exists and its HTTP port. The reply is a fixed JSON object with the `AlpacaPort` key (`Discovery::handle_probe`, `AlpacaHTTP/src/discovery/discovery.cpp:149-181`). At the default port 6800 it is 19 bytes against a 16-byte minimum matching probe (`alpacadiscovery1`); this small amplification is not a useful reflector and needs no rate limit.
- **What is trusted and why.** The sender address is spoofable and the reply goes to it; with a reply this small that is accepted.
- **Logging.** `handle_probe` logs every probe at INFO (`discovery.cpp:151`) and every non-Alpaca datagram at DEBUG (`discovery.cpp:179`, issue #740), so a host sending garbage in a loop adds no WARNING line. A valid probe still logs one INFO line per probe.
- **Resource budget.** One socket, one thread, one small `sendto` per probe; no memory grows with the count.

### Accepted residual risks

Accepted by the board on 2026-09-30; they bind as part of this record.

- **R1. A device config accepts any `portPath`, `host`, `tcpPort` or `hidPath`, so `LastConnectError` is a LAN reachability oracle.** A configuring host can register a device pointing at any host and port, connect it, and read the driver refusal (`configureddevices`, `LastConnectError`, `router.cpp:2295`). Accepted because a LAN host can already probe the LAN directly. Kept: no driver puts a credential in a connect error.
- **R2. Duplicate query and form parameters resolve by position and the two parsers differ.** The query parser keeps the last value (`AlpacaHTTP/src/core/request.cpp:151-191`); the form body keeps the first (`get_form_value`, `router.cpp:875`). Accepted because both are deterministic, no security check depends on a duplicate, and the caller is a trusted LAN host; it would matter only behind a proxy or filter that decides on the other occurrence, which is unsupported.
- **R3. The udev rules install `MODE=0666` on whole vendor IDs** (`AlpacaCore/external/**/*.rules`), so any local user can open the device. Accepted because the board is a single-purpose appliance and group access would break the companion projects that dlopen the same libraries. Unacceptable on a shared machine; the README must say so.
- **R4. `GET /management/v1/wifi/scan` holds the Wi-Fi mutex for 1.5 s.** Accepted because today the 32-worker pool and the global connection limit bound it (there is no per-host cap yet; the proposed cap is NS-03+05, open), it degrades only the Wi-Fi card, and a host that reaches the board can disconnect it by other means. The scan changes no state, so it stays a GET.

### Reviewer check for any new route or persisted config field

A change passes only when every line holds, or the exception is written in the change:

- State change is never a GET (the Wi-Fi scan, R4, is the documented exception).
- The origin guard covers the route on the path that reaches the driver, not only on the wrong-verb path.
- A change that answers a CORS preflight, adds an Access-Control-* header or handles OPTIONS must pass every driver-reaching PUT through the origin guard in the same PR.
- The body and every string field have a byte bound; integers are range-checked before a narrowing cast, so nothing wraps.
- A URL field accepts only `http` and `https` and has a size cap.
  The update check URL `update_packages_url` (`server:` key in the YAML config and env `ALPACAHTTP_UPDATE_PACKAGES_URL`, not settable over HTTP; `update_release_notes_url` and `update_release_url` are the same kind) meets the scheme line at fetch time: libcurl is limited to `http,https` for the request and every redirect (`AlpacaHTTP/src/util/software_update.cpp:291-292`) and the response to 8 MiB (`300`). The URL string itself has no length cap; only the operator writes it.
- Paths and GPIO chip and line fields are validated or behind an explicit opt-in.
- A hardware-safety override is opt-in and off by default.
- No credential in a client-facing error, including `LastConnectError`.
- A failure a client can trigger is not logged at WARNING or above.
- Memory per request is bounded by a stated constant.
- JSON output survives non-UTF-8 input (replace handler, or reject at parse).

### Known gaps (open)

- No framing or CSP headers (NS-08).
- No per-host connection cap (NS-03+05).
- Driver-reaching device PUTs are not origin-guarded; safe while no CORS preflight is answered. The DNS-rebinding path to them is open by default: the Host allowlist (NS-02) closes it only when `http.host_check_enabled` is on. The reviewer check holds the rest.
- Unbounded detached threads per shutdown and restart request; tracked with the NS-14 low-severity items.
- 28 unaudited `innerHTML`-class sinks in `app.js`; tracked with the NS-14 items (web UI item).

### Finding map

| Finding | Surface | Status | Where handled |
| --- | --- | --- | --- |
| NS-01 | Management API | fixed | landed on main |
| NS-02 | Device API, Management API, Web UI | opt-in, off by default | Host allowlist against DNS rebinding (issue #392), applied only when `http.host_check_enabled` / `ALPACAHTTP_HOST_CHECK` is true: `reject_disallowed_host` in `Router::route()`, extra names from `http.allowed_hosts` / `ALPACAHTTP_ALLOWED_HOSTS`, editable over `PUT /management/v1/description` (`AlpacaHTTP/src/core/config.cpp`, `AlpacaHTTP/include/alpacahttp/config.h:83`); closes the rebinding half of the unguarded device PUT gap when enabled; with the default (off) that half is open |
| NS-03+05 | all HTTP surfaces | open | per-host connection cap |
| NS-04 | all HTTP surfaces | fixed | request body cap lowered from 10 MiB to 64 KiB (issue #741) |
| NS-06 | Wi-Fi API | open | hotspot passphrase by default |
| NS-07 | Device API, Management API | open | JSON output throws on non-UTF-8 input (`json.dump()` with no replace handler, `AlpacaHTTP/src/core/response.cpp:75`) |
| NS-08 | Web UI | open | framing headers and CSP |
| NS-09 | Device API | open | the iOptron telescope lowers the mount altitude limit to -89 degrees when a slew is refused, with no opt-in (`AlpacaCore/src/vendors/ioptron/ioptron_telescope_driver.cpp:1872`) |
| NS-10 | Management API | open | the WeeWX feed URL has no scheme allowlist, follows redirects and reads an unbounded body (`AlpacaCore/src/vendors/weewx/weewx_observingconditions_driver.cpp:257-261`) |
| NS-11 | Management API | open | the `gpioChip` config path is checked only for a `/dev/` prefix before `gpiod_chip_open` |
| NS-12 | Device API | open | an image array request holds the whole frame (`get_image_array`, `AlpacaCore/include/alpacacore/camera_driver.h:154`); memory per request has no stated constant |
| NS-13 | Discovery, Management API | fixed | the discovery non-Alpaca datagram and the unmatched setup path log at DEBUG, the path cut to 256 bytes (issue #740) |
| NS-14 | several | open | split into separate low-severity changes: HTTP framing from parsed headers; strict URL decoding; config clamps and join-on-every-path for discovery, shutdown and restart; input length and count bounds; PulseGuide range; clock-step log pruning; atomic config writes; streamed log download. Four NS-14 items are the accepted risks R1 to R4. |

When a finding lands, set its row to `fixed` in the same change.

## Alternatives rejected

- **Add authentication.** Alpaca defines none; NINA, ConformU and every client would stop working. A shared secret over plain HTTP on an open hotspot protects nothing against the radio-range attacker anyway.
- **Bind to localhost or one interface.** The server is used from another machine by design and the LAN address changes with the hotspot.
- **Require a preflight or custom header on every state change.** It breaks non-browser clients that send no `Origin`.
- **Treat the Origin check as the whole browser defence.** It compares two attacker-influenced strings under DNS rebinding; it needs the Host allowlist beside it (enable `http.host_check_enabled`; it is off by default).
- **Fix R1 to R4 now.** R1, R2 and R4 need policy the LAN model lacks (a reachable-host allowlist, a proxy-aware parser); R3 is the price of the shared libraries.

## Consequences

- Reviewers get one list for any new route or config field. The rule stays at the instruction owners; this record keeps the reason.
- Gaps with a filed upstream issue name it in the finding map (NS-02 issue #392, NS-04 issue #741 and NS-13 issue #740, all three landed). The other open rows have a proposed fix and no owner or issue yet; this record does not promise one. The device PUT guard rests on the Host allowlist and the reviewer check; the detached threads and the `innerHTML` audit go with the NS-14 low-severity items.
- Accepted by the board on 2026-09-30 (R1 to R4 accepted as residual risk).
- NS-07 and NS-09 to NS-14 are not decided here; each carrier decides its fix and must pass the reviewer check.
- ConformU and other non-browser clients are unaffected by every control named here.

## Links

- Guard: `AlpacaHTTP/src/http/router.cpp` (`reject_cross_origin_request`); limits: `AlpacaHTTP/src/http/server.cpp`, `AlpacaHTTP/include/alpacahttp/config.h`, `AlpacaHTTP/include/alpacahttp/request.h`.
- Discovery: `AlpacaHTTP/src/discovery/discovery.cpp`. Wi-Fi: `debian/alpacabridge.polkit-rules`, `debian/alpacabridge.service`, `docs/wifi-manager-design.md`.
- Owners of the reviewer check: [AlpacaHTTP conformance](../../.github/instructions/alpaca-http-conformance.instructions.md), [WiFi manager](../../.github/instructions/wifi-manager.instructions.md).
- Related: [Server thread ownership](0002-server-thread-ownership.md), [Device catalog](0004-device-catalog.md).
- Upstream issues [#392](https://github.com/open-astro/AlpacaBridge/issues/392), [#711](https://github.com/open-astro/AlpacaBridge/issues/711), [#713](https://github.com/open-astro/AlpacaBridge/issues/713), [#740](https://github.com/open-astro/AlpacaBridge/issues/740) and [#741](https://github.com/open-astro/AlpacaBridge/issues/741).
