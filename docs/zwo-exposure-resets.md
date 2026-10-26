# ZWO exposure-start USB resets: issue #1665

## Result and scope

On 2026-10-26 UTC, the OPi6 / ASI6200MM Pro reproduced an SDK-requested USB
reset at the beginning of every exposure in the SDK-only diagnostic sequence.
The reset comes from acquisition-worker startup, before its exposure-start
message, not from a dropped-frame recovery branch in these captures.

This identifies the reset's immediate source on this rig. It does **not** explain
why a reset can fail behind the original VIA VL817 hub, establish ZWO's reason
for including the startup reset, or demonstrate that suppressing it is safe.
No production driver or SDK behavior was changed.

Issue: <https://github.com/open-astro/AlpacaBridge/issues/1665>.
Camera-removal recovery is tracked separately in
[#1667](https://github.com/open-astro/AlpacaBridge/issues/1667); the former
umbrella issue #1500 is closed.

## Baseline and preservation

- Branch: `fix/issue-1665-zwo-exposure-resets`, created in a separate worktree
  so the existing uncommitted QHY work was not disturbed.
- Base: `956c5d5c3da90e52cbd92c20aecac38a12cff557`. A fresh fetch and GitHub
  `refs/heads/main` agreed before work began. This is newer than the SHA in the
  saved plan.
- The OPi6 source was exported from that commit. SHA-256 verification of every
  tracked file against the local checkout passed before building.
- Native `aarch64`, 12 cores; build parallelism explicitly limited to 10.
- Kernel: `6.18.45-current-arm64`, build `#1 SMP Wed Aug 19 16:18:21 UTC 2026`.
- ASI6200MM Pro, full-frame 9576 × 6388, bin 1, RAW16, gain 200, offset 1.
- USB path `10-1.4`, 5000 Mbit/s, behind the Genesys Logic `05e3:0626` hub.
- SDK reports `1, 41, 0, 0`.
- The installed executable, configuration and both users' SDK settings were
  archived before probing. Each hardware session stopped the installed service,
  checked the camera USB node for another owner, and restarted the service on
  exit. Probes ran as `astro`, with separate working directories and `HOME`
  directories containing copied SDK settings. No replacement package, executable
  or sudoers rule was installed.

The ASI6200's bandwidth capabilities under this SDK were minimum 40, maximum
100, SDK default 50. The copied settings initially read back 100, automatic;
the diagnostic applies 40, manual, matching AlpacaBridge's default policy.
These are **ASI6200** figures, not the still-requested ASI2600 figures.

### Artifact hashes

| Artifact | SHA-256 |
| --- | --- |
| Bundled arm64 static ASI SDK | `55acfba4f8f2532481dacf6bc43f0db8c56c872324efc693e181c2ddd13369f6` |
| Bundled arm64 shared ASI SDK 1.41, inspected only | `3ecf511979ed571131e7d7f4a467112aba21940b4dc91d63bd109b9683bf67c4` |
| Unmodified-source baseline server | `653053101cc1b75577e801e46c0da98763a9727e151aae33594b684651bdeab5` |
| Final SDK diagnostic executable | `0ba1e2221cbad42c4f5abbc4dc18f32260b235354b4a77703d920d1a60c3196c` |
| Final SDK diagnostic source | `45c9e6c984569fac45a500c25e108627f5fe0869c38951c39066dd08143ab565` |

The first one-second SDK sequence preceded a diagnostic-only addition of
geometry checks, pixel statistics and per-frame cooling readback. Its executable
was rebuilt before the remaining sequences; the final executable hash above
applies to the minimum, 20-second and 60-second sequences.

## Debug ABI and tracing

The public bundled header does not declare the debug exports. Context7 and
public-source searches did not supply authoritative declarations. The temporary
diagnostic therefore used the following **version-specific recovered ABI**,
checked against the actual arm64 implementation, not guessed from export names:

| Export | Observed arguments and behavior |
| --- | --- |
| `ASIEnableDebugLog` | Camera ID and a 32-bit `ASI_BOOL`; changes a per-camera byte and a global debug flag; returns an ASI error code. |
| `ASIGetDebugLogIsEnabled` | Camera ID and pointer to a 32-bit `ASI_BOOL`; returns an ASI error code. |
| `ASIGetDebugLogPath` | Camera ID and output buffer; the internal getter performs a fixed 256-byte `memcpy`, with no size argument. |

The probe reads the old debug state, enables logging, verifies it, allocates a
zero-initialized **257-byte** path buffer, and keeps logging enabled through
status polling and download. Cleanup restores the prior debug state. The extra
byte makes printing bounded even if the copied 256 bytes lack a terminator.
This is not a vendor-supported public contract and should not be promoted into
the production wrapper without confirmation from ZWO.

The diagnostic links the same static SDK as production, with
`-Wl,--wrap=libusb_reset_device -rdynamic -no-pie`. The wrapper records a UTC
epoch timestamp and stack, calls `__real_libusb_reset_device` unchanged, and
records its result. **It never bypasses or fabricates a reset.** Kernel journal
timestamps provide an independent match. No debugger, kernel module, USB
rebinding or firmware update was required.

## Observed call path

For the final diagnostic executable, the minimum-duration sequence records:

```text
1792986412.572299 frame=0 trigger exposure_us=32
1792986412.572582 RESET requested
__wrap_libusb_reset_device                  0x46cf10
CCameraFX3::ResetDevice()+0x30             0x6372e4
WorkingFunc(bool*, void*)+0x5c             0x539c9c
MyThr(void*)                              0x627cb8
1792986412.666239 RESET returned 0
```

Matching kernel entry:

```text
2026-10-26T03:46:52.647468+00:00 usb 10-1.4: reset SuperSpeed USB device number 4 using xhci-hcd
```

Symbolization and disassembly place the caller in the ASI6200MM Pro acquisition
worker's startup path. In the inspected shared SDK, the corresponding worker
starts at `0x177588` and calls `CCameraFX3::ResetDevice()` at `0x1775e0`;
`ResetDevice()` calls `libusb_reset_device` at `0x274c20`. The static executable
has the same startup call sequence, and its runtime backtrace reaches it.

With debug enabled, the one-second captures show this order:

```text
StartCapture: start capture create thread
RESET requested / RESET returned 0
ResetDevice: ResetDevice
620B::WorkingFunc: working thread begin!
620B::WorkingFunc: Begin long exp 1000
620B::WorkingFunc: Exp start
620B::WorkingFunc: Exp end
620B::WorkingFunc: Long exp:Get one Frame 1000
620B::WorkingFunc: snap: EXP_SUCCESS
620B::WorkingFunc: working thread exit!
```

No application exposure retry is present in the SDK-only probe. The reset is
before the worker's frame-recovery logic, so SDK strings such as “no frame more
than 5, reset!” are not the cause of these startup resets. This observation
does not exclude additional recovery resets in a failing acquisition.

## SDK-only matrix

| Requested exposure | Frames | Successful downloads | Wrapped reset calls | Matching kernel resets |
| --- | ---: | ---: | ---: | ---: |
| SDK minimum, 32 microseconds | 3 | 3 | 3 | 3 |
| 1 second | 3 | 3 | 3 | 3 |
| 20 seconds | 2 | 2 | 2 | 2 |
| 60 seconds | 1 | 1 | 1 | 1 |

All nine reset calls returned 0. Each download returned SDK success with a
122,342,976-byte destination buffer. The later six captures also checked
effective full-frame RAW16 geometry and recorded pixel minimum, maximum and
mean; those sanity checks are not proof that the SDK wrote every buffer byte.
`ASIGetDataAfterExp` does not report actual received length.

Before the 60-second trigger, target temperature read back 0 °C and CoolerOn
read back true. Actual temperature was 28.6 °C at trigger and 21.0 °C after
download: cooling was enabled, **not yet equilibrated**. Cleanup restored the
previous target and cooler state; both restoration calls returned success.
No camera disappeared during these short sequences.

## AlpacaBridge comparison, SDK diagnostics disabled

The freshly built, unmodified-source baseline server ran from an isolated
directory on TCP port 6816, discovery disabled, with 10 HTTP workers. This run
used the real HTTP → camera driver → SDK wrapper path, not the SDK-only probe.
There was no reset wrapper linked into the server. Its copied SDK settings
retained `DebugPrint=00`, and no debug export was enabled in this process.

| Requested exposure | Client starts | Successful ImageBytes downloads | Matching kernel resets |
| --- | ---: | ---: | ---: |
| Minimum, 32 microseconds | 1 | 1 | 1 |
| 1 second | 3 | 3 | 3 |
| 20 seconds | 1 | 1 | 1 |
| 60 seconds | 1 | 1 | 1 |

Each response had ImageBytes metadata version 1, ErrorNumber 0, rank 2,
dimensions 9576 × 6388, Int32 source elements, UInt16 transmission elements,
and exactly 122,342,976 image-data bytes. LastExposureDuration matched the
requested duration for each capture. This validates the complete HTTP payload
length, not the SDK's unreported byte count or sensor-start timestamp accuracy.

The six kernel reset timestamps were `03:50:19.407396`, `03:50:21.659401`,
`03:50:24.383414`, `03:50:27.127396`, `03:50:29.799503` and
`03:50:51.643487` UTC on 2026-10-26, each on `10-1.4` with device number 4.
The client issued its six starts at approximately `03:50:19.329538`,
`03:50:21.577043`, `03:50:24.302832`, `03:50:27.050081`, `03:50:29.713450`
and `03:50:51.564431`, respectively.

Before the 60-second start, CoolerOn read true and the target read 0 °C; actual
temperature was 26.5 °C then and 19.8 °C after download. The client restored its
previous gain, offset, target and cooler state, disconnected, and the test server
exited before the installed service was restarted. The service was verified
active afterward. The installed executable and both original SDK settings files
retained their pre-test SHA-256 values. The installed device configuration also
matched the private pre-test archive byte for byte by SHA-256 comparison.

An initial client preflight incorrectly expected a format name in ReadoutModes;
this camera exposes `Normal` / `High Speed` there. That client stopped before
any exposure and cleaned up; its logs are retained separately as
`http-preflight-*.log`. The corrected client checks the camera's 16-bit range
and the actual ImageBytes metadata instead. This was a diagnostic-client error,
not a driver failure.

The combined short matrix is **15 successful downloads / 15 matching kernel
resets**, with no disappearance. Reproducing the reset both with and without
SDK debug/tracing makes a logging-only timing artifact unlikely; it does not
qualify the original rig or establish reliability over a long soak.

## Tests and remaining work

- Native RelWithDebInfo baseline build passed, using 10 jobs.
- Existing `[zwo][camera]` tests passed: 84 cases, 845 assertions. This is fake
  evidence about the existing driver, not proof about SDK reset behavior.
- The temporary acquisition client checks ImageBytes metadata and exact HTTP
  payload length; its parser self-check rejects short and error headers.
- A separate evidence cross-check passed for all 15 downloads, wrapped/kernel
  reset counts, HTTP request/reset timestamp pairing, cooler readback before
  long exposures, HTTP payload sizes and reported durations.
- Documentation drift, instruction structure, changelog-fragment validation
  and `git diff --check` passed. No full all-vendor or sanitizer run was made
  for this documentation-only change.

The observed reset reproduces without AlpacaBridge, so there is no evidence for
changing driver call ordering to remove it. Do not silently intercept the reset,
patch SDK bytes or switch acquisition modes as an assumed cure. The next
correction needs a vendor-supported setting or SDK change and a controlled
baseline/candidate comparison.

Still required:

1. Give ZWO the minimal static-SDK reproducer, stack and correlated kernel/SDK
   traces; ask whether the startup reset is required and whether a supported
   alternative exists. The vendor's rationale is unknown.
2. Re-run the original ASI2600MM Pro / CM4 / Renesas / VIA VL817 rig on a build
   containing #1657, beyond the reported approximately 25-minute onset.
3. Record that ASI2600's bandwidth minimum, maximum, SDK default and applied
   value/auto mode; ASI6200 capabilities cannot fill in this task.
4. Once a supported correction exists, exercise initial and retry triggers,
   abort/stop, failure, reconnect and teardown; run the planned direct/hub
   baseline/candidate soak and ConformU 4.5.3 on arm64.

No direct-port re-cabling, original-rig run, failure injection, candidate patch,
140-frame comparison or fresh ConformU run is claimed here. There is no candidate
production behavior change to qualify yet.

## Local evidence

Full diagnostics, probe sources, build/test logs and the source hash manifest
are retained on the OPi6 under `/home/astro/alpaca-validation/issue-1665`.
`sdk-1s-3frames.log`, `sdk-0s-3frames.log`, `sdk-20s-2frames.log` and
`sdk-60s-1frames.log` pair with the respective `kernel-sdk-*.log` files.
`http-client.log`, `http-server.log` and `kernel-http.log` contain the
diagnostics-disabled AlpacaBridge comparison; the client source is
`http-probe.py` (SHA-256
`49d60c6bb4203515847fe90d4f488df2b5a761a80a932b0cb0d2d3ae1f87fd97`).
SDK-generated diagnostics are in `sdk-home/.ZWO/asicamerasdk/asicamerasdk.log`.
`pre-test.tar` is a private restoration archive, not a support attachment.

SDK source compilation uses the bundled `ASICamera2.h` and arm64
`libASICamera2.a`, system `libusb-1.0`, pthread and dl; it does not use the
shared SDK. The probe pins the intended camera identity, runs one acquisition
at a time, snapshots controls/geometry and restores them before close.
Treat the temporary sources as diagnostic tooling, not production API examples.

The temporary probe sources and full logs are local evidence, not files shipped
in this documentation change. To rebuild that SDK probe source on arm64, set
`sdk` to the repository's `AlpacaCore/external/ZWO/ASI_Camera_SDK` directory:

```sh
g++ -std=c++20 -O0 -g -no-pie -rdynamic -I"$sdk/include" sdk-probe.cpp \
    "$sdk/lib/linux/armv8/libASICamera2.a" -lusb-1.0 -pthread -ldl \
    -Wl,--wrap=libusb_reset_device -o sdk-probe
```

Before running it, arrange exclusive camera ownership, preserve the original
service/configuration/settings, use an isolated `HOME` with copied SDK settings,
and arrange service restoration on exit. It accepts duration in seconds and
frame count; `0` selects the SDK minimum. The source pins the unit used here;
recheck the debug ABI and camera identity before adapting it to another rig.
