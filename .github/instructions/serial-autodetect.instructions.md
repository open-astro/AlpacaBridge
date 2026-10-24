---
applyTo: "AlpacaCore/src/vendors/**,AlpacaCore/include/alpacacore/vendor/**,AlpacaCore/include/alpacacore/util/**,AlpacaCore/tests/*fake*,AlpacaCore/tests/*pty*"
---

## Serial I/O and auto-detect

Shared rules for every vendor, moved verbatim from `AGENTS.md`, which keeps the
core rules and routes here. A rule that applies to a second vendor belongs in
the matching shared file, not in a vendor file.

### Serial / socket I/O: always use the shared helpers (`util/serial_io.h`)

**Open every serial port with `util::SerialLink` (`util/serial_link.h`, issue #1202).** Many vendors ship
the same CH340 adapter, so one vendor's auto-detect probe matches every other vendor's device. When only
some wrappers used the in-use registry, a Gemini flat panel scan opened the connected Gemini PDH's port at
its own baud rate and the hub stayed faulted until restart, and a port retuned to `VMIN=1` by another
program parked a WandererRotator monitor and an OnStep drain in `n_tty_read`, hanging Disconnect.
`SerialLink::open(path, kind, owner, configure, &error)` claims the port (`util/serial_port_registry.h`),
takes `TIOCEXCL`, applies the line settings and keeps the fd **non-blocking**; `close()` releases all three.
Rules ([decision 0017](../../docs/decisions/0017-serial-port-ownership.md)):

- **The connected link** opens with `SerialClaimKind::Link`. A port another connected device holds is
  refused with a message that names the holder; put it in the connect refusal.
- **A probe** opens with `SerialClaimKind::Probe` for exactly the probe's duration. Before probing,
  `enumerate_*_ports()` classifies each candidate with `util::serial_candidate_use(path, owner)`: a port held
  by another kind of device is skipped without being opened, and a port held by a connected device of the
  same kind stays in the results at its scan position without being probed, so an auto-detect index still
  names the same device while its sibling is connected. Probes that collide on a port take turns (the claim
  waits, bounded by `kSerialProbeClaimWait`); they never skip each other.
- **One owner name per wrapper**, shared by its probe and its link (`"Gemini focuser"`): it is what the
  sibling test compares and what a refused operator reads.
- **Reads go through `util::tty_read(fd, buf, n, timeout_ms)` and writes through `util::tty_write_all()`.**
  Both are `poll()`-bounded. `VMIN`/`VTIME` do nothing on the non-blocking fd, so a retune cannot park a
  reader, and a bare `read()` returns `EAGAIN` at once. A read timeout also checks the line settings and puts
  back any another program changed. A streaming reader whose link-health latch trips calls
  `util::restore_serial_line_settings(fd)`, since a stream at the wrong baud delivers garbage, not silence.
- **A reply is read with `util::read_until(fd, ReadSpec, deadline)`** (`util/serial_read.h`), not a hand-written
  loop around `tty_read()`; `read_until_socket()` is the same read over a TCP fd. It stops on a terminator set, a
  length or an idle gap, drops CR/LF, caps the length, keeps one `steady_clock` deadline and returns `LinkDead` with
  the errno for a hangup. It never latches, closes, throws, sleeps or flushes: the wrapper keeps its flush, command
  delay and exception text and decides what `LinkDead` means. It is two concrete overloads, not a `ByteChannel`
  (decision 0013 section 7). The rule binds new reads and the ones migrated so far: the Gemini focuser and flat
  panel and the iOptron iEAF and iEFW `read_response()` and ack drains (an ack drain that hears a hangup logs "no
  ack" and does not report a lost link). The iOptron mount, Celestron NexStar, SynScan and ZWO mount loops, the
  QHY CFW3 and Q-Focuser readers and the probe and handshake loops still use their own loops and move in later
  slices (upstream issue #1604). The Sky-Watcher and Celestron CGX loops (decision 0017 rule 9) and the stream
  readers (Wanderer, Gemini PDH) keep their own shape.
- `scripts/check_serial_port_ownership.py` (the `serial-port-ownership` CI job and pre-flight gate 2e2)
  fails on a raw `O_NOCTTY` open, `tcsetattr`, `TIOCEXCL`, a `c_cc[VTIME]`/`c_cc[VMIN]` assignment, a
  pre-#1202 `mark_serial_port_*` call or a raw `read()`/`write()` on a link's fd (`x.fd()`, `x->fd()`,
  `serial_fd_`) anywhere under `AlpacaCore/src` or `AlpacaCore/include` outside the two helper headers.
  Only the Sky-Watcher and Celestron CGX wrappers may use raw `read`/`write` (their `poll()`-bounded loops
  behind test seams); the allowance is per rule, so the other rules still apply to them. `TIOCEXCL` is
  best effort: a refusal logs a warning and the open goes on, unless the link opts in to
  `SerialExclusive::Required` (decision 0017). The `write_all`/`clear_nonblocking` advice below now applies to sockets and to
  non-tty fds only.
- A by-id name does not identify a CH340 device (they carry no serial number): the connect-time handshake
  is what proves which device is on a port, so every serial driver must check its handshake reply.

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

On a serial port these apply only through `SerialLink`'s own helpers: a tty fd is
non-blocking, so its reads go through `util::tty_read()` and its writes through
`util::tty_write_all()`, never a bare `read()`, `write_all()` or `clear_nonblocking()`.
`write_all`, `send_all` and the `fcntl` helpers remain the rule for sockets and other
fds. The non-blocking, `poll()`-bounded shape started as the Sky-Watcher driver's
exception (some USB CDC-ACM ports ignore `VMIN`/`VTIME`, and a blocking `read()` on a
board that went quiet parked forever in `n_tty_read`); #1202 made it the rule for every
serial driver, probes included. Two links keep their own `poll()`-bounded loops on the
same non-blocking fd: Sky-Watcher, because they are the path through its
`serial_read_`/`serial_write_` test seams, and the Celestron CGX AUX link (mount and
focuser), whose `read()`/`write()` run only after its own `poll()` on the exchange deadline
([decision 0017](../../docs/decisions/0017-serial-port-ownership.md) rule 5). Neither
restores retuned line settings on a timeout the way `tty_read()` does; the CGX link opens
with `SerialExclusive::Required`, so no other program can open its port to retune it.

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
  vanished serial or HID node (`util::device_node_missing()`), a serial port
  another connected device now holds (#1202: after a replug or a change of plug
  order the remembered name can belong to another CH340 device; serial drivers
  check both with `util::serial_endpoint_stale()`), a refused network
  connect, or a failed identity gate (SynScan's echo test). Every other
  exception propagates: a scan DTR-resets every CP210x / CH340 device on the
  box, so a wheel still homing or a handshake that missed once must never start
  one (review of #660). The scan's own message propagates as the connect
  refusal (#358), so it reaches the client and `LastConnectError`. **The first
  connect after a service restart pays the scan** (5.5 s for the iOptron Wi-Fi
  sweep, up to two boots for the CFW3 probe) inside the Platform 7 `Connecting`
  window, unless the driver remembers its location (next section), so before a
  ConformU run against a freshly restarted service connect the device once from
  the web UI, or give it an explicit port or host. A
  connect that finds another device's probe on its serial port waits for that
  probe too (up to `util::kSerialProbeClaimWait`, sized for the CFW3 probe,
  #1202), which can likewise outlast the 5 s window.
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

### Remembered auto-detect location (`util/device_location_store.h`)

Issue #1452: a reconnect that runs the full search again (about 8 s for a
Gemini hub, 5-13 s for a Gemini flat panel, 2-5 s for an iOptron Wi-Fi sweep)
misses the client's 5 s `Connect()` budget. A driver that adopts the store
(today the Gemini PDH, the three Gemini flat panel models and the iOptron
mount, serial and network) follows these rules:

- **The store is `util::DeviceLocationStore`**: one flat JSON object of strings
  in `config/device_locations.json`, runtime state kept apart from
  `registered_devices.json` (the [decision 0008](../../docs/decisions/0008-rotator-sync-offset-persistence.md)
  pattern: temp file plus rename, other keys kept, a missing or corrupt file
  means no remembered location, a failed save logs a WARNING and never fails
  the connect). No getter touches it ([decision 0016](../../docs/decisions/0016-getters-never-wait-on-device-io.md)).
- **The key is `util::location_key(get_unique_id(), index)`, `<id>#<index>`**:
  the persisted unique ID ([decision 0018](../../docs/decisions/0018-persisted-unique-id.md))
  plus the enumeration index. The ID survives an edit of the entry, so without
  the index a renumbered entry would take the old index's unit.
- **The value is the location**: the `/dev/serial/by-id/...` path when one
  resolves to the port (`util::serial_location_for()`, it survives a replug
  under another `ttyUSBn`), else the raw node; `host:port` for a network device.
- **Connect tries the remembered location once, then searches**: a serial
  driver calls `util::recall_serial_location()`, which runs the device's own
  identity handshake on that one port through a `SerialLink` probe claim and
  never probes a port a connected driver holds ([decision 0017](../../docs/decisions/0017-serial-port-ownership.md)).
  Any failure runs the full search as before. Save only after the connect
  succeeded and only for an auto-detected device: a configured port or host
  never reads or writes the store.
- **Same-model units**: a handshake that returns a model code only cannot tell
  two units apart, so the recall relies on the same index order as the full
  search; the vendor file says what its handshake carries.
- Tests point the store at a temp directory with `TempLocationDir`
  (`tests/device_location_test_dir.h`) in every case that connects an
  auto-detected driver, so no test writes `config/device_locations.json` in
  the build tree.

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
