---
applyTo: "AlpacaCore/src/vendors/**,AlpacaCore/include/alpacacore/vendor/**,AlpacaCore/include/alpacacore/util/**,AlpacaCore/tests/*fake*,AlpacaCore/tests/*pty*"
---

## Serial I/O and auto-detect

Shared rules for every vendor, moved verbatim from `AGENTS.md`, which keeps the
core rules and routes here. A rule that applies to a second vendor belongs in
the matching shared file, not in a vendor file.

### Serial / socket I/O: always use the shared helpers (`util/serial_io.h`)

[Decision 0017](../../docs/decisions/0017-serial-port-ownership.md) makes `util::SerialLink` the one way a
wrapper opens a serial port. It is in force on `stable/5.0`; on `main` the port is #1448, and
every serial wrapper here is a defect against that record until it lands. The text below
describes `main`'s current wrappers and is replaced by the `SerialLink` rules in #1448.

POSIX serial and socket code has several easy-to-get-wrong patterns that must not
be hand-rolled in a wrapper. Use `alpacacore/util/serial_io.h` (POSIX-only,
included inside the existing `#ifndef _WIN32` branches):

- **`util::write_all(fd, data, len)`** instead of a bare `write()`. `write()` may
  satisfy only part of the payload (`0 < n < len`) or be interrupted (`EINTR`);
  treating any non-negative return as success silently drops trailing bytes (e.g.
  a command terminator). `write_all` loops until the whole payload is written and
  treats a `0` return as a hard error (no infinite spin).
- **`util::send_all(fd, data, len, MSG_NOSIGNAL)`** for every socket `send()`. The
  socket analogue of `write_all`; **always pass `MSG_NOSIGNAL`** so a peer drop
  mid-send returns an error instead of delivering `SIGPIPE` (which would kill the
  server). A short send that isn't completed corrupts the next command's framing.
- **`util::clear_nonblocking(fd)`** / **`util::set_nonblocking(fd)`** instead of a
  raw `fcntl(F_GETFL)`+`F_SETFL`. A failed `F_GETFL` returns `-1`; feeding that into
  `F_SETFL` can leave the fd in the wrong mode (a stuck-non-blocking fd spins a
  reader at 100% CPU; a stuck-blocking connect socket hangs for the full ~127s TCP
  timeout). Both helpers check both `fcntl` calls and return false on failure.

Apply these on **both** the auto-detect probe path **and** the production
connect/open path — the fd is typically opened `O_NONBLOCK`, so `connect_serial()`
must `clear_nonblocking()` after `tcsetattr` (not just the probe), or reads ignore
`VMIN`/`VTIME` and `write_all` fails on `EAGAIN`. Use the **same abort-on-failure
pattern** (`close(fd); return false/""`) at every call site.

**Exception — Sky-Watcher direct serial.** On the **connected link**, that driver
deliberately keeps the fd **non-blocking** and bounds its reads and writes with `poll()`
instead, because some USB CDC-ACM virtual COM ports do not honour `VMIN`/`VTIME` and a
blocking `read()` on a board that went quiet parks forever in `n_tty_read`, wedging the
driver. There `connect_serial()` calls `set_nonblocking()`, `settle_serial`/`exchange_serial`
reads go through a `poll(POLLIN)`-bounded helper, and frame sends go through a
`poll(POLLOUT)`-bounded write (not `write_all`, which would fail fast on an `EAGAIN` before
the first byte). Do **not** "fix" that back to `clear_nonblocking` — see the Sky-Watcher
scoped instructions. The auto-detect *probe* path, `probe_skywatcher_port`, keeps its fd
non-blocking and bounds each read with `poll(POLLIN)` against its 1500 ms budget the same way,
and so does the SynScan echo guard it runs first at 115200 (`exchange_synscan_echo_on_fd` in
`util/synscan_handset_probe.h`), so a silent candidate on such an adapter cannot hang the scan.

### Auto-detect failure message (`util/auto_detect.h`)

Serial port enumeration is POSIX-only, so the `enumerate_*_ports()` helpers return
empty on Windows. When an auto-detect driver finds no ports, throw
`util::serial_auto_detect_failed_message("<device label>")` rather than a
hard-coded "no device found" string, so the Windows path reports "auto-detect not
supported on this platform" instead of implying missing hardware.

### Auto-detect resolves at connect, never in a factory (`util/connection_resolver.h`)

A persisted device is constructed at server start-up, and the hardware is often
not there yet: a Wi-Fi mount is still joining the access point, a USB adapter is
powered after the SBC. Every auto-detect factory (`create_*_auto`,
`_auto_network`, `_by_index`) used to run its serial probe or subnet sweep at
construction, so the factory threw, the router logged "Failed to load persisted
device", the web UI showed `(failed to load)`, and nothing ever retried (issue
#659, iOptron HAE over Wi-Fi on the Pi rig). Rules:

- **The factory hands the driver a `util::ConnectionResolver<Info>`** (the
  scan as a callable that returns the endpoint or throws the operator-facing
  refusal) and constructs without touching the bus. The scan body stays a
  plain function (`resolve_<vendor>_<mode>(index)`) so it is reusable and
  testable on its own.
- **The connect path calls `util::connect_resolved(info_, resolved_, resolver_,
  try_connect)`** under whatever lock it already holds. `try_connect` throws on
  failure (the existing `if (!protocol.connect(info)) throw ...` moved into the
  lambda). The helper retries the last resolved endpoint before scanning again,
  so repeated ConformU / NINA connects stay under the Platform 7 5 s `Connect()`
  budget. **Re-scanning is opt-in**: the retry falls through to the resolver
  only when the lambda throws `util::StaleEndpoint`, which it does for a
  vanished serial or HID node (`util::device_node_missing()`), a refused network
  connect, or a failed identity gate (SynScan's echo test). Every other
  exception propagates: a scan DTR-resets every CP210x / CH340 device on the
  box, so a wheel still homing or a handshake that missed once must never start
  one (review of #660). The scan's own message propagates as the connect
  refusal (#358), so it reaches the client and `LastConnectError`. **The first
  connect after a service restart pays the scan** (5.5 s for the iOptron Wi-Fi
  sweep, up to two boots for the CFW3 probe) inside the Platform 7 `Connecting`
  window, so before a ConformU run against a freshly restarted service connect
  the device once from the web UI, or give it an explicit port or host.
- **Every driver converted from a construction-time scan (the ten in #660)
  exposes a `create_*_deferred(device_number, resolver, ...)` seam** and ships
  the cases in `tests/deferred_connect_cases.h` over its fake:
  refused (construction succeeds, the refusal is the connect error, sync and
  async), reused (one scan across a reconnect), re-resolved (the fake behind the
  resolved endpoint dies, a new one appears, the driver reaches it). A driver
  whose deferred factory cannot take a fake (Astroasis: its HID fake is injected through `create_astroasis_focuser`, not the resolver factory) ships the refusal case alone and says so.
  Put the vendor's cheap identity gate (SynScan's echo test) INSIDE the
  `try_connect` lambda: the helper only re-scans when the lambda throws, and a
  stale port another adapter now owns still opens.
- Drivers that resolve inside `set_connected(true)` by hand (iEFW
  `resolve_serial_port_locked()`, Gemini PDH / flat panel, WandererAstro) already
  meet the rule; do not move them back into a factory. A new `_auto` or
  `_by_index` factory that scans at construction is a review-blocking regression.

### Serial auto-detect scan (`util/serial_by_id_scan.h`)

Every auto-detect `enumerate_*_ports()` scans `/dev/serial/by-id` and, for most
vendors, falls back to raw `/dev/ttyUSBn`/`ttyACMn` nodes when a device isn't
covered by (or visible in) `by-id`. Use the shared helpers below instead of
hand-rolling this — issue #179 found nine near-identical copies of the scan, each
independently broken the same way, before it was centralised:

- **`alpacacore::util::list_serial_by_id(dir)`** instead of a raw
  `std::filesystem::directory_iterator` + `is_symlink()` loop. Built entirely on
  the `std::error_code` overloads, so it never throws `filesystem_error` — a
  device unplugged mid-scan just ends the scan with whatever was already
  collected, instead of aborting the whole `enumerate_*_ports()` call and
  discarding results already found. Still resolve each returned `.path` with
  `std::filesystem::canonical(path, ec)` yourself using the `error_code`
  overload (never the throwing one) — the by-id symlink can go stale between
  the scan and that call too.
- **`alpacacore::util::read_raw_tty_usb_descriptor(port_path)`** +
  **`usb_tty_descriptor_matches(descriptor, {...})`** to filter a raw
  `/dev/ttyUSBn` fallback by USB vendor/manufacturer/product, read straight
  from sysfs — the same fields udev uses to build by-id names. Required
  whenever the raw fallback can run unconditionally (see next point): without
  it, the fallback opens — and for CH340/CH341-class hardware, DTR-resets —
  every serial device on the box, not just this vendor's. Beware the udev
  spelling trap (issue #181): by-id *names* carry udev's underscore mangling
  (`USB_Serial`), but the raw sysfs strings keep their spaces (`USB Serial`) —
  the helper matches patterns against both spellings, so udev-style patterns
  copied from a by-id name filter are fine, but don't "simplify" that
  double-match away.
- **`alpacacore::util::path_exists(path)`** instead of bare
  `std::filesystem::exists(path)` anywhere in an `enumerate_*` scan. The plain
  overload throws `filesystem_error` on a traversal error (e.g. EACCES on a
  parent directory), aborting the whole auto-detect including fallback passes
  (issue #181 — every wrapper had this, and in ZWO it silently replaced a
  `try`/`catch` that existed specifically to protect the WiFi fallback).
- **Run the raw fallback unconditionally, not only when `by-id` is entirely
  absent, and dedupe by resolved canonical path.** Generic USB-serial adapters
  (CH340/CH341, Prolific, FTDI, CP210x) report identical descriptor strings
  with no per-device serial number, so when two of the same chip are plugged
  in at once, udev's by-id naming collides and only ONE gets a symlink — the
  other silently vanishes from a `by-id`-only scan even though the directory
  itself exists. Track already-probed canonical paths in a
  `std::set<std::string>` (or `unordered_set`) populated as the `by-id` pass
  resolves each candidate, and skip any raw-fallback port already in that set
  — otherwise a port tried via `by-id` gets opened (and reset) a second time.
- **Collect candidates first, then probe them concurrently (issue #218).** A
  probe against a port that isn't this device (the iOptron mount shares the
  Prolific chip class with the iEAF and iEFW; a mount controller often shares
  CH340 with the Gemini gear) costs the full handshake timeout — ~5 s for the
  iOptron probes, ~6 s for Gemini — so probing inside the scan loop made
  auto-detect connect time grow linearly with adapter count. Both scan passes
  now only append `{path, name}` to a `candidates` vector; a `std::thread` per
  candidate then runs `probe_port()` into an index-matched results vector, and
  the found ports are emitted in candidate order (so the auto-detect device
  index stays deterministic). Wall time is bounded by one port's worst case.
  Don't move the probe back into the scan loop when copying this pattern to a
  new vendor, and keep the by-id/raw dedupe set — it is what stops two threads
  opening the same port at once.
