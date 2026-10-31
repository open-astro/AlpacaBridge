# LAN surface threat model

Status: accepted

## Context

AlpacaBridge listens on every interface (`create_listener()` in `AlpacaHTTP/src/http/server.cpp`, listener bound to `INADDR_ANY`, port 6800 by default) and answers UDP discovery probes on port 32227 by default (`discovery.port`, configurable; `AlpacaHTTP/src/discovery/discovery.cpp`). The host is a single-board computer on an observatory LAN, and often is the LAN: the Wi-Fi manager turns the board into a hotspot, sometimes an open one. ASCOM Alpaca has no authentication and no transport security, so a client that can reach the port can already slew the mount, take frames and change the site. We do not add authentication the protocol does not have.

The audit findings NS-01 to NS-14 read as unrelated defects. Against the code they are one question asked five times: who may reach this surface, what can they do, what is trusted, and what does a hostile or careless caller cost. This record answers it once per surface, states the trust limit, fixes the bounds each surface must keep and maps every finding to where it is handled. Every audit claim was re-read against current main.

Five surfaces: (1) Device API `/api/v1/<type>/<n>/...`; (2) Management API `/management/v1/...`; (3) Web UI `AlpacaHTTP/web/`; (4) Wi-Fi API `/management/v1/wifi/*` over NetworkManager and `debian/alpacabridge.polkit-rules`; (5) Discovery responder, UDP, default port 32227, configurable.

## Decision

### Trust statement

- **A LAN host is trusted to operate and configure the server.** Alpaca has no login, so a peer that can open a TCP connection may drive devices, register or remove them, set the log level and the clock, join or leave a Wi-Fi network, stop or restart the service, and start a software update. The limit of that trust is the process: a LAN host is not trusted to make the server unresponsive for other clients (held sockets, oversized input, unbounded work) and cannot reach anything beyond the service privileges.
- **A browser page is not trusted.** A page the operator opens elsewhere can send requests to the board from the operator browser. It is held by `reject_cross_origin_request` (`AlpacaHTTP/src/http/router.cpp`), an opt-in Host allowlist against DNS rebinding, off by default (`http.host_check_enabled`; `reject_disallowed_host`, called from `Router::route()`; NS-02, issue #392) and framing headers against clickjacking (NS-08, not landed).
- **A device in radio range is not trusted.** On an open hotspot anyone in range is on the LAN. The hotspot must carry a passphrase by default (NS-06); until then radio range equals LAN trust.
- **Localhost gets no extra trust**: a local browser is still a browser.

The guard compares `Origin` with `Host` as strings. It passes every GET and every request with no `Origin` header (curl, NINA, ConformU). That is intended: a browser always sends `Origin` on a cross-origin POST or PUT, and a non-browser client is a LAN host and trusted anyway. It is an origin check, not authentication, and on its own DNS rebinding defeats it, because the rebound page is same-origin under the attacker name; the Host allowlist (`reject_disallowed_host`, applied in `Router::route()` to every method and path before routing when `http.host_check_enabled` is on; the server constructor in `server.cpp` passes it `http.allowed_hosts` (`set_allowed_hosts()`)) refuses that request first. The allowlist is off by default (see Guard coverage). The operator can change both settings from the web UI: `PUT /management/v1/description` (`HostCheckEnabled`, `AllowedHosts`) writes them live and to the `http:` section of the config file. That write is itself an HTTP write to a security control, so it is held by `reject_cross_origin_request`, by the Host check itself when it is on, and by a self-lockout rule: a request that turns the check on or edits the list is refused with 400 when its own `Host` would be refused by the new settings, and nothing changes. With the check off, a rebinding page that passes no cross-origin guard can turn it on with its own name in the list (the write persists) or lock the operator out of a custom DNS name; that is the same exposure as the rest of the unguarded surface while the check is off. The same page can also persist its own name into the allowed host names while the check is off, because the lockout rule applies only when the resulting check is on; the operator should read the list before turning the check on. A request with an absent or empty `Host` skips both the Host check and the lockout rule.

### Surface 1: Device API

- **Who can reach it.** Any LAN host; a browser page only through the CORS rules below.
- **What it can do.** Read every property, move hardware (slew, `MoveAxis`, pulse guide, focuser and rotator moves, dome and cover commands), start and abort exposures, set switches (power ports), call `Action` and `CommandBlind`/`CommandBool`/`CommandString` where a driver implements them, read image data.
- **What is trusted and why.** The caller is trusted to operate the device. Parameters are not: each driver validates its ranges and throws `InvalidValue`. The device number is bounded at parse time (`Router::parse_route()`).
- **Guard coverage.** The verb check in `Router::handle_device()` (the #574 wrong-verb branch) calls the origin guard only for a request whose verb the method does not accept, so a forged POST gets 403. A well-formed PUT that reaches a driver is not passed through the guard; only the `UTCDate` and `Site*` PUTs in `Router::dispatch_telescope_method()` are. A browser cannot deliver such a PUT cross-origin: PUT needs a CORS preflight and the server never answers one (no `Access-Control-*` header and no OPTIONS handling anywhere under `AlpacaHTTP/`). That protection is absent-by-omission. The Host allowlist (NS-02, issue #392) closes the DNS-rebinding path to it when it is enabled, because a rebound page is refused with 403 before any routing. It is opt-in and off by default (`http.host_check_enabled`, so a router name such as `astropi.lan` is not locked out after an upgrade), so by default the DNS-rebinding path to the unguarded device PUTs is open. The gap is listed under Known gaps.
- **Resource budget.** Header block at most 64 KiB (`kMaxHeaderBytes`, `server.cpp`). Body at most 64 KiB (`Request::kMaxBodyBytes`, `AlpacaHTTP/include/alpacahttp/request.h`, issue #741): a larger Content-Length is answered `413` from the headers alone, before any body byte is read (`read_request()` in `server.cpp`), and refused by `Request::parse()` (`AlpacaHTTP/src/core/request.cpp`). URL path at most 2048 bytes (`kMaxRequestPathBytes`, `router.cpp`), refused at the top of `Router::route()` before any regex (issue #711). Worker pool 32 threads and 512 open connections (`thread_pool_size_` and `max_connections_` in `AlpacaHTTP/include/alpacahttp/config.h`), 1000 requests per connection (`kMaxRequestsPerConnection`, `server.cpp`), a total-request deadline, a per-recv timeout and an idle keep-alive deadline (`kRequestDeadlineSeconds`, `kSocketTimeoutSeconds`, `kKeepAliveIdleSeconds`, `server.cpp`). There is no per-host cap, so one host can hold every slot (NS-03+05).

### Surface 2: Management API

- **Who can reach it.** Any LAN host; a browser page only for what the guard does not cover.
- **What it can do.** Read and change persisted device configuration, remove devices, change the server description and the `SyncSystemClockFromClients` opt-out, read, download and delete log files, set the log level, set the system clock (`synctime`, needs `CAP_SYS_TIME`), stop (`shutdown`) and restart the service, read server values and the device catalog, and start a software update (`/management/v1/update/status`, `check`, `install`, `channel`; routed in `Router::route()` to `Router::handle_software_update()`). `check` makes the board fetch the configured `Packages` index; `install` makes systemd start the root oneshot unit `alpacabridge-update.service`, which runs `apt-get update` and `apt-get install --only-upgrade alpacabridge` against the host's own signed apt sources (`debian/alpacabridge-update.service`, `debian/alpacabridge-software-update`, `docs/software-update.md`). The daemon passes no arguments to the update unit, and the polkit rule lets the service user start three units and nothing else (that unit and the two channel instances below) (`debian/alpacabridge-update.polkit-rules`), so a LAN host can make the board upgrade AlpacaBridge as root but cannot choose the package, the source or the command. `channel` makes systemd start one of the two instances `alpacabridge-channel@stable.service` and `alpacabridge-channel@beta.service`, which add or remove one fixed apt source for OpenAstro's own `beta` component (`debian/alpacabridge-set-update-channel`); the polkit rule names both by exact string and the daemon takes the unit from a two-entry table, so a LAN host can move the board between OpenAstro's stable and beta releases, nothing else.
- **What is trusted and why.** The configuring host is trusted to choose which device to talk to (source of R1). Field values are not: they pass `sanitize_device_config` (strict allowlist) and typed reads through `DeviceConfig::get()` and the `catalog_json` readers (`has_value()`, `get_string()`, `get_int()`). Log file names are matched against the daily-file pattern, so a path cannot leave the log directory.
- **Guard coverage.** Every state-changing handler calls the guard: server description PUT (`handle_description`, which also guards the clock-sync opt-out), `configuredevice` (`handle_configure_device`), removal (`handle_remove_device`), log level (`handle_log_level`), log files (`handle_log_files_list`, `handle_log_file_item`), shutdown (`handle_shutdown`), synctime (`handle_sync_time`), Wi-Fi (`handle_wifi`), software update (`handle_software_update`, the `check`, `install` and `channel` POSTs; the guard passes every GET, so `status` is unguarded), restart (`handle_restart`), all `Router::` members in `router.cpp`. New handlers must do the same.
- **Resource budget.** Surface 1 limits apply; log reads are capped at 10 MiB (`util::read_log_file`). `shutdown` (`Router::handle_shutdown()`) and `restart` (`Router::handle_restart()`) detach one thread per accepted request with no cap on how many live at once. Fix shape: a once-flag, so a second shutdown or restart while one is pending answers without spawning a thread (tracked with the NS-14 low-severity items).

### Surface 3: Web UI

- **Who can reach it.** Any LAN host loads the static files (`Router::handle_static_file()`); the operator browser runs them.
- **What it can do.** Everything the APIs can, with the operator reach. It renders server-supplied strings (device names, descriptions, `LastConnectError`, log lines, Wi-Fi SSIDs from a scan).
- **What is trusted and why.** Nothing from the network is trusted as markup: an SSID and a driver error are chosen by third parties. `AlpacaHTTP/web/app.js` has 28 `innerHTML`-class sites (text search count); none has been audited for an unescaped server- or network-supplied value. This record does not claim the UI is free of script injection; the audit of these sites is tracked with the NS-14 low-severity items (web UI item).
- **Framing.** No `X-Frame-Options`, `frame-ancestors` or CSP is sent, so an attacker page can frame the UI (NS-08).

### Surface 4: Wi-Fi API

- **Who can reach it.** Any LAN host, including one that just joined the hotspot; a page only past the guard (`Router::handle_wifi()`).
- **What it can do.** Scan, list, join, forget and edit stored profiles, switch the hotspot and edit its SSID, passphrase, band and channel, set the regulatory country. The service user holds six NetworkManager polkit actions (`wifi.scan`, `enable-disable-wifi`, `network-control`, `settings.modify.system`, `wifi.share.protected`, `wifi.share.open`) and two ambient capabilities (`CAP_NET_ADMIN`, `CAP_SYS_TIME`; `debian/alpacabridge.service`). Over the LAN that can disconnect the operator or change the hotspot passphrase, which is the function of the card and why NS-06 is the radio-range control.
- **What is trusted and why.** The caller is trusted to reconfigure networking. Strings are not: an SSID or passphrase reaches NetworkManager only as a typed D-Bus value, never a shell. `Router::handle_wifi()` passes them to `WifiManager::save_profile` and `set_ap`, which refuse an SSID outside 1..32 bytes and a non-empty passphrase outside 8..63 before any D-Bus call (`AlpacaHTTP/src/util/wifi_manager.cpp`).
- **Resource budget.** One `sd_bus` connection serialized on one mutex (see the wifi-manager instructions). A scan holds it for a fixed 1.5 s `nanosleep` after `RequestScan` (`WifiManager::scan()`); other Wi-Fi calls queue on it and each holds one of the shared 32 workers.

### Surface 5: Discovery responder

- **Who can reach it.** Any host that can send UDP to the discovery port (default 32227, set by `discovery.port` or `ALPACAHTTP_DISCOVERY_PORT`; Alpaca API Reference s5.8.1), including the multicast group.
- **What it can do.** Learn that an Alpaca server exists and its HTTP port. The reply is a fixed JSON object with the `AlpacaPort` key (`Discovery::handle_probe`, `AlpacaHTTP/src/discovery/discovery.cpp`). At the default port 6800 it is 19 bytes against a 16-byte minimum matching probe (`alpacadiscovery1`); this small amplification is not a useful reflector and needs no rate limit.
- **What is trusted and why.** The sender address is spoofable and the reply goes to it; with a reply this small that is accepted.
- **Logging.** `handle_probe` logs every probe at INFO and every non-Alpaca datagram at DEBUG (issue #740), so a host sending garbage in a loop adds no WARNING line. A valid probe still logs one INFO line per probe.
- **Resource budget.** One socket, one thread, one small `sendto` per probe; no memory grows with the count.

### Deliberate departures from the Alpaca API listings

The Device API schema (`docs/AlpacaDeviceAPI_v1.yaml`) lists HTTP 200, 400 and 500 for every path. The Management API schema (`https://ascom-standards.org/api/AlpacaManagementAPI_v1.yaml`) lists `GET` only for `/management/v1/description`, with 200, 403 and 500. AlpacaBridge departs from those listings on purpose in the places below. None changes what an ASCOM member returns: each one answers before a request reaches a driver, or on a path the Alpaca APIs do not define. Paths the listings do not define at all (an unroutable URL answered 404, the static web UI files and their 403) are outside this list. The Device API 400-versus-404 rule is owned by the [AlpacaHTTP conformance instructions](../../.github/instructions/alpaca-http-conformance.instructions.md) (HTTP status codes).

- **403 on the Device API and on `/management/apiversions`.** A forged browser request (origin guard, `reject_cross_origin_request`) and a request whose `Host` the allowlist refuses (`reject_disallowed_host`, applied to every path) get 403. The Device API lists no 403, and the Management API lists none for `apiversions`; it lists 403 for `v1/description` and `v1/configureddevices`. A 200 with an Alpaca error would tell a hostile page that the device exists and what it refused; 403 refuses the request as a request.
- **Transport-level refusals on every HTTP surface.** `read_request()` in `server.cpp` answers before any request exists to route: 413 for a `Content-Length` above `Request::kMaxBodyBytes` (NS-04, issue #741), 431 for a header block above `kMaxHeaderBytes`, 408 when the whole request has not arrived within `kRequestDeadlineSeconds`, 501 for any `Transfer-Encoding` (the body is framed from `Content-Length` only; the conformance instructions explain the choice), 400 for a duplicate or invalid `Content-Length`, and 400 for a line that `Request::parse()` would frame differently from `read_request()` (issue #1692): "Invalid request line" for a bare CR or LF in the request line, and "Invalid header name" for whitespace before the colon, a leading space or tab, or a bare CR or LF in a header line (`Request::header_line_ok()`, shared by both). A receive timeout before the deadline closes the connection without an answer. These are resource and framing bounds from the Resource budget above; there is no request to answer with an Alpaca envelope.
- **400 for an over-long path.** A URL path above `kMaxRequestPathBytes` gets 400 with a `text/plain` message at the top of `Router::route()`, before any routing (issue #711, issue #1476), on every path including `/management/v1/description`.
- **`PUT` and `POST` on `/management/v1/description`.** The Management API defines this path for `GET` only. AlpacaBridge adds `PUT` and `POST`, handled alike, so the web UI can set the server description, the clock-sync opt-out and the Host allowlist (`handle_description`, issue #392). A client that uses only `GET` sees the standard behavior.
- **400 with an Alpaca body from a `PUT` or `POST` to `/management/v1/description`.** A write the server cannot interpret answers 400: a missing body, invalid JSON, no known property, an invalid field value (issue #1476), an invalid `AllowedHosts` list and the self-lockout rule above. A valid write that then fails (the settings cannot be persisted, or a management driver owns the location) stays HTTP 200 with a non-zero `ErrorNumber`. The Management API lists no 400 for this path, and no `PUT` or `POST` for it at all.
- **405 only where the Alpaca APIs define no path.** A wrong verb on an Alpaca-defined management path answers 400 with a `text/plain` message (Alpaca API Reference s2.5, issue #1508): on `/management/v1/description` a verb other than `GET`, `PUT` or `POST` (`handle_description`), and on `/management/apiversions` and `/management/v1/configureddevices` (and their unversioned aliases) any verb other than `GET` (`handle_api_versions`, `handle_configured_devices`). The AlpacaBridge-only management paths, which the listings do not define, keep 405.
- **400 on the AlpacaBridge-only management paths.** `configuredevice` answers 400 when a request names a GPIO line, GPIO chip or device path outside the board's allowlist, or a duplicate GPIO line (`kHardwareConfigRefusal`, issue #765). A `POST` or `PUT` to `/management/v1/update/channel` answers 400 for a body that is not `{"Channel":"stable"|"beta"}` (issue #1103). Neither path is in the Alpaca APIs. The departure is from AlpacaBridge's own convention that management responses are HTTP 200 with a non-zero `ErrorNumber`. These requests try to choose which line, node or package source the server opens. That is a refused request, not a failed operation, and a status the web UI and a script can test without parsing the body.

The transport-level refusals (408, 413, 431, 501, the header 400s, and the 400 "Invalid request" when `Request::parse()` fails in `serve_one_request()`) carry a short plain-text body from `send_error()` in `server.cpp`. Every router 400 on a request the Alpaca APIs define carries a `text/plain` message and no JSON envelope (Alpaca API Reference s2.5, issues #1476 and #1508): the Device API 400s (unknown device type, invalid device number, unknown method, wrong verb, unregistered device, the over-long path, and a required parameter that is missing or does not parse), and the wrong-verb 400s on `apiversions`, `configureddevices` and `description`. The 400s of the AlpacaBridge-only management requests keep the Alpaca JSON envelope: the `PUT` or `POST` write to `description` (the Management API defines no such verb), `configuredevice` and `update/channel`. The web UI reads their `ErrorMessage`.

### Accepted residual risks

Accepted by the board on 2026-09-30; they bind as part of this record.

- **R1. A device config accepts any `portPath`, `host`, `tcpPort` or `hidPath`, so `LastConnectError` is a LAN reachability oracle.** A configuring host can register a device pointing at any host and port, connect it, and read the driver refusal (`configureddevices`, `LastConnectError`, `Router::handle_configured_devices()`). Accepted because a LAN host can already probe the LAN directly. Kept: no driver puts a credential in a connect error.
- **R2. Duplicate query and form parameters resolve by position and the two parsers differ.** The query parser keeps the last value (`Request::parse_query_string()`, `AlpacaHTTP/src/core/request.cpp`); the form body keeps the first (`get_form_value`, `AlpacaHTTP/src/http/device_call.cpp`). Accepted because both are deterministic, no security check depends on a duplicate, and the caller is a trusted LAN host; it would matter only behind a proxy or filter that decides on the other occurrence, which is unsupported.
- **R3. The udev rules install `MODE=0666` on whole vendor IDs** (`AlpacaCore/external/**/*.rules`), so any local user can open the device. Accepted because the board is a single-purpose appliance and group access would break the companion projects that dlopen the same libraries. Unacceptable on a shared machine; the README must say so.
- **R4. `GET /management/v1/wifi/scan` holds the Wi-Fi mutex for 1.5 s.** Accepted because today the 32-worker pool and the global connection limit bound it (there is no per-host cap yet; the proposed cap is NS-03+05, open), it degrades only the Wi-Fi card, and a host that reaches the board can disconnect it by other means. The scan changes no state, so it stays a GET.

### Reviewer check for any new route or persisted config field

A change passes only when every rule in the Rules section below holds.

### Known gaps (open)

- No framing or CSP headers (NS-08).
- No per-host connection cap (NS-03+05).
- Driver-reaching device PUTs are not origin-guarded; safe while no CORS preflight is answered. The DNS-rebinding path to them is open by default: the Host allowlist (NS-02) closes it only when `http.host_check_enabled` is on. The reviewer check holds the rest.
- Unbounded detached threads per shutdown and restart request ([#1739](https://github.com/open-astro/AlpacaBridge/issues/1739)).
- 28 unaudited `innerHTML`-class sinks in `app.js`; tracked with the NS-14 items (web UI item).

### Finding map

| Finding | Surface | Status | Where handled |
| --- | --- | --- | --- |
| NS-01 | Management API | fixed | landed on main |
| NS-02 | Device API, Management API, Web UI | opt-in, off by default | Host allowlist against DNS rebinding (issue #392), applied only when `http.host_check_enabled` is true: `reject_disallowed_host` in `Router::route()`, extra names from `http.allowed_hosts`, editable over `PUT /management/v1/description` (`AlpacaHTTP/src/core/config.cpp`, `Config::host_check_enabled()` in `AlpacaHTTP/include/alpacahttp/config.h`); closes the rebinding half of the unguarded device PUT gap when enabled; with the default (off) that half is open |
| NS-03+05 | all HTTP surfaces | open | per-host connection cap |
| NS-04 | all HTTP surfaces | fixed | request body cap lowered from 10 MiB to 64 KiB (issue #741) |
| NS-06 | Wi-Fi API | open | hotspot passphrase by default |
| NS-07 | Device API, Management API | fixed | JSON output replaces invalid UTF-8 with U+FFFD instead of throwing (`error_handler_t::replace` in `Response::set_body`, `AlpacaHTTP/src/core/response.cpp`, issue #764) |
| NS-08 | Web UI | open | framing headers and CSP |
| NS-09 | Device API | fixed | the iOptron telescope no longer changes the mount altitude limit (`:SAL`) or meridian treatment (`:SMT`); a GOTO the firmware refuses is reported and not retried (issue #763) |
| NS-10 | Management API | open | the WeeWX feed URL follows redirects and reads an unbounded body (`http_get()` in `AlpacaCore/src/vendors/weewx/weewx_observingconditions_driver.cpp`); its scheme is limited to `http,https` for the request and every redirect |
| NS-11 | Management API | fixed | a device config may name only the board's own GPIO chip and lines (`AlpacaCore/include/alpacacore/util/hardware_config_refusal.h`, issue #765, PR #818) |
| NS-12 | Device API | open | an image array request holds the whole frame (`CameraDriver::get_image_array()`, `AlpacaCore/include/alpacacore/camera_driver.h`); memory per request has no stated constant |
| NS-13 | Discovery, Management API | fixed | the discovery non-Alpaca datagram and the unmatched setup path log at DEBUG, the path cut to 256 bytes (issue #740) |
| NS-14 | several | open | split into separate low-severity changes: HTTP framing from parsed headers; strict URL decoding; config clamps and join-on-every-path for discovery, shutdown and restart; input length and count bounds; PulseGuide range; clock-step log pruning; atomic config writes; streamed log download. Four NS-14 items are the accepted risks R1 to R4. |

When a finding lands, set its row to `fixed` in the same change (rule 12).

## Rules

The reviewer check, for any new route or persisted config field. Each rule names where it applies and what checks it.

1. A state change is never a GET. Scope: `AlpacaHTTP/src/**`. Checked by: review.
2. The origin guard covers a route on the path that reaches the driver, not only on the wrong-verb path. Scope: `AlpacaHTTP/src/http/router.cpp`. Checked by: review.
3. A change that answers a CORS preflight, adds an `Access-Control-*` header or handles OPTIONS passes every driver-reaching PUT through the origin guard in the same PR. Scope: `AlpacaHTTP/src/**`. Checked by: review.
4. The body and every string field have a byte bound, and integers are range-checked before a narrowing cast, so nothing wraps. Scope: `AlpacaHTTP/src/**`, `AlpacaCore/src/catalog/*.cpp`. Checked by: review.
5. A URL field a client can set accepts only `http` and `https` and has a size cap; an operator-only URL (the update URLs below) meets the scheme part at fetch time. Scope: `AlpacaHTTP/src/**`, `AlpacaCore/src/vendors/**`. Checked by: review.
6. Paths and GPIO chip and line fields are validated or behind an explicit opt-in. Scope: `AlpacaHTTP/src/**`, `AlpacaCore/src/vendors/**`. Checked by: review.
7. A hardware-safety override is opt-in and off by default. Scope: `AlpacaCore/src/vendors/**`, `AlpacaHTTP/src/**`. Checked by: review.
8. No credential appears in a client-facing error, including `LastConnectError`. Scope: `AlpacaCore/src/vendors/**`, `AlpacaHTTP/src/**`. Checked by: review.
9. A failure a client can trigger is not logged at WARNING or above. Scope: `AlpacaHTTP/src/**`, `AlpacaCore/src/vendors/**`. Checked by: review.
10. Memory per request is bounded by a stated constant. Scope: `AlpacaHTTP/src/**`. Checked by: review.
11. JSON output survives non-UTF-8 input (a replace handler, or a reject at parse). Scope: `AlpacaHTTP/src/**`. Checked by: review.
12. A change that lands a finding sets its row in the finding map to `fixed`. Scope: `docs/decisions/0007-lan-surface-threat-model.md`. Checked by: review.
13. A new HTTP status or verb on a path the Alpaca API listings define, outside what they list for it, is added to Deliberate departures in the same change, with its reason. A status on a path the listings do not define (the AlpacaBridge-only management paths, static files) needs no entry, and may answer 405; a wrong verb on an Alpaca-defined management path answers 400 with a text message (Alpaca API Reference s2.5). A 400 the Device API lists (a path, device type or device number the server cannot interpret, or a required parameter that is missing or does not parse as its type) needs no entry and carries a `text/plain` message; a value that parses but is out of range, and an operation that fails, stay HTTP 200 with a non-zero `ErrorNumber`. Scope: `AlpacaHTTP/src/**`. Checked by: review.

The update check URL `update_packages_url` (`server:` key in the YAML config and env `ALPACAHTTP_UPDATE_PACKAGES_URL`, not settable over HTTP; `update_release_notes_url` and `update_release_url` are the same kind) meets the scheme line at fetch time: libcurl is limited to `http,https` for the request and every redirect (`CURLOPT_PROTOCOLS_STR` and `CURLOPT_REDIR_PROTOCOLS_STR` in `AlpacaHTTP/src/util/software_update.cpp`) and the response to 8 MiB (`CURLOPT_MAXFILESIZE_LARGE`). The URL string itself has no length cap; only the operator writes it.

## Alternatives rejected

- **Add authentication.** Alpaca defines none; NINA, ConformU and every client would stop working. A shared secret over plain HTTP on an open hotspot protects nothing against the radio-range attacker anyway.
- **Bind to localhost or one interface.** The server is used from another machine by design and the LAN address changes with the hotspot.
- **Require a preflight or custom header on every state change.** It breaks non-browser clients that send no `Origin`.
- **Treat the Origin check as the whole browser defence.** It compares two attacker-influenced strings under DNS rebinding; it needs the Host allowlist beside it (enable `http.host_check_enabled`; it is off by default).
- **Fix R1 to R4 now.** R1, R2 and R4 need policy the LAN model lacks (a reachable-host allowlist, a proxy-aware parser); R3 is the price of the shared libraries.

## Consequences

- Reviewers get one list for any new route or config field: the Rules above. The instruction files link here rather than restating it.
- Gaps with a filed upstream issue name it in the finding map (NS-02 issue #392, NS-04 issue #741, NS-07 issue #764 and NS-13 issue #740, all four landed). The other open rows have a proposed fix and no owner or issue yet; this record does not promise one. The device PUT guard rests on the Host allowlist and the reviewer check; the detached threads are [#1739](https://github.com/open-astro/AlpacaBridge/issues/1739) and the `innerHTML` audit goes with the NS-14 low-severity items.
- Accepted by the board on 2026-09-30 (R1 to R4 accepted as residual risk).
- NS-09 to NS-14 are not decided here; each carrier decides its fix and must pass the reviewer check.
- ConformU and other non-browser clients are unaffected by every control named here.

## Links

- Guard: `AlpacaHTTP/src/http/router.cpp` (`reject_cross_origin_request`); limits: `AlpacaHTTP/src/http/server.cpp`, `AlpacaHTTP/include/alpacahttp/config.h`, `AlpacaHTTP/include/alpacahttp/request.h`.
- Discovery: `AlpacaHTTP/src/discovery/discovery.cpp`. Wi-Fi: `debian/alpacabridge.polkit-rules`, `debian/alpacabridge.service`, `docs/wifi-manager-design.md`.
- Instruction files that link the reviewer check: [AlpacaHTTP conformance](../../.github/instructions/alpaca-http-conformance.instructions.md), [WiFi manager](../../.github/instructions/wifi-manager.instructions.md).
- Related: [Server thread ownership](0002-server-thread-ownership.md), [Device catalog](0004-device-catalog.md).
- Upstream issues [#392](https://github.com/open-astro/AlpacaBridge/issues/392), [#711](https://github.com/open-astro/AlpacaBridge/issues/711), [#713](https://github.com/open-astro/AlpacaBridge/issues/713), [#740](https://github.com/open-astro/AlpacaBridge/issues/740), [#741](https://github.com/open-astro/AlpacaBridge/issues/741) and [#764](https://github.com/open-astro/AlpacaBridge/issues/764).
