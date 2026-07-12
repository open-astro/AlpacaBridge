# AlpacaBridge Code Audit

> **RESOLUTION (2026-07-11, branch `audit-fixes-3.0.1`):** All Critical, High, and Medium findings and the tractable Low findings below are **fixed** on this branch (see CHANGELOG 3.0.1 UNRELEASED for the itemized list). Test-coverage gaps closed: telescope-driver TSan `[stress]` suites (ZWO/Celestron/SynScan/iOptron over a new loopback fake-mount seam), a `persisted_devices_` concurrency test, and router-level path-traversal / Content-Length tests. Deliberately not addressed: network-authentication-dependent items (CSRF on `/shutdown`/`/restart`) — ASCOM Alpaca defines no authentication model; and the full multi-device redesign of the singleton protocol wrappers (a connect-steal guard now throws a clear error instead of silently stealing).

**Date:** 2026-07-11
**Version audited:** 3.0.0 (commit `a3ba62d`, branch `main`)
**Scope:** All first-party source (~57k LOC across AlpacaCore + AlpacaHTTP). Vendor SDKs under `external/` excluded except to verify API usage.
**Method:** Six parallel subsystem reviews — HTTP transport, core/base drivers, ZWO drivers, camera-SDK drivers (ToupTek/PlayerOne/SVBONY/QHY), serial/protocol drivers (Bisque/Celestron/Gemini/iOptron/SynScan/WandererAstro/WeeWX), and build/CI/packaging/web-UI. Findings audited against the project's own `AGENTS.md` "Driver concurrency & lifecycle" contract. Line numbers for the most severe items were spot-verified against source.

---

## Executive summary

AlpacaBridge is, on the whole, a **carefully engineered and well-tested** project. The concurrency core (`AsyncConnectable`, `serial_io.h`, `poll_settle.h`), the Alpaca envelope/JSON/routing logic, the log-file endpoints, and the CI/supply-chain setup are genuinely hardened — the CI in particular (SHA-pinned + checksummed actions, local/CI parity, zizmor self-audit) is better than most projects this size.

The weaknesses cluster in three places:

1. **Vendor driver *operation* paths.** The connect/disconnect *lifecycle* follows the `AGENTS.md` contract, but the *operational* paths (async slews, pulse guides, cooler control, sync waits) still hand-roll **detached threads that capture `this`** — the single pattern the contract most explicitly forbids. Several are destructor-unsafe (use-after-free) and one can call `std::terminate` and crash the entire server.
2. **The HTTP transport edge.** `server.cpp` and `handle_static_file` read as early-prototype code that never got the hardening the rest of the module has: a **path-traversal file-read**, no socket timeouts, trusted `Content-Length`, single unbounded recv/send, and an **unguarded shared `persisted_devices_` vector** raced across worker threads.
3. **Deployment posture.** An unauthenticated network daemon shipped with **zero systemd sandboxing** and an over-privileged service user (notably the `input` group).

A recurring meta-finding, and the one worth internalizing: **the project's own "fix shared patterns everywhere" rule is being violated repeatedly.** Multiple bugs are ToupTek/Celestron/ZWO-camera fixes that were never swept into their SVBONY/PlayerOne/QHY/SynScan twins. The reference implementations are correct; the copies lag.

### Severity tally

| Severity | Count |
|----------|-------|
| Critical | 4 |
| High     | 13 |
| Medium   | 20 |
| Low      | 20+ |

---

## Critical

### C1 — Path traversal / arbitrary file read in static web serving
`AlpacaHTTP/src/http/router.cpp:5276-5356` (`handle_static_file`) — **verified.**
The handler builds a filesystem path by string concatenation with no `..` rejection or canonicalization. `request.path()` is stored raw. `GET /web/../../../../etc/passwd` passes the `starts_with("web/")` guard; `filename` becomes `../../../etc/passwd`; the file is opened and returned as `text/plain`. On a LAN-exposed bridge this is a full arbitrary-read primitive against anything the process can read (config, device registry, `/etc/passwd`, SSH keys).
**Fix:** canonicalize the resolved path and confine it to the web root; reject any path containing `..` segments.

### C2 — ZWO telescope: detached teardown thread captures `this`, races destructor
`AlpacaCore/src/vendors/zwo/zwo_telescope_driver.cpp:377-413` — **verified** (`std::thread([this]{…}).detach()` that locks `mutex_`, calls `stop_poll_thread()`/`stop_pulse_thread()`).
The destructor calls `set_connected(false)`, which spawns this detached thread and returns immediately; the object is then destroyed while the detached thread is still locking `mutex_` and joining member threads → **use-after-free**. Direct violation of the `AGENTS.md` never-detach rule.

### C3 — Celestron async slew tail can throw out of a detached thread → `std::terminate` (whole-server crash)
`AlpacaCore/src/vendors/celestron/celestron_telescope_driver.cpp:1234-1246` (with `1570`, `1599`, `1422-1424`).
The detached slew-completion thread calls `restore_tracking_after_slew_locked()` / `learn_ra_offset_locked()` **outside** a try-block; on a disconnect-during-slew these rethrow `NotConnected`. An exception escaping the thread lambda calls `std::terminate`, killing the Alpaca server and every other device with it.

### C4 — QHY disconnect closes the camera without joining the exposure thread → use-after-free in libqhyccd
`AlpacaCore/src/vendors/qhy/qhy_camera_driver.cpp:220-284`.
`set_connected_impl(false)` calls `CloseQHYCCD` without joining `exposure_thread_` or taking `exposure_lifecycle_mutex_`. If a client disconnects mid-exposure, the worker is blocked in `GetQHYCCDSingleFrame` on a handle that `CloseQHYCCD` then frees → UAF inside the SDK. Every sibling driver joins the exposure worker before close; QHY only does so in the destructor, not on a normal Disconnect. Violates the `AGENTS.md` close-order rule.

---

## High

### Concurrency / lifecycle (detached-thread-captures-`this` family)
The `AGENTS.md` rule "**Never `.detach()` a thread that touches `this`**" is violated in numerous operational paths. None are joined by `shutdown_connection()`; all are destructor-unsafe UAF risks:

- **H1** — ZWO telescope async GOTO setup: `zwo_telescope_driver.cpp:1895-1957` (detached, ~1 s lifetime).
- **H2** — QHY cooler-off: `qhy_camera_driver.cpp:546-573` — **verified** (`std::thread([this]{…}).detach()`).
- **H3** — QHY pulse-guide timer: `qhy_camera_driver.cpp:1009-1012` (writes `pulse_guiding_` after `sleep_for`). PlayerOne fixed this exact bug (round 9, `shared_ptr` flag); never swept into QHY.
- **H4** — Celestron/SynScan/iOptron async slews: `celestron_…:1173-1247`, `synscan_…:1005-1023`, `ioptron_…:1312-1320, 1411-1424`.

### H5 — QHY temp/telemetry thread double-start race → `std::terminate`
`qhy_camera_driver.cpp:1333-1367, 1383-1431`. Two concurrent starters both pass the `joinable()` pre-check; the loser fails to move its running thread, whose destructor on a joinable thread calls `std::terminate` — process abort.

### H6 — No read timeout on client sockets → trivial slowloris DoS
`AlpacaHTTP/src/http/server.cpp:274-303`, `socket_utils.h:55-57`. Accepted sockets get no `SO_RCVTIMEO`; `socket_recv` is a plain blocking `recv`. A handful of idle connections park every worker thread in `recv()` forever with no recovery. The server has a small thread pool and no auth.

### H7 — Data race on `persisted_devices_` across worker threads
`AlpacaHTTP/src/http/router.cpp` (`handle_configured_devices` 1553-1641 iterates; `handle_configure_device` 5420-5469 `push_back`; `handle_remove_device` 5509-5563 `erase`; `load_persisted_devices` 7417). No mutex guards this `std::vector<json>` while the server dispatches to a thread pool → iterator invalidation / concurrent read+write → UB or crash. `server_info_mutex_` guards the string fields but not this vector.

### H8 — ZWO camera connect path leaks ref-counted open on any init throw; some cameras can never connect
`AlpacaCore/src/vendors/zwo/zwo_camera_driver.cpp:192-224`. No `catch(...){close; throw;}` guard after `open_camera()` (all EAF/EFW/CAA siblings have it). `ASIGetSerialNumber` returns an error for cameras with no serial number, so those cameras **cannot connect at all**, and each attempt permanently increments `open_count`.

### H9 — SVBONY camera connect path: same unguarded ref-counted-open leak
`AlpacaCore/src/vendors/svbony/svbony_camera_driver.cpp:222-308`. A throw from post-open init leaves `SVBOpenCamera` unbalanced; `connected_` stays false so nothing ever closes it. ToupTek has the guard; SVBONY never got it.

### H10 — SVBONY holds the singleton wrapper mutex across `SVBGetVideoData` (up to 500 ms) — **verified**
`svbony_sdk_wrapper.cpp:495-498`. `AGENTS.md` explicitly exempts `SVBGetVideoData` from being called under a lock; ToupTek and PlayerOne release it. During any exposure, every SDK call on every SVBONY device (temp poll, abort, disconnect, a second camera) stalls behind the download loop.

### H11 — ZWO SDK wrapper holds its single global mutex across `ASIGetDataAfterExp` (blocking USB bulk transfer)
`AlpacaCore/src/vendors/zwo/zwo_sdk_wrapper.cpp:452-455`. Abort/status/other-device calls all stall for the whole multi-second download, defeating the unlocked-download design the camera driver was built around.

### H12 — SynScan wrapper reads binary payloads through the text-framing path → corrupted coordinates/time
`AlpacaCore/src/vendors/synscan/synscan_protocol_wrapper.cpp:636-719` vs `read_response()` 992-998 (strips `0x0D`/`0x0A`, terminates on `0x23`). Latitude/hour bytes of value 10, 13, or 35 delete or truncate the frame, shifting every following field → wrong site → wrong LST/SideOfPier. **Celestron fixed exactly this** with a `binary_bytes` phase; the fix was never propagated. Classic "fix shared patterns everywhere" miss.

### H13 — SynScan pulse guide can leave an axis slewing indefinitely (mount runaway)
`synscan_telescope_driver.cpp:937-956`. The detached sleep-then-stop thread swallows a stop-command failure (`catch(...){}`, no retry). A serial glitch during a pulse → mount keeps moving at the commanded rate until manually stopped.

### H14 — Blocking TCP `connect()` with no timeout, under the driver mutex
`bisque_…:79`, `celestron_…:1415`, `synscan_…:882`, `ioptron_…:1249`. Timeouts are applied only after connect. An unreachable mount IP wedges the connection task (and driver `mutex_`) for the ~2 min kernel TCP timeout, hanging all Connected polls. The correct non-blocking `poll` pattern already exists in the iOptron prober but wasn't applied.

---

## Medium

**HTTP / transport**
- **M1** `request.cpp:107-116` — `body_.resize(std::stoul(content_length))` trusts the header; sub-`bad_alloc` values (e.g. `Content-Length: 500000000`) force large allocations from tiny requests → memory-exhaustion DoS on an SBC.
- **M2** `server.cpp:276-291` — single `recv` ≤8191 bytes, no accumulation loop; large configs or TCP-fragmented requests are silently truncated → parse failure / HTTP 400.
- **M3** `server.cpp:301-302`, `socket_utils.h:59-61` — single `send`, return value ignored, no write loop; large `imagearray` JSON or log archives truncate on a short write, delivering a corrupt body with no error.

**Base drivers / API contract**
- **M4** `telescope_driver.h:271-278` — `RightAscensionRate` documented as "arcseconds/second"; ASCOM unit is seconds-of-RA per sidereal second (a **15× error**). Any vendor following the header drifts at 15× for offset tracking.
- **M5** `rotator_driver.h:103-122` — `Move`/`MoveMechanical` doc comments have absolute/relative **swapped** vs ASCOM; a driver built to the comments fails ConformU.
- **M6** `dome_driver.h` / `safetymonitor_driver.h:26-38` — the only two device bases with no Platform 7 `get_device_state()` override; the first vendor dome/safetymonitor will pass local review then fail ConformU DeviceState.
- **M7** `device_registry.cpp:126-146` — virtual driver getters invoked while holding the global `g_registry_mutex`, establishing a registry→driver lock order; one slow driver connect stalls the entire management API, and any future `get_device()`-under-driver-mutex is an ABBA deadlock.
- **M8** `async_connectable.h:259-276` — publishes `kConnIdle` (Connecting=false) *before* running the deferred disconnect, so a client observes `Connecting==false && Connected==true` during the entire teardown of a disconnect-races-connect.

**ZWO**
- **M9** `zwo_telescope_driver.cpp:1356-1375` — `move_axis()` leaves `poll_pause_` stuck true if a protocol call throws; poll thread never refreshes again.
- **M10** `zwo_telescope_driver.cpp:1247-1293` — `set_site_latitude/longitude` set `site_coords_valid_=true` before the `has_longitude` check, so a lat-only set writes `longitude=0.0` to the mount, corrupting site.
- **M11** `zwo_camera_driver.cpp:443-450` — `get_full_well_capacity()` has a spurious `×1000` (reports full well 1000× too high).
- **M12** unlocked/torn reads: `zwo_telescope_driver.cpp:2474` (`pulse_guiding_end_`), `zwo_rotator_driver.cpp:244-313` (`sync_offset_` written before lock taken).
- **M13** `zwo_switch_driver.cpp:317-323` — `dew_caps_or_throw()` returns a `const&` into a mutex-guarded optional after releasing the lock; concurrent disconnect `.reset()` → dangling read. The camera fixed this (#116); the switch wasn't swept.

**Camera-SDK vendors**
- **M14** `touptek_camera_driver.cpp:1129-1133` — RAW8-only camera: worker always reads 16-bit/`rowPitch=num_x*2`; per SDK, RAW8 is 1 byte/pixel → every frame garbled.
- **M15** No `exposure_active_` gating on setters (gain/offset/ROI/bin/temp) in SVBONY (`570-576, 702-705, 1435-1509`), PlayerOne (`535-549, 659-674`), QHY (`1444-1485, 870-907`) — a setter mid-exposure corrupts the frame or the buffer-size math. ToupTek's `ensure_not_exposing()` guard was never swept.
- **M16** `svbony_camera_driver.cpp:986-1073` — ROI/speed dirty flags not re-marked on apply-failure; one transient failure breaks every subsequent exposure until the user re-touches ROI.
- **M17** SVBONY/PlayerOne/QHY don't clear cached caps on disconnect and their range getters lack a connection check → serve the previous camera's ranges instead of `NotConnected`.
- **M18** ToupTek `pulse_guide` (`888-913`) and overlapping pulses in PlayerOne (`830-842`) are not serialized against themselves → double-join UB / truncated guide pulses.

**Serial/protocol**
- **M19** Celestron/SynScan sync slews & park hold the driver mutex for up to 120 s (`celestron_…:1726-1755`, `synscan_…:1306-1336`); all property GETs and disconnect block. Bisque's unlock/sleep/relock shows the intended pattern.
- **M20** WeeWX: unsynchronized `supported_properties_` read races the poll thread (`weewx_…:434, 449` vs insert at `545`); plus stale-data timestamp stamped at fetch not observation time (`188`) so a dead station reports `TimeSinceLastUpdate≈0` forever. Also iOptron `set_park_position` (`ioptron_…:1853-1861`) uses unpadded `to_string` → malformed command; iOptron latches `device_faulted_` on one transient timeout (`1544-1548`), bricking the session.

---

## Low (selected)

- **Web UI XSS latent:** `app.js:537` uses quote-blind `escapeHtml` inside an inline `onclick` attribute; safe only because the server currently rejects the values — one server relaxation from stored XSS in an auth-less UI.
- **CSRF on `/management/v1/shutdown` & `/restart`** (`app.js:1210, 1242`) — unauthenticated simple-POSTs; any LAN web page can stop the observatory server (clean exit → `Restart=on-failure` won't respawn).
- **RGB24 channel order:** ZWO/PlayerOne/SVBONY copy BGR24 into the ImageArray without reorder → R/B swapped (rarely hit; shared-pattern candidate).
- **Unguarded `std::stod`/`stoi` on device responses** throw raw `std::invalid_argument` instead of `AlpacaException` across ZWO mount, iOptron, SynScan wrappers.
- **Per-vendor singleton protocol wrappers** (ZWO/Bisque/Celestron/SynScan/iOptron): a second device of the same vendor silently steals/tears down the first's connection.
- **`gethostbyname`** (obsolete, non-reentrant, IPv4-only) in all four network mount wrappers; `getaddrinfo` recommended.
- **ROI aligned *down* + zero-padded** (ZWO `538-559`, SVBONY `1485-1490`): a client requesting a 100-px width silently gets fabricated black columns — a photometry hazard. ToupTek pads up and stride-crops.
- **QHY has no exposure watchdog** — reports `Exposing` indefinitely if `GetQHYCCDSingleFrame` hangs.
- **Celestron/SynScan `is_aligned`** treats ASCII `'0'` as aligned (`ch != 0` bug), bypassing the slew-safety gate.

### Build / packaging / deployment
- **No systemd sandboxing** (`debian/alpacabridge.service`): no `NoNewPrivileges`/`ProtectSystem`/`ProtectHome`/`PrivateTmp`; runs a network-facing unauthenticated daemon as a user enrolled in `dialout`, `plugdev`, `gpio`, and **`input`** (keylogging surface). Config/state in writable `/etc/alpacabridge` blocks `ProtectSystem=strict`.
- **debian packaging:** files under aliased `/lib/...` violate Debian 13 usr-merge policy; `fxload` shipped with no `Conflicts/Replaces` risks clobbering the Debian package. `postinst` `chown -R` (no `-h`) dereferences the packaged `web` symlink and re-chowns dpkg-owned `/usr/share/...`.
- **Stale `localhost:11111` in `debian/control` description** — server listens on 6800; fresh users get connection refused.
- **Missing AGPL headers** on `web/*.js/.css/.html` despite the 3.0.0 relicense claiming full coverage. `vcpkg.json` is dead metadata.

---

## Test-coverage gaps

- The entire `Server` socket I/O path (where the highest-severity HTTP issues live) is untested — `test_routing.cpp` feeds well-formed requests straight into `Router::route`, bypassing sockets. No tests for path traversal, oversized/mismatched Content-Length, partial read/write, or read timeouts.
- No concurrency test for `persisted_devices_`.
- The TSan `[stress]` harness (#101) is excellent but only covers ToupTek camera/AFW/thermal-switch, ZWO EFW, PlayerOne — it does **not** cover the telescope drivers, which is exactly where the worst detached-thread bugs (C2, C3, H1) live.

---

## Recommended priority order

1. **C1** path traversal — small, high-impact, LAN-exposed read primitive.
2. **C2 / C3 / C4 / H1–H5** — the detached-thread + close-order UAF/`terminate` family; convert every detached `this`-capturing thread to a joinable member with a cancel flag + `condition_variable`, joined in the destructor, and wrap async tails in try/catch. Add telescope drivers to the TSan stress suite.
3. **H6 / H7 / M1–M3** — HTTP transport hardening (socket timeouts, recv/send loops, `persisted_devices_` mutex, bounded body).
4. **H8–H14, M9–M20** — the shared-pattern sweeps: connect-side leak guards, exposure-lifecycle gating, the SynScan binary-framing fix, connect timeouts, and de-locking the 120 s sync waits — applied across *all* siblings per the project's own "fix shared patterns everywhere" rule.
5. **Deployment** — add systemd sandboxing, drop the `input` group, move state to `/var/lib`, fix the `debian/control` port and packaging issues.
6. **M4–M8** — correct the base-driver unit/semantics docs (RA rate 15×, rotator Move/MoveMechanical) and add DeviceState to dome/safetymonitor bases before the first vendor driver is built on them.

---

*Findings were produced by subsystem review and audited against `AGENTS.md`. Line numbers for Critical items and a sample of High items were verified directly against source; remaining line references should be confirmed at fix time as the tree evolves.*
