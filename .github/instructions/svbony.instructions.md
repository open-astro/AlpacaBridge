---
applyTo: "AlpacaCore/src/vendors/svbony/**,AlpacaCore/include/alpacacore/vendor/svbony/**,AlpacaCore/tests/*svbony*,AlpacaCore/conformu/SVBONY/**"
---

### SVBONY

Devices: Camera.

- The camera driver accepts a non-owning `SVBSDK` seam; any injected SDK must outlive the driver and its joined exposure worker. Extend `fake_svbony_sdk.h` and its connected contract/stress coverage when changing acquisition behavior.

SDK location: `AlpacaCore/external/SVBONY/lib/armv8/`, headers under `external/SVBONY/include/`.

- **SC715C is a rebadged ToupTek G3M715C, NOT served by this driver.** The SVBONY SDK does not recognize the SC715C. Configure it with vendor `touptek` (device type Camera, `ALPACACORE_ENABLE_TOUPTEK`) — the ToupTek SDK enumerates it natively under its own model name `G3M715C`. Same rebadge pattern as the iOptron iCAM cameras being served by the Player One driver (see [iOptron notes](ioptron.instructions.md)) -- but with one difference that matters to the user: the router aliases vendor `ioptron` + camera onto the Player One driver, so an iCAM owner still selects `ioptron`. There is no `svbony` + camera alias (that vendor has its own driver), so the SC715C must be configured as `touptek`. Validated 2026-09-11 on Linux arm64: 0 errors, 0 issues, 0 timing issues (ConformU 4.5.1 — see the [ConformU 4.5.0 arm64 timing bug](../../AGENTS.md#target-architecture) note if an earlier ConformU version shows spurious timing failures). Report saved at `AlpacaCore/conformu/SVBONY/SC715C/Linux-arm64.txt`.

- **Control warm-up at connect (SV905C2 quirk)**: After `SVBOpenCamera`, `SVBSetControlValue(SVB_GAIN, ...)` returns `SVB_ERROR_GENERAL_ERROR` indefinitely on SV905C2 — regardless of value, regardless of `bAuto` flag, regardless of whether `SVBStartVideoCapture` is active, and `SVBRestoreDefaultParam` does not clear the state. The driver works around this by iterating every writable control reported by `SVBGetControlCaps` and writing each to its `default_value` during the connect path (after `SVBSetROIFormat` / `SVBSetOutputImageType`). Once any `SVBSetControlValue` call has landed, subsequent client gain writes succeed. Failures during the warm-up are tolerated and logged at DEBUG. Do not remove the warm-up loop in `set_connected` without re-running ConformU against an SV905C2 — the failure is silent until a client tries to set gain. Likely related to SDK readme entries `v1.13.1: Fixup ASCOM software to support SV905C2` and `v1.13.2: Optimize gain settings of SV905C2`.
- **Auto control writes**: `disable_auto_if_needed` reads the current value/auto flag and only writes back if currently auto, since some SVBONY models reject manual writes while auto is active with the same `SVB_ERROR_GENERAL_ERROR`.
- **`SVBSetControlValue` retry**: The wrapper retries up to 3 times with a 50 ms backoff specifically on `SVB_ERROR_GENERAL_ERROR` to absorb genuinely transient hardware-op faults; deterministic rejections still surface after the retries are exhausted.
- **Camera mode**: We use `SVB_MODE_NORMAL` (continuous video) and start/stop `SVBStartVideoCapture` per exposure. INDI's `indi-svbony` driver instead uses `SVB_MODE_TRIG_SOFT` with persistent video capture for stills — keep this in mind if a future SVBONY model needs trigger-mode behavior.
- **Bin/ROI quirks**: divisors width%8, height%2 (see [Camera ROI alignment](../../AGENTS.md#camera-roi-alignment-all-camera-vendors)). ROI updates and `FrameSpeedMode` writes are deferred to `start_exposure` because some SDK control writes take ~1.1 s and would otherwise blow ASCOM client timing budgets.
- **`SVBRestoreDefaultParam`** is called immediately after `SVBOpenCamera` to clear any leftover state from a previous session, mirroring `indi-svbony`. Tolerate failure for older SDK builds that don't export the symbol.
