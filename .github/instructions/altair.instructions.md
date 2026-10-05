---
applyTo: "AlpacaCore/src/vendors/altair/**,AlpacaCore/include/alpacacore/vendor/altair/**,AlpacaCore/tests/*altair*,AlpacaCore/conformu/Altair/**"
---

### Altair Astro

Devices: Camera. Altair Astro cameras are ToupTek OEM hardware, so read the
[ToupTek](touptek.instructions.md) file too: every camera rule there (readout
modes, odd-bin ROI rounding, the thermal poller, abort mechanics, the exposure
guard) applies to an Altair camera unchanged, because it is the same driver.

SDK location: `AlpacaCore/external/Altair/altaircamsdk.20260531/` (altaircam
60.31589.20260531; `inc/altaircam.h`, `linux/arm64/glibc/libaltaircam.so`,
`linux/udev/99-altaircam.rules`, `license.txt`). Only the arm64 glibc subset
of Altair's download is vendored. The SDK ships no static library.

- **License: MIT.** Altair Support confirmed (2026-10) that the SDK may be
  redistributed provided the copyright notice ("Copyright 2026 Altair Astro
  Limited") and the license text are included. `license.txt` holds that text,
  and `debian/copyright` carries the `Files: AlpacaCore/external/Altair/*`
  entry that installs it with the `.deb`. Keep both when the SDK is bumped: a
  new SDK directory needs its `license.txt`, and the `debian/copyright` path
  and year must follow.

- **altaircam is the ToupTek SDK renamed.** Against `toupcam.h` 59.30701 it
  differs only in the `Altaircam_` / `ALTAIRCAM_` prefixes, comments, four GigE
  timeout constants and eight added functions; every function the wrappers call
  has the same signature, every struct the same layout, every constant the same
  value. Recheck that claim when either SDK is bumped: rename the prefixes in a
  copy of `altaircam.h` and diff it against `toupcam.h`.
- **One wrapper implementation for both brands.** `ToupcamFamilySDK<Api>`
  (`AlpacaCore/src/vendors/touptek/toupcam_family_sdk.h`, private to the
  vendor sources: it is not installed) holds every SDK
  interaction; `AltairSDKWrapper` (`altair_sdk_wrapper.cpp`) and
  `ToupTekSDKWrapper` supply only a traits struct generated from the
  `ALPACACORE_TOUPCAM_FAMILY_FUNCTIONS` / `_CONSTANTS` X-macro lists. A
  function or constant the shared wrapper starts using goes into those lists,
  and then both brands fail to compile until each traits struct provides it.
  Never fork the wrapper body into the Altair directory.
- **Handles cross the seam as `HToupcam`.** The Altair traits
  `reinterpret_cast` between `HToupcam` and `HAltaircam` (both opaque pointers
  to a one-int struct the SDK only round-trips). Driver code never sees
  `HAltaircam` and never includes `altaircam.h`.
- **The camera driver is the ToupTek one.** `create_altair_camera()` calls
  `touptek::create_toupcam_family_camera()` with Altair branding
  (`ToupCameraBranding{"Altair", "ALTAIR"}`): name, description, driver info,
  sensor fallback, log tag, connect-refusal text and the `ALTAIR_SN_` /
  `ALTAIR_` unique-id prefix. A camera fix lands in
  `touptek_camera_driver.cpp` and reaches both brands; anything Altair-only
  goes in the branding or in the Altair traits, not in a copied driver. The
  build therefore requires `ALPACACORE_ENABLE_TOUPTEK`, and
  `alpacacore_altair` links `alpacacore_touptek`.
- **Separate singletons, separate open maps.** `AltairSDKWrapper::instance()`
  and `ToupTekSDKWrapper::instance()` each own their library's enumeration and
  their own reference-counted open map. Nothing coordinates opens across the
  two libraries.
- **USB vendor ids.** The ALTAIR178M3 enumerates as `16d0:0d82`. On the dev
  board (2026-10-05) libaltaircam listed it and libtoupcam 59.30701 did not, so
  an Altair camera never appears under the ToupTek vendor. The reverse is
  unconfirmed (TODO: needs a ToupTek camera): `99-altaircam.rules` also grants
  `0547` / `04b4`, ToupTek's ids, so libaltaircam may list a ToupTek camera
  too. Configure each camera under exactly one vendor; the web UI says so
  beside the Altair camera index.
- **OrangePi 3 LTS (Allwinner H6): use a USB 2 port.** On that board's USB 3
  port the ALTAIR178M3 (`16d0:0d82`) loses full frames with no SDK or kernel
  error: `WaitImageV4` just times out. Measured 2026-10-05 against raw
  libaltaircam, driver not involved: at the camera's default speed level 2
  every frame over ~5 MB was lost (4.5 MB still arrived); level 1 delivered
  5-7/10 full frames, level 0 9-10/10, and a 1 Hz temperature poll made no
  difference. On a USB 2 port of the same board (`16d0:0d83`, listed as
  `ALTAIR178M3(USB2.0)`) it delivered 10/10 at every level, 0.81 s per full
  16-bit frame at level 2 (1.58 s at level 0). The driver therefore leaves the
  speed level at the camera's maximum, like ToupTek: lowering it does not make
  that USB 3 port reliable and only slows working hosts. Before suspecting the
  driver when frames go missing, rerun the exposure on a USB 2 port.
- **Device permissions.** Without `99-altaircam.rules` the `16d0` node stays
  root-only: the SDK still enumerates the camera, but opening it fails with
  E_ACCESSDENIED, which the wrapper maps to NotConnected. The `.deb` and both
  install scripts install the rule.
- **Registration is catalog-only.** The `(altair, camera)` pair has no router
  arm and adds no include to `router.cpp`: `altair_schema.cpp` (no vendor
  header, compiled in every build) and `altair_catalog.cpp` (the factory, under
  `ALPACACORE_ENABLE_ALTAIR`) register it in `builtin_catalog.cpp`. The only
  persisted field is `cameraIndex`; the web form submits it as
  `altairCameraIndex`.
- **Tests run over `FakeToupTekSDK`.** `test_altair_camera.cpp` and
  `altair_concurrency_stress.cpp` inject the ToupTek fake through the
  `create_altair_camera(..., ToupTekSDK&)` seam, so no Altair test loads
  libaltaircam or touches USB. The fake camera mirrors the ALTAIR178M3: mono,
  uncooled, ST4.

Validated: ALTAIR178M3 (IMX178, mono, uncooled) on a USB 2 port of the
OrangePi 3 LTS dev board, ConformU 4.5.1 (build 54507) run on the board,
2026-10-05: 0 errors, 0 issues, 0 timing issues
(`AlpacaCore/conformu/Altair/ALTAIR178M3/Linux-arm64.txt`).
