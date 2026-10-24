---
applyTo: "AlpacaCore/src/vendors/gphoto/**,AlpacaCore/include/alpacacore/vendor/gphoto/**,AlpacaCore/tests/*gphoto*,AlpacaCore/conformu/GPhoto/**"
---

### GPhoto (DSLR/mirrorless cameras — Canon, Nikon, Sony via libgphoto2)

Devices: Camera.

**STATUS: ConformU-validated against real bodies: Nikon D5300, D3200 and D3300, and Canon EOS 4000D, 70D, 250D and 600D.** Originally built
from libgphoto2/libraw API documentation and source reading, plus the reference indi-gphoto
driver (`indilib/indi-3rdparty`) for protocol shape, with no physical DSLR available in the
session that added it (issue #241). A later session with hardware access ran `/deploy-test` +
`/conformu` against both bodies on the same rig and device slot (swapped with no reconfiguration)
and fixed what that surfaced: `gp_camera_autodetect()`'s return-value contract, the Gain-mode
ASCOM contract, a `StartExposure` ROI bounds check, and the `PixelSizeX`/`PixelSizeY` lookup table
described below; a third session validated the D3300 on the same slot with no code change.
The Canon EOS 4000D report came from a user's Raspberry Pi 5, not that rig (issue #611), also with no
code change, and the committed report is that user's re-run for the Canon bulb change on a 5.0.0 nightly built from main (issue #1063, ConformU 4.5.1, 2026-10-12); the Canon EOS 70D report (issue #637) and the Canon EOS 250D report came from a second
Raspberry Pi 5, again with no code change; both were re-run on that Pi for the Canon bulb change (issue #640, ConformU 4.5.1, 2026-10-09), and the committed reports are those runs. The Canon EOS 600D report (issue #1077) came from a user's Raspberry Pi 5 running AlpacaBridge 4.2.0, which has no Canon bulb path, again with no code change (ConformU 4.5.2-rc.1, 2026-10-17). The 70D works on mode dial B (its shutter-speed list holds only `bulb`; every exposure takes the Canon bulb path, issue #640), and a Raspberry Pi 3 is too
slow for it under ConformU: libraw's `unpack()` of its 20 MP CR2 takes about 4 s there, the frame stays
`Exposing` for about 8 s, and ConformU's `StartExposure` wait gives up. The Canon EOS 250D (the EOS 200D II in Asia) needs the
lens on MF: with AF its priming capture and every exposure fail with "Unspecified error". Every run is clean (0 errors, 0 issues, 0 timing violations); see
`SUPPORTED-DRIVERS.md` and `AlpacaCore/conformu/GPhoto/`. Bulb-mode capture was validated on the
D3300 in a fourth session (issue #569: 60 s and 300 s frames through the Alpaca API), which also
found and fixed the driver's first real bulb defect -- see the bulb bullet below and
`docs/failures/0009-gphoto-nikon-bulb-full-config-walk.md`. Coverage beyond these specific
bodies (other Canon/Nikon/Sony models, the SDK's other transports) is still only as validated as
the notes below say for each.

SDK: **libgphoto2 is bundled**, `libraw` is a system package (`libraw-dev` via pkg-config). The
libgphoto2 2.5.34 source tarball is vendored in `AlpacaCore/external/libgphoto2/` (pins and bump
procedure in its README); `scripts/build_bundled_gphoto.sh` builds it into the private prefix
`/usr/lib/alpacabridge/libgphoto2` (library, the default `standard` camlibs, iolibs and the
generated `61-alpacabridge-libgphoto2.rules`), and `AlpacaCore/src/vendors/gphoto/CMakeLists.txt`
links that copy through `ALPACACORE_GPHOTO_PREFIX` / `ALPACACORE_GPHOTO_STAGE`. The distro's
libgphoto2 is never linked or loaded: the executables carry a `DT_RPATH` (not `DT_RUNPATH`) to the
prefix, and the prefix is never added to `/etc/ld.so.conf.d/`, so other programs on the host keep the
distro library. `scripts/check_bundled_gphoto.sh` is the acceptance check (CI job `package-deb` on every PR, and the nightly build job before it publishes;
on the rig add `--pid` of the service). A dev build runs the script once before CMake. Decision:
[0012](../../docs/decisions/0012-bundled-libgphoto2.md). Unlike the proprietary camera SDKs there is
no prebuilt binary under `external/`, so Step 4's SDK cleanup checklist does not apply.

- **Enumeration is USB-autodetect-by-index, matching the other SDK-enumerated cameras**: no
  serial/network auto-detection was implemented (per the driver-build guide's "SDK-enumerated
  devices" category) — `cameraIndex` indexes into `gp_camera_autodetect()`'s result list, same
  contract as ZWO/QHY/SVBONY/PlayerOne/ToupTek's `cameraIndex`. `open_camera()` then does an
  explicit model+port `gp_abilities_list_lookup_model` / `gp_port_info_list_lookup_path` init
  (mirroring `indi-gphoto`'s default path) rather than a bare `gp_camera_init` auto-probe, so
  opening one detected camera can't race another USB-attached camera's enumeration.
- **Sensor geometry is resolved at Connect time via a per-model priming capture + on-disk
  cache** — libgphoto2 has no "sensor size" query; `CameraXSize`/`CameraYSize`/`BayerOffsetX`/
  `BayerOffsetY`/`MaxADU` are all derived from decoding an actual captured RAW frame with libraw
  (`LibRaw::imgdata.sizes`, `LibRaw::COLOR(row,col)` + `cdesc` for Bayer phase,
  `imgdata.color.maximum` for MaxADU). ConformU hardware validation against a real Nikon D5300
  confirmed this needed a fix — the ASCOM Camera contract expects these properties readable
  before any exposure — and also confirmed the caution below was justified: the D5300's real RAW
  crop libraw decodes is **6016×4016**, not the 6000×4000 public spec (an 8px masked/calibration
  border per edge that no libgphoto2 widget or PTP `ObjectInfo` metadata reports — confirmed by
  probing both directly). So `set_connected` no longer waits for the caller's first exposure:
  on Connect it checks an on-disk cache (`config/gphoto_sensor_cache.tsv`, a tiny tab-separated
  flat file — AlpacaCore has no JSON, see `alpaca_json.h`) keyed by camera **model** string; a hit
  populates geometry instantly with no capture. A miss (this exact model has never connected on
  this rig before) triggers one throwaway capture at the fastest native shutter speed off
  `mutex_`, decodes it, populates geometry, and writes the cache entry — so the cost (one shutter
  actuation, one ~4-6s download+decode for a 24MP frame) is paid at most once ever per model, not
  per Connect, not per reboot, not per additional camera of the same model. Best-effort: if
  priming fails for any reason, geometry simply falls back to the pre-fix behavior (unknown,
  `InvalidOperation`, until the caller's own first real exposure) rather than failing Connect.
  See `prime_sensor_geometry_and_cache`/`set_geometry_locked`/the cache helpers in
  `gphoto_camera_driver.cpp`.
- **`connected_` is published only after the priming capture finishes, never before it** (review
  PR #485) — the first version of this code set `connected_.store(true)` before starting the
  priming capture, then unlocked `mutex_` for the capture's multi-second duration. That looked
  harmless (priming is best-effort and `Connecting` stays true the whole time) but broke
  `AsyncConnectable`'s `record_disconnect_if_connect_in_flight()` / `consume_pending_disconnect()`
  contract (see `async_connectable.h`), which assumes `connected_==true` means the connect task's
  real work is already done: a `set_connected(false)` racing in during priming saw `connected_`
  already true and proceeded straight to `sdk.close_camera()` on the handle priming was still
  using, and a racing `start_exposure()` passed `ensure_connected()` and ran a second
  `gp_camera_capture()` on that same handle concurrently — both genuine use-after-close /
  concurrent-SDK-call bugs, not theoretical ones. Publishing `connected_` only after geometry is
  resolved (both the cache-hit and priming branches) closes both for the ASYNC `connect()`/
  `disconnect()` entry points, which is what `conn_task_` tracks. **`set_connected()` is also a
  public sync entry point in its own right** (the project's own `[stress]` harness calls it
  directly, bypassing `conn_task_` entirely, which then stays `kConnIdle` for the whole priming
  window) — round 2 of the same review added `connecting_priming_`, mirroring ToupTek AFW's
  `connecting_homing_`: a racing sync disconnect calls `record_pending_disconnect()` instead of
  being dropped as falsely idempotent (`connected_` is still `false` on both sides), a racing sync
  connect no-ops instead of double-opening and leaking the first handle, and this call consumes
  the pending flag after re-locking post-priming, closing the handle instead of publishing
  `connected_` if a disconnect was recorded. `ensure_connected()` correctly rejects any
  operational call for the whole window either way. If you touch this function again,
  `connected_.store(true)` has to stay the *last* thing that happens on the connect path, and any
  new `mutex_`-released window needs its own `connecting_*` flag the same way — the async task
  gate alone never covers a sync caller.
- **`set_gain()` holds `mutex_` across the SDK call via `with_handle()`, not just for the handle
  copy** — the same class of bug as above, on a different path: the original code read `handle_`
  under a lock via a `handle_value()` helper, released the lock, then called
  `GPhotoSDKWrapper::set_choice_value()` unlocked. A concurrent `set_connected(false)` could close
  the handle in that window. `with_handle()` (mirroring ToupTek's helper of the same name) takes
  `mutex_` for the whole validate-then-call sequence; `handle_value()` no longer exists; if a
  future property setter needs the SDK handle, go through `with_handle()`, not a bare handle copy.
- **`with_handle()`/`mutex_` alone does not serialize against the exposure worker thread** (review
  PR #485 round 3) — `run_exposure()` (the `exposure_thread_` body) makes its libgphoto2 calls
  (`gp_camera_capture()`/bulb loop) *without* holding `mutex_`, and `exposure_active_` is cleared
  to `false` *before* the thread has actually left the SDK in two places: `stop_exposure()` stores
  `false` then joins, and the `get_camera_state()` watchdog force-clears it with no join at all
  (a truly wedged capture has no cancel primitive in libgphoto2, so that one stays a documented,
  accepted gap). A setter like `set_gain()` that only checked `exposure_active_.load()` under
  `with_handle()`'s `mutex_` could still slip an SDK call in during the narrow window after
  `stop_exposure()` clears the flag but before `exposure_thread_.join()` returns, corrupting the
  shared PTP session. Fix: `set_gain()` now takes `exposure_lifecycle_mutex_` (same mutex
  `start_exposure()`/`stop_exposure()` hold for their setup-or-join duration, same lock order as
  everywhere else in this class — lifecycle mutex first, then `mutex_`) before its `with_handle()`
  call, so it blocks until any in-flight `exposure_thread_` has actually been joined. Any future
  SDK-touching setter needs the same `exposure_lifecycle_mutex_` guard, not just `with_handle()`.
- **`fallback_unique_id()` keys on `device_number_`, not the libgphoto2 USB port string** (review PR
  #485 round 4). Since [decision 0018](../../docs/decisions/0018-persisted-unique-id.md) the
  `UniqueID` a client sees is the ID the router stores for the configured device; this fallback
  answers only for a driver a unit test builds directly. The reasoning still holds for it:
  `camera_info_.port` (e.g. `usb:001,005`) looks like a natural per-camera identity, but it isn't stable: it changes across every unplug/replug (the bus device number
  increments) and even within one session, since a DSLR that was powered off when
  `preload_camera_info_locked()` ran and switched on later re-enumerates with a new port. Unlike
  every other camera vendor here, libgphoto2/PTP exposes no serial number to fall back to, so
  `device_number_` (the config-assigned Alpaca device slot, fixed for the life of the process) is
  the only identifier that doesn't move under the camera's own power state — use it unconditionally
  rather than preferring the port string when camera info happens to be cached.
- **`PixelSizeX`/`PixelSizeY` come from a static per-model lookup table** — libgphoto2 exposes no
  pixel-pitch query, and unlike sensor geometry above, pixel pitch in microns isn't recoverable
  from decoding a RAW frame either (libraw doesn't expose it), so this is the one geometry-like
  property that can't be learned from the camera itself. `known_pixel_size_um_table()` in
  `gphoto_camera_driver.cpp` covers ~140 interchangeable-lens Nikon and Canon bodies (Canon's
  Rebel/Kiss/EOS-number regional rebrand names included as separate keys with identical values,
  since libgphoto2 reports whichever name matches the camera's actual USB product ID). A model not
  in the table — every fixed-lens compact/camcorder libgphoto2 also supports, or a body released
  after the table was last updated — still reports `0.0` (ASCOM "unknown") rather than a guess.
  Nikon D5300 confirmed against ConformU: 3.91 microns; Nikon D3200: 3.86 microns; Nikon D3300: 3.92 microns; Canon EOS 4000D: 4.3 microns; Canon EOS 70D: 4.1 microns; Canon EOS 250D: 3.72 microns; Canon EOS 600D: 4.3 microns.
- **ISO is a discrete `Gains()` list, not a continuous register** — deliberate departure from
  every other camera driver here (ZWO/QHY/SVBONY/PlayerOne/ToupTek all throw
  `PropertyNotImplemented` for `get_gains()` and treat `Gain` as a raw numeric register). A DSLR's
  ISO is fundamentally a fixed choice list (the "iso" libgphoto2 widget's `choices`), so `Gain` is
  the index into `Gains()` (the ISO value strings themselves), matching the ASCOM convention for
  named/discrete gain lists. If a future non-ISO-list camera type is added to this vendor, don't
  reflexively copy this shape — it exists because ISO specifically is a fixed list on this
  hardware class.
- **Offset is unsupported** (`PropertyNotImplemented`/`NotImplemented`, unconditionally, no
  `ensure_connected()` gate) — DSLRs have no analog-offset register concept over PTP.
- **Bulb capture has two mechanisms: the standalone `"bulb"` toggle widget (Nikon) or, on Canon
  bodies without it, the `eosremoterelease` press/release pair (`Press Full MF` / `Release` on the bundled libgphoto2 2.5.34, `Press Full` / `Release Full` before it) plus the
  shutter-speed `"bulb"` choice (see the Canon paragraph below).** The toggle path (confirmed present in
  libgphoto2 2.5.31's `ptp2.so` camlib via `strings`, which is the single camlib handling
  Canon/Nikon/Sony PTP — not a per-vendor code branch, so this should generalize across brands):
  set the shutter-speed widget to its `"bulb"` choice if the choice list has one, flip `"bulb"`
  toggle on, hold for the requested duration by pumping the camera's event queue in 100 ms slices
  (`GPhotoSDK::drain_events`, a driver-owned abortable loop so `StopExposure`/`AbortExposure` can
  close the shutter early), flip `"bulb"` off, then poll for `GP_EVENT_FILE_ADDED` in 1 s slices
  (`GPhotoSDK::poll_bulb_file_and_download`) for up to `duration + 30 s` and download.
  **Validated on the Nikon D3300 (issue #569): 60 s and 300 s frames through the Alpaca API.**
  Three things that first run taught, all now load-bearing:
  - **Every widget read/write goes through libgphoto2's single-config API**
    (`gp_camera_get_single_config` / `gp_camera_set_single_config`), never the full tree
    (`gp_camera_get_config` + `gp_camera_set_config`). The full walk reads dozens of PTP
    properties; on a busy Nikon body (mid-bulb, or after a capture it refused) it fails or comes
    back truncated, which is how the shutter-close toggle reported "Unspecified error", left the
    capture unterminated and hung the camera's PTP stack until a power cycle, and how a later
    session saw "Widget not present: shutterspeed2" for a widget `gphoto2 --get-config` showed
    plainly. The gphoto2 CLI takes the single-widget path and the identical bulb sequence
    succeeded every time on the same body. Record:
    `docs/failures/0009-gphoto-nikon-bulb-full-config-walk.md`.
  - **The hold pumps events rather than sleeping**, the way `gphoto2 --set-config bulb=1
    --wait-event=<n>s --set-config bulb=0` does; a file-added event seen during the hold cannot be
    this exposure's (the shutter is open) and is deleted from the camera and dropped rather than
    handed to the next poll as a fresh frame.
  - **The frame wait scales with the exposure.** With the body's long-exposure noise reduction on,
    the file is posted a full exposure-length after the close (the dark frame), so a fixed 15 s
    wait lost every long frame; the watchdog deadline in `start_exposure` includes that window
    for a bulb capture (and for a mirror lock-up exposure). Tell users to turn "Long exposure NR" **Off** for astro use regardless:
    it doubles the time to every frame and the dark is better taken separately. Also from the
    bench: mode dial on M with the shutter speed on Bulb, and the lens/body on MF (with AF the
    body refuses to fire, which is also why a priming capture can fail "Unspecified error" on a
    first connect).
  **Canon bodies (no standalone `"bulb"` widget, issue #640): rig-verified on the EOS 250D (M, shutter Bulb) and the EOS 70D (mode dial B), ConformU 4.5.1 arm64, 0 errors, 0 issues, on libgphoto2 2.5.31 and again on the bundled 2.5.34.**
  When the shutter-speed list has a `"bulb"` choice and the `eosremoterelease` widget lists a
  whole press/release pair (matched by name, never by index), the same
  `bulb_capture_with_abort` loop runs with the press as the open and the release as the close.
  libgphoto2 2.5.34 renamed the choices: the pair is `Press Full MF` / `Release` there and
  `Press Full` / `Release Full` up to 2.5.33, both `remotereleaseon(3, 1)` then
  `remotereleaseoff(3)`. 2.5.34's own `Release Full` is `remotereleaseoff(2)`, which leaves the
  half press held, so the driver picks the pair as a unit and never mixes the two spellings. The
  #569 rules above hold unchanged (events pumped during the hold, release sent on every exit path
  including a throw, abort closes early, frame waited for by file event). `ExposureMax` is 3600 s
  only when the `"bulb"` widget exists or a whole pair was found; a Canon body without one stays
  at its longest native shutter speed.
  **B mode:** with the mode dial on B the shutter-speed list holds only `bulb`, so there is no
  native speed to use and every duration, short ones too, takes the bulb path instead of failing
  with "No shutter speed control exposed by this camera". The shutter-speed widget is not written there: the body owns the shutter and refuses the write ("I/O in progress", EOS 70D), so the press/release pair alone drives the exposure. Bench evidence is in #640 (EOS 250D on M
  with shutter Bulb, EOS 70D on B: 5 s and 35 s frames by hand); the EOS 4000D is user-verified with a 60 s
  frame through NINA and a ConformU re-run (#1063; its camera-side settings are not recorded), and
  other Canon bodies need their own check. Written from libgphoto2's documented behaviour only
  (INDI is LGPL; copy nothing from it).
- **Mirror lock-up (MLU) follows the camera's own menu; the driver never writes it** (issue #1062).
  With MLU on, a Canon body answers `gp_camera_capture` only after a 60 s timeout (EOS 70D and
  250D, bench-proven), so the driver reads the state at exposure start and fires the shutter by hand.
  The state is a single-widget lookup (never a config walk, `docs/failures/0009-gphoto-nikon-bulb-full-config-walk.md`):
  `/main/status/mirrorlock` (70D, 0/1; a range or text widget, read through the wrapper's
  type-aware `get_string_value`), else the read-only `/main/status/mirrorlockstatus` (250D: 0 off, 1
  on, 2 after the first press; any non-zero number is on). A body with neither widget is MLU off and
  keeps today's paths. With MLU on, the sequence is the `eosremoterelease` pair (found by name
  whatever the mode dial says, never by index): Press, Release (mirror up), wait
  `mirrorLockupSettleMs`, Press, Release, then wait for the file-added event and download. In bulb
  the second Press is held for the exposure. The settle runs on the injected `util::TaskClock`
  (ADR 0005) pumping events; an abort cuts it short but still sends the second pair, because a body
  left with the mirror up would fire on the next exposure's first press, and the frame is discarded.
  Never `gp_camera_capture` with MLU on, so the connect priming capture is skipped too (geometry is
  learned from the first exposure). A body that reports MLU on but has no usable pair fails
  `StartExposure` with `InvalidOperation` telling the user to turn mirror lock-up off in the camera
  menu; it never stalls. The watchdog deadline adds the settle and the frame wait. The driver was
  run on a rig with the EOS 70D; the EOS 250D is proven only by the gphoto2 CLI bench probe, and the
  driver is tested on the fake for both. Fields: `mirrorLockupSettleMs` (500 to 10000, default 2000).
- **Long-exposure noise reduction (LENR): `longExposureNR` is `camera` (default, writes nothing), `off` or `on`.**
  For `off`/`on` the driver writes `/main/capturesettings/longexpnr` once at connect, camera idle,
  with a single-widget set, matching the camera's own On/Off text without regard to case (libgphoto2
  translates it). It is never written during an exposure. Nikon's widget comes from the libgphoto2
  source and is tested on the fake only; no Canon body tested shows an LENR widget over gphoto2 (the
  70D and 250D, mode dial M or B), so there the driver logs one WARN, "long-exposure NR is not
  settable on this camera; set it in the camera menu", and connects anyway. A refused write warns
  and does not fail the connect either. Tell Canon users to set LENR in the camera menu.
- **RAW format selection**: at connect, the driver scans the `"imageformat"`/`"imagequality"`
  widget's choices for one containing `raw`/`nef`/`cr2`/`cr3`/`arw` (case-insensitive), preferring
  a pure-RAW choice over a combined RAW+JPEG one, and sets it. If no RAW choice is found, capture
  proceeds anyway (logged as a warning) and will fail at libraw's `open_buffer`/`unpack` step —
  surfaced as a clear `DriverException`, not a silent misdecode.
- **Sensor temperature**: best-effort only, read from libraw's
  `imgdata.makernotes.common.SensorTemperature` after each decoded exposure (populated mainly for
  Canon RAW files per LibRaw's own docs/INDI precedent); throws `PropertyNotImplemented` when
  absent. Do not expect Nikon NEF files to populate this.
- **Exposure metadata timing** (issue #1477): native duration comes from the selected parsed shutter choice. Native UTC is stamped after setup; MLU uses the second press, not mirror settling. Driver-timed bulb duration uses monotonic open/close command boundaries, excluding file wait, LENR and decode. Publish only after accepted decode; connect priming never publishes metadata. Host command boundaries do not establish physical shutter latency, and RAW/EXIF timestamps have no verified UTC/accuracy contract here.
- **No pulse guiding** — a plain USB libgphoto2 camera has no ST-4/autoguider port; `PulseGuide`
  throws `NotImplemented` once connected (matches `CanPulseGuide=false`).
- **`HasShutter=true`** — the one camera vendor in this project where that's actually true (every
  CMOS SDK camera here reports `false`); a DSLR has a real mechanical shutter.
