# QHY USB recovery investigation (#1211)

## Branch target

PR [#1340](https://github.com/open-astro/AlpacaBridge/pull/1340) now targets
`main`, as the maintainer requested in the PR. The defect predates the 5.0
cycle, so stable-policy admission is no longer sought. The contribution was
rebased from stable base `18aa5361` onto main `a49b060e`, retaining main's QHY
SDK **26.07.28** and CMake **3.18** minimum. That minimum already selects
`CMP0099` NEW; the explicit policy setting needed by the old 3.15-minimum
stable build is not necessary on main. Both symbol-isolation tests remain.

**Evidence boundary:** the native Pi pre-flight, power-cycle experiments and
ConformU results below were obtained on the stable-based candidate with SDK
**26.06.04.16**. They are historical evidence, not validation of this rebase or
the newer SDK. The original contribution commit was `f94df996`; prior GitHub
CI, including ASan/UBSan and TSan, passed on that head. New checks must run on
the rebased head. The installed rig service is not changed by this rebase.

Main's existing canonical reports are preserved. The exact recovery-run bytes
are shipped separately as
`AlpacaCore/conformu/QHY/miniCam8M/Linux-arm64-usb-recovery.txt` and
`AlpacaCore/conformu/QHY/miniCam8M CFW/Linux-arm64-usb-recovery.txt`.

The following sections are a chronological investigation record, including the
earlier, superseded stable target and checkpoints that were still uncommitted
or unvalidated at the time. Their outstanding-check statements are historical;
the dated/later results supersede them only for the exact builds they name.

### Main rebase checks

Re-ran changed-line clang-format, Black and Flake8, documentation drift,
supplemental-report validation, changelog and Unicode checks, layering,
stress/contract registration, and PR template/Falsified-by checks successfully.
Native Pi changed-line clang-tidy and Cppcheck 2.17.1 also passed against the
rebased sources and new SDK headers. The manual probe's ABI bindings still
match those headers; its 31 fake-only Python tests passed.

A separate native QHY/SVBONY configure with SDK 26.07.28 succeeded. Its build
was explicitly **cancelled at the user's request** to defer arm64 testing to
GitHub CI. No CTest run completed and no native build/test pass is claimed for
the rebased head. The earlier stable-based pre-flight and CI results below
remain evidence for their old heads only. Neither a new physical recovery run
nor a new ConformU run was performed; the installed rig service was untouched.

## Status: diagnostics plus a USB-symbol isolation candidate

[Issue #1211](https://github.com/open-astro/AlpacaBridge/issues/1211) reports a
miniCam8M integrated wheel that could not reconnect after its stationary camera
lost 12 V and disappeared from USB. USB detected it again after power returned;
two wheel connects failed with "No QHY cameras detected" until AlpacaBridge
restarted. The report used official `5.0.0~beta4` on Raspberry Pi Linux arm64.

The mechanism is **unconfirmed**. A stale SDK device list, failed ID lookups,
firmware readiness, and retained camera handles need different remedies. The
first raw-SDK rig test below did not reproduce the failure with all handles
closed. A subsequent installed-driver test reproduced the original failure with
the integrated wheel connected during power loss. A later in-daemon reset
experiment returned success from release/init but did not repair the failure
(see below). ConformU remains outstanding.

This first stage adds:

- Raw SDK scan/init results (unsigned decimal and hex) and timings to DEBUG logs.
- WARNING logs for invalid scan results and failed ID lookups, preserving the
  original enumeration index. A genuine zero-camera scan remains DEBUG.
- Usable-ID counts, model-lookup fallback diagnostics, and the number of logical
  handle-map entries. **That count is not a physical-handle or SDK-idle count.**
- Hardware-free tests of the production enumeration algorithm through injected
  scan/ID/model callables, without loading libqhyccd.
- A standalone, explicitly acknowledged raw-SDK probe for the later arm64 test.

The daemon gains no reset, new SDK calls, retry, selection policy, or config field.
The existing error/empty-list behavior remains unchanged. A subsequent linker-only
candidate below isolates SVBONY's bundled USB symbols. A matched rig comparison
reproduced the failure in the control and recovered across two power cycles in
the isolated daemon without restarting or resetting the SDK. Deleted USB
descriptors still accumulate; broader device/conformance validation remains
outstanding. The issue remains open.

## Hardware-free checks

From the repository root:

```bash
python3 -B -m unittest discover -s scripts/tests -p 'test_qhy_usb_recovery_probe.py' -v
python3 scripts/qhy_usb_recovery_probe.py --help
python3 scripts/check_docs_drift.py
python3 scripts/check_layering.py
python3 scripts/check_stress_registration.py
python3 scripts/check_contract_sweep.py
python3 scripts/changelog_fragments.py --check
```

On a supported arm64 development host, build production driver and tests together:

```bash
cmake -S AlpacaCore -B AlpacaCore/build-vendors \
  -DALPACACORE_ENABLE_ALL_VENDORS=OFF -DALPACACORE_ENABLE_QHY=ON
cmake --build AlpacaCore/build-vendors --target alpacacore_qhy alpacacore_tests -j2
AlpacaCore/build-vendors/tests/alpacacore_tests '[qhy][enumeration]'
AlpacaCore/build-vendors/tests/alpacacore_tests '[qhy]'
```

These tests must not call the real SDK. Existing `FakeQHYSDK` driver tests replace
the wrapper; they cannot establish the safety of resource release/reinitialization.
The new enumeration tests instead exercise the exact helper the wrapper calls.
They still cannot establish vendor hotplug behavior. Cross-build/emulated results
are not native rig validation. Run the full pre-flight and sanitizer/stress gates
on an appropriate arm64 host before submission.

### Development checkpoint (base `e94c276d`)

Historical hardware-free validation before the move from `main` to
`stable/5.0`, on an x86_64 development host (these C++ results are not a fresh
stable-base validation):

| Check | Evidence |
| --- | --- |
| Python probe | 31 fake-only unit tests passed, including retained-handle commands, wheel setup/rollback, ABI, and execution context; no SDK load |
| AArch64 cross-build | Actual `alpacacore_qhy` and `alpacacore_tests` targets built |
| Enumeration under QEMU | 47 assertions, 6 cases passed |
| Full QHY suite under QEMU | 1,177 assertions, 163 cases passed, including 7 stress cases |
| Enumeration with UBSan under QEMU | 47 assertions, 6 cases passed |
| ASan+UBSan under QEMU | Built, but execution exited 139 before test output; an empty ASan smoke program failed identically; **not a pass** |
| Enumeration mutations | All 7 mutations below detected as assertion failures |
| Static gates | Docs drift, layering, stress/contract registration, connect-error hooks, changelog, Unicode, formatting, and whitespace checks passed |

The local cross-build logs, toolchain, mutation copies, and checked-source hashes
are in `AlpacaCore/build-1211-cross/` (ignored build output, not committed).
Tool versions were GCC 14.2.0, Catch2 3.7.1, and QEMU 10.0.13. Regular emulated
stress cases are not TSan evidence. Native arm64 pre-flight, ASan/TSan, hardware
validation of the modified daemon, proof of failure recovery, and ConformU remain
outstanding. The rig experiments below used the official beta4/beta5 daemons and
the separately identified raw-open and wheel-mode probe versions.

Falsifying edits to the production helper
`AlpacaCore/include/alpacacore/vendor/qhy/detail/qhy_enumeration.h`, for the
eventual PR's `## Falsified by` section:

| Case (prefix `QHY enumeration - `) | Verified mutation |
| --- | --- |
| zero and invalid scan results remain distinguishable | Change the normal scan summary from DEBUG to WARNING |
| ID failures do not hide usable cameras | Return the partial camera vector instead of continuing after an ID failure |
| all ID failures are not logged as zero detected | Lower the failed-ID WARNING to DEBUG |
| model failure preserves camera identity and ordering | Clear the model instead of falling back to the camera ID |
| the maximum accepted scan count is enumerated | Reject `count >= 64` instead of `count > 64` |
| unterminated SDK strings are bounded | Remove the ID terminator assignment; separately remove the model terminator assignment |

### Stable-base recheck (`2b0fdeb6`)

After reapplying the diagnostic changes onto `stable/5.0`, all 31 fake Python
tests passed again. Docs drift, changelog, stress/contract registration,
connect-error hooks, formatting, and whitespace checks passed. The probe's C ABI
bindings were checked against stable's `26.06.04` SDK headers.

The layering gate passed on a clean snapshot of tracked source plus this work's
new source files. Running stable's older scanner directly in the existing
checkout fails while trying to read the pre-existing ignored directory
`AlpacaCore/cmake-build-debug/.cmake` as a file. No build directories were deleted
or unrelated scanner code changed to hide that local-artifact limitation.

New stable-base cross-build evidence is kept separately in
`AlpacaCore/build-1211-cross/stable/`; the earlier `main` results above must not
be relabelled as stable results. Hardware testing of the enhanced probe remains
deferred.

### Beta.5 base update (`18aa5361`)

The fix branch was advanced by rebase, preserving all uncommitted work and the
earlier validation/evidence records. Those earlier C++ results remain labelled
with their original bases; they are not a fresh beta.5 build or hardware run.

## Initial raw-SDK rig result (2026-10-16)

On the original Raspberry Pi Linux arm64 rig, with installed AlpacaBridge
`5.0.0~beta4`, the miniCam8M enumerated as USB `1618:0588`.
`/usr/lib/alpacabridge/libqhyccd.so` resolved to `libqhyccd.so.26.6.4.16`, SHA-256
`c9bdaa66ce956611d4097b3d008365229940f5fdeb3fd331c0ac0d8030cd9b3e`.
The probe source SHA-256 was
`c67f85b4bab20b516621c5305adf8a3ee8b65983a8861b0f21f92ef02b31701a`.

AlpacaBridge was stopped, all configured devices were initially disconnected,
and no other QHY consumers were found. One isolated probe process remained alive
throughout:

1. Three baseline scans found the same camera ID; three open/close cycles
   succeeded, leaving all handles closed.
2. The operator removed 12 V while the camera was stationary and USB remained
   connected. The camera disappeared from `lsusb`; three scans returned **0**, not
   `0xffffffff`.
3. After the operator restored 12 V, USB detected it again. Three scans found the
   original ID and an open/close succeeded **without resetting the SDK**.
4. Three explicit release → initialize → scan → same-ID open/close sequences
   succeeded. Release/init calls returned 0, taking approximately 0.02–0.11 ms;
   the first process initialization had taken 1.016 s. Process thread counts
   remained 3 throughout these checks. This does not establish that release/init
   rebuilds hotplug state or would repair a stale SDK.
5. The probe released resources and exited. AlpacaBridge was restarted and
   confirmed active. No exposure or wheel move was commanded.

**Conclusion:** the fully closed-handle power-cycle path recovered on its own.
Basic repeated SDK lifecycle calls worked in this limited test, but their
effectiveness against #1211 remains unproven. Reproduce power loss with the
driver's handle still open before selecting a recovery policy.

Raw evidence remains on the rig in `/home/pauleyj/qhy-1211-probe.d7Bk3s/` and was
copied locally to `AlpacaCore/build-1211-hardware/power-cycle-closed-handles/`
(ignored output). SDK diagnostic output is in `prompts.log`; structured probe
events are in `output.jsonl`.

## Connected-wheel rig reproduction (2026-10-16)

With the same installed `5.0.0~beta4` service and USB camera:

1. Connected integrated filter wheel 0 using `ClientID=1211`; it reported
   `Connected=true`, `Position=0`. The camera driver remained disconnected. No
   movement was commanded.
2. The operator removed 12 V, leaving USB attached. At 04:57:05 UTC, `lsusb` no
   longer listed the camera. The wheel still returned `Connected=true` and its
   cached `Position=0`, not a live hardware reading.
3. The operator restored 12 V. At 04:57:37 UTC, USB again showed `1618:0588`.
4. Explicit logical disconnect succeeded, but reconnect returned error **1280**,
   **"No QHY cameras detected"**. `Connected` then returned false; `Position`
   returned `NotConnected` (1031).
5. A second reconnect at 04:57:57 UTC returned the same 1280 refusal. The service
   remained active throughout; it had not been restarted between these attempts.
6. After evidence collection, restarted AlpacaBridge. A stationary wheel connect
   and disconnect both succeeded again; the service was left active and the
   wheel disconnected. Saved this result in `daemon-restart-recovery.json`.

The kernel log places the operational USB reappearance at 04:57:23 UTC, so the
two refusals were approximately 14 s and 35 s later, not just the firmware-loader
transition. The service journal emitted `Open QHYCCD error` at that reappearance.
After logical disconnect and both refused reconnects, the daemon still held a
descriptor for `/dev/bus/usb/002/004 (deleted)`, the pre-cycle USB device; the
restored camera was device 005. This is evidence of a retained stale USB
descriptor, not proof of the SDK's internal ownership or reset behavior.

This reproduces #1211 on the installed driver and identifies an important
distinction: the earlier fully closed-handle power cycle recovered, whereas a
cycle with the wheel's camera handle still open did not. The raw `ScanQHYCCD`
result in the failed daemon process is **not known**: the installed release lacks
the new diagnostic logs. Do not infer zero versus an SDK error from its refusal.

The second-reconnect API/USB snapshot and service journal are retained as
`connected-handle-reproduction.json`, `connected-handle-journal.log`,
`kernel-usb-events.log`, and `failed-daemon-usb-fds.txt` in the same rig evidence
directory. A separate probe cannot reset the daemon's in-process SDK state.

## Raw held-handle comparison (2026-10-16)

A third power-cycle experiment used the extended probe with `hold` and `close`,
source SHA-256
`e3f7ad157352c8f0b4b87b0062ca30e21c22b78d8228762c8f8e0d0a8a8bb36f`,
against the same installed SDK. AlpacaBridge and other consumers were stopped.

1. Initialized once, scanned the original ID, and called `OpenQHYCCD`, leaving
   its handle open across the stationary 12 V power cycle.
2. While power was off, USB lost the camera and three raw scans returned 0.
3. After power returned, USB showed `1618:0588`, now device 006. Three scans
   returned 1 and the original ID **while the old handle was still held**.
4. `CloseQHYCCD` returned 0 in approximately 0.017 ms; three more scans found
   the camera. A fresh same-ID open/close also succeeded without any reset.
5. Quit, releasing resources successfully. Restarted AlpacaBridge and verified
   it active with all configured devices disconnected. No exposure or motion
   was commanded.

**Conclusion:** raw `OpenQHYCCD` retention alone did not reproduce the installed
wheel-driver failure either. Do not conclude that every open handle poisons the
SDK, or enable automatic release/init on the strength of healthy-state tests.

This result motivated matching the wrapper's full non-moving CFW connection
sequence: logging setup, open, stream mode 0, CFW capability/slot-count/model and
status reads, and comparing the daemon's `alpacabridge` user and
`/var/lib/alpacabridge` working directory with the probe's `pauleyj` user and home
directory (the SDK logs loading its INI from that directory). Those differences
are investigation leads, **not established causes**. Only test a reset as a
remedy after reproducing the failed scan in that same process with all physical
handles confirmed closed.

Evidence is on the rig in `/home/pauleyj/qhy-1211-held-probe.yctG39/` and copied
locally to `AlpacaCore/build-1211-hardware/power-cycle-raw-held-handle/`
(ignored output). This comparison narrows reproduction; it does not satisfy the
automatic-recovery evidence gate or replace ConformU.

## Beta.5 installation and wheel-mode comparisons (2026-10-18)

Installed the official release asset `alpacabridge_5.0.0-beta.5_arm64.deb` after
checking its published SHA-256 locally and again on the Pi. Apt upgraded only
AlpacaBridge, leaving apt channels unchanged. The package audit is clean; all
five saved device configurations were verified unchanged. The pre-install state
is backed up at `/var/backups/alpacabridge-before-beta5.P5GBxS.tar.gz` on the Pi.

The installed SDK remains version `26.6.4.16`, SHA-256
`c9bdaa66ce956611d4097b3d008365229940f5fdeb3fd331c0ac0d8030cd9b3e`.
These runs used wheel-mode probe SHA-256
`88556dc05c0f00f8445973a1e319cae01598b17e2f8a6fbf15fe41a458c84496`.
Every baseline completed the driver-matching setup and reported 8 slots,
position 0, with a handle retained. No exposure, wheel move, or SDK reset was
commanded during these comparisons.

| Context and outage sequence | Result after USB restoration |
| --- | --- |
| `pauleyj` UID 1000, cwd/HOME `/home/pauleyj`, no `LD_LIBRARY_PATH`; scan during outage and before close | Three scans found the original ID; close succeeded; three further scans and a fresh full CFW open/close succeeded |
| `alpacabridge` UID 102/GID 105, cwd/HOME `/var/lib/alpacabridge`, `LD_LIBRARY_PATH=/usr/lib/alpacabridge`; same scan sequence | Same recovery, with fresh CFW setup again reporting 8 slots, position 0 |
| Same service-user probe process, second power cycle; **no SDK calls during this outage**, then close old handle before scanning | Close succeeded; all three scans found the original ID; fresh CFW setup succeeded |

The no-outage-scan control reused a process that had scanned during its previous
outage. It excludes SDK calls during that particular cycle, not possible effects
of earlier process history. These controls do not establish complete equivalence
to the daemon's threads, library loading, or systemd sandbox.

### Actual beta.5 daemon: failure reproduced and raw scan observed

Restarted the official beta.5 daemon, connected only the integrated wheel, and
confirmed position 0. With it stationary, the operator removed/restored 12 V;
no SDK/API request was made during the outage. USB returned as `1618:0588`,
device 011. Logical disconnect succeeded, but reconnect again returned **1280,
"No QHY cameras detected"**. `Connected` then read false.

Attached GDB to observe a subsequent normal API reconnect, with automatic GDB
script loading disabled. A breakpoint on the actual SDK `ScanQHYCCD` export,
followed by `finish`, recorded its AArch64 return register:

```text
ScanQHYCCD () from /usr/lib/alpacabridge/libqhyccd.so.20
SCAN_RETURN=0 HEX=0x0
```

No SDK calls were injected by the debugger. The normal reconnect still returned
1280, and GDB detached and exited. Thus this failing daemon path is now known to
return a genuine zero-camera scan, not the error sentinel or failed ID lookup.
Its process retained `/dev/bus/usb/002/010 (deleted)` after the restored camera
became device 011. The service journal again printed `Open QHYCCD error` when
USB returned. Loaded-library paths and the debugger transcript were saved for
offline comparison.

Restarted beta.5 after evidence collection. Stationary wheel connect/disconnect
both succeeded, and the service was left active with all five devices
disconnected. **The fault still reproduces in beta.5; an in-process SDK reset has
not repaired a reproduced failure.** Next investigate the remaining daemon/probe
differences rather than enabling a speculative reset policy.

Rig evidence directories:

- Login-user probe, daemon trace, and final state:
  `/home/pauleyj/qhy-1211-beta5-wheel.uIfDDs/`.
- Service-user probe and no-outage-SDK-call markers:
  `/var/tmp/qhy-1211-beta5-service.2PTcgX/`.
- Package/config verification: `/home/pauleyj/alpacabridge-beta5-install.Iy1PbY/`.

An archive excluding FIFOs and the package binary was copied locally to
`AlpacaCore/build-1211-hardware/beta5-20261018/qhy-1211-beta5-evidence.x_mhiq4t.tar.gz`
(ignored output). The same archive is retained in the Pi user's home directory.

## Failed-state in-daemon reset experiment (2026-10-18)

On the same official beta.5 package and installed SDK recorded above, connected
only the integrated wheel and confirmed position 0. The operator removed and
restored 12 V with USB attached; no SDK/API requests were made during the outage.
USB returned as device 012. GDB observed the normal disconnect's physical
`CloseQHYCCD` return **0**, then detached. Reconnect still failed with 1280,
"No QHY cameras detected". All five devices were confirmed disconnected before
the reset experiment; the camera driver had not been connected or started any
workers in this service session.

For the reset experiment, GDB stopped a normal reconnect at `ScanQHYCCD`, let it
return, and recorded **0**. On that same API thread, while the wrapper's
bookkeeping lock was still held, GDB explicitly called the SDK exports below.
Scheduler locking was off so SDK listener threads could run. Automatic GDB
script loading was disabled. This is debugger-injected experimental behavior,
not a new daemon endpoint or production recovery implementation.

| SDK call | Return | Debugger-observed elapsed time |
| --- | --- | --- |
| `ReleaseQHYCCDResource()` | 0 | 6.565 ms |
| `InitQHYCCDResource()` | 0 | 6.438 ms |
| `ScanQHYCCD()` | **0 cameras** | 5.891 ms |

These timings include debugger overhead, not just SDK execution. GDB detached
and exited normally. The interrupted normal reconnect still failed (its scan
had already returned zero before the injection). A **fresh API reconnect nine
seconds later also failed** with the same 1280 error. The process still held
`/dev/bus/usb/002/011 (deleted)` while USB showed device 012. Process thread
counts before and after the experiment were both 39; this single comparison
does not prove lifecycle safety across repeated resets.

**Conclusion: release/init success is not recovery.** One controlled attempt
against the reproduced failure did not restore enumeration or connectivity.
Do not implement the proposed simple automatic release/init retry from the
earlier healthy-state results. This does not exclude a different recovery
mechanism, but the remaining daemon/probe differences need investigation first
(full service sandbox, library loading/symbol resolution, and initialization
thread/process history are unconfirmed leads).

Restarted the service after evidence capture. Wheel connect/disconnect succeeded
again; all five devices were left disconnected, saved configurations unchanged,
and beta.5 active. No exposure or wheel movement was commanded. No production
recovery code was added.

Rig evidence: `/home/pauleyj/qhy-1211-failed-reset.ZclQw5/`, including
`close-trace.log`, `reset.gdb`, `reset-trace.log`, pre/post-reset state, service
and kernel journals, loaded mappings, and final-state verification. The archive
was copied locally to
`AlpacaCore/build-1211-hardware/beta5-20261018/qhy-1211-failed-reset.ZclQw5.tar.gz`
(ignored output).

### Independent repeat with explicitly acknowledged power steps

Repeated the failed-state reset test in a fresh official beta.5 service process
on 2026-10-18, after confirming USB presence and all devices disconnected. The
wheel connected at slot 0. The operator explicitly acknowledged removal and
restoration of 12 V; USB disappeared and returned as device 013. No SDK/API
requests were made during the outage. The normal disconnect's physical
`CloseQHYCCD` again returned 0, but reconnect failed with 1280.

Using the same debugger reset script on the next normal reconnect recorded:

| SDK call | Return | Debugger-observed elapsed time |
| --- | --- | --- |
| Initial `ScanQHYCCD()` | 0 cameras | Not separately timed |
| `ReleaseQHYCCDResource()` | 0 | 6.479 ms |
| `InitQHYCCDResource()` | 0 | 6.421 ms |
| Subsequent `ScanQHYCCD()` | **0 cameras** | 6.002 ms |

A fresh reconnect approximately eight seconds later still failed with 1280.
The stale device-012 USB descriptor remained while device 013 was present;
thread counts before and after reset were again 39. These are now **two
failed-state reset attempts in separate service processes**, not successful
recovery. The same debugger-injection and timing limitations apply to both.

Restart restored wheel connect/disconnect. The service was left active, all
five devices disconnected, configurations unchanged, and the operator was
explicitly asked to **leave 12 V and USB connected**. No movement or exposure
was commanded. Rig evidence is in
`/home/pauleyj/qhy-1211-reset-repeat.nCXUQO/`, with its archive copied locally to
`AlpacaCore/build-1211-hardware/beta5-20261018/qhy-1211-reset-repeat.nCXUQO.tar.gz`
(ignored output).

### Next lead: executable libusb symbol interposition

Read-only inspection after service restoration found that the official beta.5
executable exports 85 `libusb_*` definitions. Although QHY declares a dynamic
dependency on `libusb-1.0.so.0`, its live relocation slots for `libusb_init`,
`libusb_open`, `libusb_close`, `libusb_get_device_list`, `libusb_get_version`, and
`libusb_hotplug_register_callback` all pointed into **`/usr/bin/alpacabridge`**,
not the loaded system libusb. These addresses were read from `/proc/<pid>/mem`;
no SDK call or process-memory write was made. A slot pointing back into the
SDK was excluded from that conclusion because it may be a lazy-binding stub.

Scanning the stable checkout's vendored static archives with `nm` found that
only `external/SVBONY/lib/armv8/libSVBCameraSDK.a` (relative to `AlpacaCore/`)
defines `libusb_init`, `libusb_open`, and `libusb_get_version`, all in
`core.armv8.o`. Its vendor CMake file links this archive into the daemon. This
identifies an actionable interposition lead, not yet proof of the hotplug fault
or byte-for-byte attribution of the official binary to that archive.

Next test a build that confines the archive's embedded USB implementation to
its own users, verify QHY's actual runtime bindings, then repeat the stationary
power cycle. Do not add SDK resets as part of that comparison. The linker
change must also preserve SVBONY and other vendors' behavior before release.
The comparison now uses the service's normal sandbox with a temporary runtime
ExecStart override, as recorded below; the installed package binary is unchanged.

The candidate adds `--exclude-libs,libSVBCameraSDK.a` as an INTERFACE link
requirement on `alpacacore_svbony`, so executables reached both directly (Core
tests) and through built-in catalog/HTTP composition receive it. It keeps the
SDK's archive definitions local without removing SVBONY support, changing the
SDK bytes, or adding a QHY resource reset. The sibling sweep found one production
link site for that archive; the requirement covers both consumers. The new
`svbony_sdk_symbol_isolation` CTest inspects the linked Core test executable with
`nm -D --defined-only` and fails on an exported `libusb_*` definition. The HTTP
test configuration adds the same check for the actual server as
`svbony_server_symbol_isolation`.

The first native configure exposed a necessary sibling fix: HTTP's older CMake
policy scope dropped interface link options across PRIVATE static dependencies,
so Core tests received the flag while the server did not. HTTP now explicitly
sets `CMP0099` to NEW. Actual generated link commands were checked for the flag
on both targets; a passing Core check alone is not server coverage.

A native arm64 build is staged separately on the Pi at
`/home/pauleyj/qhy-1211-link-isolation.LG8pZG/`, with source hashes and build logs.
It enables QHY/SVBONY/ZWO/Celestron (the configured-device vendors plus the
suspected archive), Release with `-O1 -DNDEBUG`, Core tests ON and HTTP tests OFF.
The native server and Core test targets built successfully. The saved control
server exports 85 `libusb_*` symbols; the isolated server and test executable
export none. The Core symbol CTest passed, and the test executable was confirmed
to retain an actual `SVBOpenCamera` definition (not an absent SDK). Removing the
production archive-exclusion option, reconfiguring and relinking made that CTest
fail on exported symbols and restored 85 server exports. Restoring the option
made the check pass again. Saved control/fixed binaries and hashes are in that
evidence directory.

The initial native QHY fake suite passed 155/156 cases (1059/1060 assertions),
with the CFW3 boot-byte early-completion test taking 2.919 s against its 2.5 s
limit while the mutation relinks were also running. Three focused idle repeats
passed unchanged; the next full QHY run passed **156 cases / 1060 assertions**,
and SVBONY passed **44 cases / 537 assertions**. The first failure is retained
in the logs, not reclassified as a pass. These native cases use fakes and do not
prove physical recovery or SVBONY camera operation. The registered HTTP symbol
CTest also rejected the retained control server and accepted the isolated one
(ELF inspection only, with no HTTP runtime test hooks compiled into either).

The source hashes were verified on the Pi. A pre-custom-daemon state backup and
original-unit/package hashes were saved. Initial staging paths were unsuitable:
PrivateTmp hid `/var/tmp`, and the Pi mounts `/run` noexec. Both attempts failed
at EXEC before any camera operation; the original service was restored before
restaging. Do not weaken either setting for this experiment.

Root-owned matched binaries are now staged at
`/opt/alpacabridge-1211.Ox224Q/`. Only
`/run/systemd/system/alpacabridge.service.d/1211-usb-isolation.conf` overrides
ExecStart; the service user, working directory, library environment and sandbox
are unchanged. Remove **that experiment-owned override only**, daemon-reload,
and restart to return to the installed package after the comparison. A reboot
also removes the runtime override. The test binaries under `/opt` are not part
of the package.

The control daemon baseline succeeded at slot 0 with only the integrated wheel
connected (PID 698440). Read-only inspection confirmed the six resolved QHY USB
bindings point into the control executable, matching the official failing
daemon, and the loaded QHY library has the exact installed SHA-256 recorded
above. The control's stationary power cycle then reproduced 1280 after USB
returned as device 014; logical disconnect succeeded, reconnect failed, and
the deleted device-013 USB descriptor remained open. Thus the reduced-vendor
control reproduces the official package's failure under the same sandbox.

Switched only the runtime override's executable to the isolated binary. Its
baseline succeeded at slot 0 (PID 698928). The same read-only inspector now
found all six resolved QHY USB calls bound to system
`libusb-1.0.so.0.5.0`, with the exact same installed QHY SDK hash.

### Matched control/fixed rig outcome

| Build | Stationary 12 V power-cycle result |
| --- | --- |
| Unisolated control, PID 698440 | USB device 013 → 014; disconnect succeeded, reconnect failed with 1280 and "No QHY cameras detected" |
| Isolated daemon, PID 698928, cycle 1 | USB device 014 → 015; disconnect/reconnect both succeeded, Connected true, position 0 |
| Same isolated process, cycle 2 | USB device 015 → 016; disconnect/reconnect both succeeded again, Connected true, position 0 |

Both isolated successes occurred **without an SDK resource reset or service
restart**. For cycle 2 the operator initially said reconnected while power was
still off; the camera's absence was verified, no SDK/API request was made, and
the operator corrected the state and explicitly confirmed power on before the
second reconnect. Kernel logs establish both USB removals/re-enumerations. No
exposure or wheel movement was commanded in either build.

This matched result strongly supports executable USB-symbol interposition as
the cause of this wheel reconnect failure. It establishes the candidate's
limited stationary integrated-wheel recovery, not qualification of camera
exposures, camera/wheel shared ownership, all vendor combinations, or ConformU.

**Residual descriptor retention:** the isolated daemon retained deleted device
014 after cycle 1, and deleted devices 014 and 015 after cycle 2, alongside the
live device 016 descriptor. Explicitly disconnecting the wheel closed the live
device 016 descriptor but left both deleted descriptors until process shutdown.
Thus reconnect works in these two cycles, but per-cycle stale-descriptor
retention is not fixed; do not claim leak-free long-running hotplug or unlimited
power-cycle endurance. Its precise SDK/libusb ownership remains unconfirmed.

After capturing logs, mappings and descriptors, disconnected the wheel, removed
only the experiment's runtime override, daemon-reloaded and restarted the
**official beta.5 package**. Its stationary wheel connect/disconnect succeeded.
The package binary hash and all five saved device configurations are unchanged,
there are no service drop-ins, and all devices are disconnected. The candidate
is **not left running**; the installed beta.5 still has the original defect.
The operator was asked to leave 12 V and USB connected.

The evidence archive (excluding source/build trees, test binaries and the state
backup) was copied to
`AlpacaHTTP/build-1211-usb-isolation/qhy-1211-link-isolation.LG8pZG-evidence.tar.gz`
(ignored output). Its Pi counterpart and the full experiment directory remain
available. Saved native control/fixed binaries and manifests are also retained
locally under `AlpacaHTTP/build-1211-usb-isolation/`.

This is not an all-vendors package, full pre-flight, sanitizer run, or ConformU
evidence. Both control/fixed binaries and the original installed daemon remain
available; do not describe the candidate as fully qualified or leak-free.

Local evidence:
`AlpacaCore/build-1211-hardware/beta5-20261018/runtime-usb-bindings.json` (ignored
output). The full dynamic-export scan is also retained on the Pi as
`usb-dynamic-exports.txt` in the failed-reset evidence directory.

### Broader native software validation and review

Enabled HTTP tests in the native four-vendor build (QHY/SVBONY/ZWO/Celestron),
still Release `-O1 -DNDEBUG`; the retained hardware-test server remains the
earlier HTTP-test-hooks-OFF binary, SHA-256 recorded in `fixed-binaries.json`.
The first attempt to run the entire Core binary serially reached its 480 s
harness limit without an explicit assertion failure. It is not a completed
suite or a pass. The repository's parallel CTest layout is the appropriate
runner for these wait-heavy cases.

An initial parallel Core/HTTP run passed 727/729 checks. The two failures were
validation setup problems: the reduced source snapshot omitted the unchanged
`scripts/release_tag_cases.txt` and Debian helper/unit fixtures, and the live
ASCOM simulator plus official daemon shared discovery UDP port 32227 with the
test. Added and hash-recorded those four unchanged fixtures, then ran the
registered tests as the same login user in a separate network namespace with
loopback enabled (no host services stopped or network settings changed).
**All 729 CTests passed**, `-j4`, in 154.18 s. The earlier failures and timeout
logs remain preserved. No assertion or timing limit was loosened.

An independent read-only linker review found no confirmed blockers in archive
visibility, transitive options, CMake compatibility, or the two symbol checks.
Formatting (changed lines and complete new C++ files), gate self-tests, probe
tests and documentation checks also passed. This is still not full all-vendors
pre-flight, ASan/TSan coverage, or real SVBONY-camera validation.

### Residual descriptors: exact installed SDK inspection

Copied the installed `c9bdaa66...` SDK for offline analysis without loading it.
Its `.text` and `.rodata` sections are byte-identical to the repository's
`c334c906...` artifact; full hashes and section hashes are retained in
`AlpacaHTTP/build-1211-usb-isolation/qhy-sdk-section-comparison.json`.
Read-only AArch64 inspection confirms this path in the installed bytes (ELF
virtual addresses, before ASLR; local function names recovered from the
matching repository artifact):

- The departure callback stores status **10001** at `0x14a530`, then calls
  `CloseQHYCCD` with the saved handle at `0x14a568`.
- `QHYCAM::closeCamera` tests that same status at `0x15e784–0x15e79c` and
  returns early when it is 10001, bypassing its `libusb_close` at `+0x94`.
- The departure callback clears the stored handle at `0x14a678`.
- A later `CloseQHYCCD` whose handle lookup returns -1 can return **0** without
  teardown (`CloseQHYCCD+0x48–0x50`; result initialized to 0 at `+0x24`).

This establishes a plausible SDK-side orphaning path, not its execution in each
rig cycle or the absence of another cleanup path. In particular, the earlier
observed **CloseQHYCCD return 0 is not proof of USB descriptor closure**.
The wheel-only wrapper releases its owner, and the live descriptor closes on
normal disconnect while deleted predecessors remain. Do not change wrapper
ownership, force-close SDK-owned descriptors, patch the blob, or add resource
resets on this evidence. Keep this limitation separate from the proven linker
reconnect improvement and prepare an upstream QHY report with the hashes,
stationary reproduction, binding evidence, descriptor snapshots and instruction
anchors. No upstream report has been submitted by this task.

### Camera and shared-owner operational baseline

Restaged the retained isolated server under the same runtime-only override;
both mounts remain disconnected. In fresh PID 776520, the miniCam8M camera and
integrated wheel connected together. The camera reported Full Resolution mode,
3856×2180, bin 1; the wheel was at slot 0. A 0.1 s light exposure at temporary
64×64 ROI returned 4096 integer pixels in range, with duration readback 0.1 s.
Original ROI geometry was restored. Image quality/calibration is not claimed.

Both ownership directions then passed real operations: disconnecting the camera
left the wheel connected and able to move 0→1→0; reconnecting the camera and
disconnecting the wheel left the camera able to capture another 64×64 frame.
Reconnected the wheel and restored ROI geometry. Both devices now remain
connected in PID 776520, camera Idle, wheel stationary at slot 0, ready for a
coordinated shared-owner power-loss test. Camera telemetry and its default
cooler-control worker can make SDK calls during that outage; unlike the earlier
wheel-only test, this is not an SDK-silent scenario. No exposure was active
during power removal.

### Shared-owner power recovery and wheel conformance

The operator removed/restored 12 V with both camera and wheel logically
connected. USB device 016 disappeared and returned as 017; PID 776520 remained
unchanged. Explicitly disconnected camera then wheel to release both owners,
then reconnected camera (its cached-ID path) and wheel (configured index path).
All four writes succeeded; camera returned Idle and wheel position 0, with no
SDK reset or service restart. A fresh 0.1 s 64×64 exposure returned 4096 valid
integer pixels, ROI was restored, and the wheel moved 0→1→0 successfully with
the camera connected. These operations establish real shared-owner recovery,
not just a successful connection response.

Then disconnected both of our client registrations and ran full ConformU
**4.5.1.54507** on the Pi against the same candidate process, FilterWheel/0.
The full report (142 lines) was read: **0 errors, 0 issues, 0 warnings, and no
timing violations**, including both Connected setter and Platform 7 Connect /
Disconnect, DeviceState, every slot in both directions, long moves and invalid
positions. All timing classes met target; the slowest FAST member was
FocusOffsets at 0.047 s. The TRACE window had only the two expected InvalidValue
exceptions for positions -1 and 8. Original log level **WARNING** was restored;
all five devices are now disconnected. The isolated candidate service remains
running under the experiment's runtime-only override for the remaining camera
check; the package binary is unchanged.

The report, TRACE window, server PID/source/binary metadata and verdict are
preserved on the Pi in `conformu-wheel-after-recovery/` inside the experiment
directory, and locally in
`AlpacaHTTP/build-1211-usb-isolation/conformu-wheel-after-recovery/`. Existing
canonical ConformU reports and `SUPPORTED-DRIVERS.md` have not been overwritten.

The camera advertises `CanPulseGuide=true`, so its full ConformU suite can move
a mount through ST-4 even with telescope Alpaca devices disconnected. Before
that run the operator explicitly confirmed **only USB and 12 V were plugged
in**. No guide cable was connected during the following camera conformance
test. Preserve that safety check on future runs.

### Camera conformance after shared recovery

Full ConformU **4.5.1.54507**, Camera/1, also passed in the same recovered PID
776520: **0 errors, 0 issues, 0 warnings, all response-time targets met**. The
299-line report was read in full. It exercised connect/disconnect, DeviceState,
capabilities and optional-member errors, invalid values, cooler writes, four
ST-4 pulse directions with the guide port empty, and full-frame 2 s exposures
with ImageArray / ImageArrayVariant at bin 1, 2, 3 and 4. This qualifies the
default Full Resolution mode tested by ConformU, not a separate HDR endurance
test. The slowest FAST member was DeviceState at 0.012 s; StartExposure
was at most 0.114 s against STANDARD, and the longest image response was
ImageArrayVariant at 1.248 s against EXTENDED.

The 1,793-line TRACE window contained only expected invalid/unsupported-member
exceptions. Its two ERROR-level ImageArray / ImageArrayVariant entries were
the deliberate reads before any exposure, explicitly accepted by ConformU;
there were no unexpected SDK failures. Log level was restored to WARNING and
all devices disconnected. Evidence is retained in `conformu-camera-after-recovery/`
on the Pi and under `AlpacaHTTP/build-1211-usb-isolation/` locally. Existing
canonical reports were not overwritten.

Afterward removed the experiment runtime override and restored official
beta.5 again, confirming the package binary and saved configs unchanged. Only
the one deleted device-016 descriptor remained in the candidate before its
shutdown; healthy ConformU reconnects had not added more. This is still not
physical hotplug endurance or a fix for SDK teardown.

### All-vendors software checks and final hardware variant

Added the remaining **unchanged stable SDK files** to the native snapshot with
hash checks, then configured `ALPACACORE_ENABLE_ALL_VENDORS=ON`. Every vendor,
the actual server and the Core/HTTP test targets built. The first build
supervisor reached its 1,500 s limit at 94%; its make child continued. Waited
for that exact child to exit before running an incremental confirmation, which
returned success for every target. The timeout is preserved in its own log,
not called a clean completed build.

The isolated-network all-vendor CTest run passed **1,849/1,850** cases. One
unchanged SkyWatcher fake-clock race failed:
`SkyWatcher async - Tracking=false as a reversing pulse restore ends stops RA`.
Built otherwise identical all-vendor Core binaries with/without only the SVB
archive exclusion and repeated that case in fresh processes, seed 2600113356,
four at a time: the same failure occurred in **9/25 control** runs and **6/25
isolated** runs. This establishes baseline occurrence, not a new QHY
regression; it does not make the all-vendors suite green. No SkyWatcher code or
assertion was changed. The source-backed review identifies a split-lock pulse
restore / Tracking=false scheduling window; its correction is a separate task.

Native Cppcheck 2.17.1 passed on the changed production source/header. Native
clang-tidy with the repository config and changed-line filter passed, and the
layering gate passed on the clean staged source (avoiding the unrelated local
ignored build-directory issue). This still is not a full clean pre-flight:
the Pi's exact kernel is `6.18.34+rpt-rpi-v8`, `CONFIG_ARM64_VA_BITS=39`, so the
documented ASan host limitation applies. Sanitizers require a suitable host/CI.
Issue #1211 currently has no milestone, and the public v5.0.0 promotion
milestone has no named admission for this change; that policy requirement
remains unverified, not overridden by the branch target.

Built and saved an all-vendors server with **HTTP test hooks OFF**, SHA-256
`7880cf431405351afc3be6977a2997bd323c42629975f6725c3fe986d65a4f93`.
This is another native Release `-O1 -DNDEBUG` test binary, not a replacement
package. It exports zero `libusb_*` definitions. Restaged it through the same
runtime-only service override in fresh PID 843026: camera and wheel both
connected, camera Idle, wheel at slot 0. All six inspected QHY USB bindings
still resolve to system libusb with the exact installed SDK bytes, now with
all other vendor libraries present. The earlier hardware/ConformU results
belong to the four-vendor server; a final coordinated power cycle on this
all-vendor variant is next. Both mounts remain disconnected, and no exposure
is active.

### Final all-vendors recovery and conformance result

The operator removed and restored 12 V in separate acknowledged steps, with
USB left connected. Verified device 017 absent before requesting restoration;
it returned as device 018 with **PID 843026 unchanged**. Released both logical
owners and reconnected camera then integrated wheel successfully, without an
SDK reset or service restart. A new 0.1 s 64×64 image contained 4,096 valid
pixels (range 1,744–2,000), and the wheel completed 0→1→0. Restored ROI and
disconnected both before conformance.

Full ConformU **4.5.1.54507** then passed for both Camera/1 and FilterWheel/0
in that same recovered all-vendors process: **zero errors, issues, warnings
and timing violations**. Read both complete reports (299 and 142 lines).
Camera tested Full Resolution at bin 1–4; this is not an HDR endurance test.
Reviewed the extracted TRACE findings: camera's 48 entries were expected
invalid/unsupported calls, including the deliberate image reads before an
exposure; wheel's two entries were invalid positions -1 and 8. There was no
unexpected failure among these findings. TRACE windows retain 1,808 camera
and 540 wheel lines. Evidence directories are `conformu-all-vendors-camera/`
and `conformu-all-vendors-filterwheel/`, on the Pi and in the local approved
build directory. PR preparation subsequently copied these exact report bytes
to `AlpacaCore/conformu/QHY/miniCam8M/Linux-arm64.txt` and
`AlpacaCore/conformu/QHY/miniCam8M CFW/Linux-arm64.txt`, respectively. Their
SHA-256 values are `dfba00174af5e7c110281c537b5605b3d31c3994f00102495e7448c5258d3402`
and `f677be7f9eb0550e62bc639c4c77c5199428955c1e67eca967547a541eb914fa`.

One deleted device-017 descriptor remained before shutdown, so the SDK
descriptor-retention limitation is unchanged. Removed only the experiment's
runtime override and restored the **official beta.5 service**. Verified it is
active with no drop-ins, the package binary and all five saved device configs
unchanged, all devices disconnected, and log level WARNING. Final restoration
record: `official-after-all-vendors-conformu.json`.

Hardware recovery qualification of this all-vendors candidate is complete for
the tested miniCam8M setup. Remaining delivery gates are a clean full pre-flight,
suitable-host sanitizer coverage, physical SVBONY validation, and named stable
policy admission. The independently reproduced SkyWatcher test failure remains
separate. No commit, push or PR has been made.

### PR preparation

Included both final all-vendors reports in their canonical paths and updated
`SUPPORTED-DRIVERS.md`. Report bytes, including ConformU's trailing spaces,
are unchanged; source/document whitespace checks exclude those two reports.
Verified the current C++/CMake source hashes still match the tested manifest.
Applied Black to the manual Python probe and its tests; Black check, Flake8
(88-column limit, Black-compatible E203/W503 exclusions), and all 31 fake tests
pass. Changed-line clang-format and new-file format checks pass. Re-ran native
Cppcheck 2.17.1 and changed-line clang-tidy successfully. Documentation, Unicode,
changelog, connection-error hook and stress/contract registration checks pass.
The PR description is prepared in the ignored experiment build directory;
template validation and working-tree coverage of all six `Falsified by` cases
pass. Full local pre-flight is deferred at the contributor's request to GitHub
CI, not reported as a local pass. No workflow has been started and stable-policy
admission is still pending; preparation does not waive repository submission gates.

### Full Pi pre-flight: initial result and launcher correction

Ran the unchanged `scripts/ci_preflight.sh` on a separate hash-checked Pi git
snapshot, validation-only commit `b7d9220767f04392841dcfa42c21733e29fe5036`
over stable base `18aa5361`. That snapshot commit is not the contribution
branch's commit and was not pushed. Supplied the prepared PR body, restricted
the build to two CPUs, and isolated network discovery. No installed service or
package was changed. Full log and result: `full-preflight-pi.log` and
`full-preflight-pi-result.json` in the experiment evidence directory.

The first run exited 1. All static gates, including format, Unicode,
registration, PR body, reports, docs, layering, clang-tidy and Cppcheck passed.
Vendor-neutral Core passed **212/212**; all-vendors Core passed **1,833/1,833**
on this run (the earlier baseline SkyWatcher race remains a known intermittent
failure, not disproved by this pass). HTTP passed 13/15 vendors-OFF and 15/17
vendors-ON. Both failures in each HTTP variant were `test_server_socket` and
`test_thread_join`, aborting on permission denied for
`/root/.local/state/AlpacaBridge/logs`. The launcher dropped UID/GID but
incorrectly retained sudo's `HOME=/root`; this was a validation-launcher error,
not evidence of a QHY regression. No production assertion was changed.

The enabled ASan/UBSan build reached Catch2 test discovery and aborted before
tests with `sanitizer_allocator_primary64.h:131`, the documented Pi 39-bit-VA
host limitation. This is **not sanitizer coverage**. TSan was not enabled.
Shell/JavaScript/workflow gates were inapplicable; no tools were installed.
The official service stayed active, with over 2 GB free at completion.

The second full unchanged-script run completed with **exit 0**, with
HOME/USER/LOGNAME set to the actual user and inherited XDG paths cleared.
Vendor-neutral Core passed **212/212**, vendor-neutral HTTP **15/15**,
all-vendors Core **1,833/1,833**, and all-vendors HTTP **17/17**. All enabled
pre-flight gates passed, including both link-isolation checks, format, lint,
documentation and PR/report validation. The script reported "Safe to push."
It explicitly set `RUN_SANITIZERS=0` after the documented host failure;
sanitizers remain **skipped, not passed**, and require suitable-host/CI coverage.
TSan was not enabled. Shell, JS and workflow checks were inapplicable.
No tool installations were needed. The official service stayed active.

The initial failure log is retained separately, not replaced by the rerun.
Corrected evidence is `full-preflight-pi-corrected-home.log` and
`full-preflight-pi-corrected-home-result.json`, retained on the Pi and locally
under `AlpacaHTTP/build-1211-usb-isolation/`. Later edits to this narrative and
the prepared PR description are not new C++/CMake code; the tested source
hashes remain the same. Named stable-policy admission is still pending.

## Rig checklist: capture the failing state first

Use the original miniCam8M/Pi when available. Do not obstruct the wheel, cut power
mid-motion, or start an exposure during power-cycle experiments.

1. Record the commit/package, SDK version, configured camera ID/index, driver
   connection states, `lsusb` identities, and firmware-loader/kernel events.
2. Set daemon logging to DEBUG. Connect once, disconnect, unplug/replug USB, wait
   for the post-firmware USB identity, then reconnect without restarting.
3. Repeat with the stationary camera's 12 V supply. Separately reproduce the
   original power-loss-while-logically-connected sequence, then disconnect all
   paired drivers before reconnecting.
4. Test camera alone, integrated wheel alone, and both sharing the camera; test
   explicit IDs and index-based configuration. Record the actual elapsed time
   between restoration and each connect attempt.
5. Preserve `ScanQHYCCD`, `GetQHYCCDId`, and init diagnostics. Distinguish zero,
   `0xffffffff`, an oversized count, and positive scans with failed ID retrieval.
   Partial enumeration compresses the returned vector today: do not assume an
   enumeration index still identifies the same physical camera after a replug.

Error 1280 is consistent with the router mapping a failed connect to
`DriverException` while retaining its reason. Do not change HTTP error mapping
to address this SDK problem.

## Deferred rig test: raw SDK experiment

The probe uses Python's standard library and explicitly binds the SDK's C ABI.
It is not installed into, called by, or connected to the daemon. **Do not run it
on a USB-less test runner. Even loading/calling the real SDK can crash.**

Stop AlpacaBridge and every other QHY consumer (including guiding software,
camera tools, and other probes). Verify this yourself: the probe cannot detect
another process's handles. A separate process also cannot inherit the daemon's
stale SDK state; reproduce the failure afresh inside this probe.

### Wheel-mode procedure

Preparation is covered by 13 additional fake-only cases on top of the original
18. Fourteen targeted mutations (including the expanded ABI case) were detected
as assertion failures. An independent source review found no blocking issues.
No real SDK was loaded during that code-preparation/test pass. The later
2026-10-18 wheel-mode rig runs are recorded above; a failed-state reset test
remains outstanding.

Use `--wheel` for the next reproduction. Without it, the original raw-open
comparison remains unchanged. Wheel mode makes the same non-moving C API calls
as the integrated wheel's fresh, wheel-only connection:

```text
EnableQHYCCDMessage(false)
EnableQHYCCDLogFile(false)
InitQHYCCDResource()
GetQHYCCDSDKVersion(...)
scan: ScanQHYCCD(), then GetQHYCCDId()/GetQHYCCDModel() for each usable entry
hold/cycle: OpenQHYCCD(id), SetQHYCCDStreamMode(handle, 0)
           IsQHYCCDControlAvailable(handle, 17 /* CFWPORT */)
           GetQHYCCDParam(handle, 44 /* CFWSLOTSNUM */)
           GetQHYCCDModel(id), GetQHYCCDCFWStatus(handle)
```

No `InitQHYCCD`, exposure, wheel order, or parameter write is requested. Initial
model lookup occurs during `scan`, then model lookup is repeated after open,
matching index resolution followed by connect. The operator still selects the
exact ID; the probe does not automatically rebind an enumeration index or model
a camera/wheel shared-handle pair. Matching API order does not reproduce the
daemon's thread scheduling or linked-library load timing.

The SDK version call follows successful resource initialization; a version-query
failure does not refuse the connection. Missing CFW capability, invalid slot
count, or stream-mode failure closes the acquired handle once and stops. Slot
validation preserves the raw double, checks signed-int bounds/nonfinite values,
then truncates toward zero and requires a positive result, as production does.
Model failures and a failed/nondigit status read are nonfatal; status maps to
position `-1`. All status-buffer bytes are recorded independently of that
interpretation. A digit is not proof that a wheel is stationary.

The probe intentionally remains stricter than the production wrapper about
close errors: a failed or uncertain close prevents release/reset and is not
retried. Interrupted opens retain the existing uncertain-open guard. It adds no
automatic recovery policy.

Session records include real/effective UID and GID, supplementary groups, cwd,
`HOME`, `LD_LIBRARY_PATH`, the selected library, and whether wheel mode is enabled.
Calls record timings, thread counts, unsigned status/hex, void returns, and raw
double representations (nonfinite values have JSON `null` plus their explicit
representation). Compare those records when running under the two accounts.

**Run only with hardware authorization and all other consumers stopped:**

```bash
# Existing login-user context, for comparison:
python3 scripts/qhy_usb_recovery_probe.py --wheel \
  --library /usr/lib/alpacabridge/libqhyccd.so > qhy-wheel-login.jsonl

# Service user and cwd, with the probe at an absolute path readable by that user:
sudo -u alpacabridge -g alpacabridge -- sh -c '
  cd /var/lib/alpacabridge &&
  exec env LD_LIBRARY_PATH=/usr/lib/alpacabridge /usr/bin/python3 -B \
    /absolute/readable/path/qhy_usb_recovery_probe.py --wheel \
    --library /usr/lib/alpacabridge/libqhyccd.so
' > qhy-wheel-service-user.jsonl
```

Stop every SDK consumer before **each** session and complete it before starting
the other. These commands do not edit the service or change file permissions.
The second matches user/group/cwd/library environment, **not the full systemd
sandbox**, and the session record reveals actual identity/environment differences.
Do not copy or modify the SDK INI to make the contexts appear equal. Vendor code
may create INI/log files or have hardware side effects even without motion calls.

### Future interactive sequence

```bash
python3 scripts/qhy_usb_recovery_probe.py \
  --wheel --library /usr/lib/alpacabridge/libqhyccd.so > qhy-usb-recovery.jsonl
```

Prompts go to stderr. Type `STOPPED` only after all consumers are stopped.
Vendor output may mix with the JSON records on stdout, so retain the entire file.
Example interactive sequence (replace the ID with the exact baseline scan ID):

```text
scan 1
hold EXACT-CAMERA-ID
unplug
replug
scan 3
close
cycle EXACT-CAMERA-ID 1
reset
scan 3
cycle EXACT-CAMERA-ID 3
quit
```

`unplug` and `replug` each require a separate `DONE` acknowledgement after the
physical step. For 12 V tests, use `scan` before and after manually removing and
restoring power, noting timestamps and waiting for firmware enumeration. Repeat
several unplug/power cycles **in the same process**, retaining the baseline ID.

`hold` leaves one handle open across the physical cycle; `close` explicitly
closes it. With `--wheel`, that handle has completed CFW setup. To compare the
fully closed-handle path, use `cycle EXACT-CAMERA-ID 1` instead of `hold`, and
omit `close`. Omit `--wheel` only for the earlier bare-open control experiment.
Neither `hold` nor `cycle` opens a second handle while one is held. Close before
`quit`: quitting while held refuses resource release and exits nonzero.
`reset` explicitly releases and reinitializes resources; it is never automatic.
All probe-owned handles must have confirmed closes. An uncertain open, failed
close, release failure, or initialization failure stops the experiment rather
than resetting over unknown SDK state. A confirmed null open is logged and can
be repeated or followed by an explicit reset. Exit status zero means the session
completed normally, **not** that every scan/open succeeded or recovery worked;
inspect the recorded events. The probe requests no exposure,
wheel move, guiding, or camera initialization; vendor open/resource-init code
may itself affect hardware.

Record scan/open/close/release/init return values and timings, and process thread
counts across cycles. SDK calls have no enforceable in-process timeout: a hang
or crash is a failed experiment, not permission to detach and reset concurrently.
Ctrl-C cleanup is best-effort. Do not start another consumer while a probe call
is still running.

## Gate for implementing automatic recovery

Only proceed once repeated release → initialize → scan → same-ID open/close has
restored the reproduced failure reliably, without leaked listener threads,
crashes, or unacceptable delays. Then verify an actual exposure and wheel move
through the driver, and run ConformU 4.5.1+ for both on the exact tested commit.

The production design must cover these siblings together:

- Camera and integrated wheel; synchronous and asynchronous connect.
- Index-based, cached-ID, and explicit-ID opens. Explicit IDs bypass enumeration;
  the camera retains its resolved ID across disconnect. The wheel clears an
  index-resolved ID on disconnect and post-open failure, but not when open itself
  fails. Enumeration-only recovery would miss these paths.
- Shared owners and other connected QHY cameras: never close behind them.
- Running **and queued** calls retaining shared handles after map removal.
- Completion of physical close, not merely removal of logical ownership.
- Detached-worker liveness between SDK calls, especially the exposure guard.
- Failed release/init and exactly one retry per connection attempt.

Track physical lifetime through close completion independently of the handle
map. Serialize recovery/open decisions but never wait for workers while holding
the bookkeeping mutex; cancellation may need it. Deleters must not reacquire
that mutex, and `cancel_exposure()` must retain its per-handle-lock bypass.
Build a narrow backend seam beneath the actual wrapper to test those invariants;
do not simulate recovery solely in `FakeQHYSDK` and claim wrapper coverage.

If repeated SDK reinitialization is unsafe or ineffective, retain this diagnostic
stage and report the evidence to QHY. A successful service restart alone does not
prove safe in-process recovery.
