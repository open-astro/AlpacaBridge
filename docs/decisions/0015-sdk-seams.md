# SDK seams

Status: accepted

## Context

A vendor SDK driver reaches its vendor library through a wrapper, and before #104 every wrapper was a singleton the driver called directly. The paths most likely to break (a throw during connect, a ref-count that does not balance, a reconnect, enumeration index math, an SDK call that hangs) could then only be tested on hardware. For QHY no connect could be tested at all: the first `libqhyccd` call starts `PnpEventListenerThread`, which segfaults in `libusb_hotplug_register_callback` on a host without a working USB stack (#321). The concurrency stress harness (#101) needs to drive one driver from many threads, and an unguarded test fake then shows up in ThreadSanitizer as races in test code, which hide the driver races the harness exists to find.

## Decision

1. **One seam per vendor library.** The library is reached through one abstract interface in the vendor's include directory. The production wrapper implements it. Every driver factory keeps its production signature, which passes the real wrapper, and gains an overload that takes the interface by reference. Driver code reaches the library only through the injected interface. The wrapper's `instance()` is named only where production code builds the real seam: the production factory overloads, free enumeration helpers, and production adapters over a seam (such as Player One's thermal adapter over its camera SDK).
2. **Three implementations.** The real wrapper; a scripted fake under `AlpacaCore/tests/` (`fake_*.h`, for example `fake_qhy_sdk.h` or `fake_astroasis_hid.h`) that can throw from any named call and returns canned enumerations; and a `Locked<Vendor>SDK` decorator under `AlpacaCore/tests/` that forwards every call under one leaf mutex, for the `[stress]` cases. The decorator is test-only: each production wrapper serialises its own calls.
3. **Destructor.** A seam over a shared library singleton has a protected, non-virtual destructor. Nothing owns a seam pointer (drivers hold a reference, workers capture a raw pointer, implementations are statics or stack objects), so `delete` through the interface must not compile. A seam over a per-device transport that the driver owns through `std::unique_ptr`, such as `AstroasisHidTransport`, has a public virtual destructor instead, because the owner deletes through the interface.
4. **The fake is the harsher of the two.** Where a fake and the real library disagree, the fake answers the sentinel, the slower settle or the error, never the plausible value. Calls the driver uses as a pair (size a buffer with one, fill it with the other) agree with each other in the fake. Known differences should be listed in a `KNOWN PARITY GAPS` block at the top of the fake, and closing a gap is preferred to listing it.
5. **No fake method blocks** unless a test asks it to through an explicit hook. A decorator that holds its mutex across a blocking fake call turns the stress storm into a queue, and a detached driver worker left inside a blocking fake outlives its test.

The rules and their history are owned by the [testing instructions](../../.github/instructions/testing.instructions.md#hardware-free-driver-tests-via-the-sdk-seam-touptek-qhy-gphoto-and-svbony-camera--extend-to-other-vendors). The vendor deltas (QHY workers capture a raw `QHYSDK*`, the QHY decorator's separate `cancel_exposure()` lock, the gphoto `RawDecoder` seam) are owned by the vendor instruction files.


## Rules

The rules a change is checked against. Each names where it applies and what checks it; the reasoning is in the Decision above.

1. A seamed driver reaches its vendor library only through the injected interface; the wrapper's `instance()` is named only by production factories, enumeration helpers and production adapters. Scope: `AlpacaCore/src/vendors/**`. Checked by: review.
2. Each seam has a scripted fake and a `Locked<Vendor>SDK` decorator under `AlpacaCore/tests/`, and the decorator takes its mutex on every forward; the gphoto `RawDecoder` seam has only a fake (see its Adoption row). Scope: `AlpacaCore/tests/fake_*.h`, `AlpacaCore/tests/locked_*.h`. Checked by: `scripts/check_docs_drift.py` check 6 (QHY) and check 23 (roster), review.
3. A seam over a shared library singleton has a protected non-virtual destructor; a seam the driver owns through `std::unique_ptr` has a public virtual one. Scope: `AlpacaCore/include/alpacacore/vendor/**`. Checked by: review.
4. A fake is the harsher of the two where it and the library disagree, and calls used as a pair agree in the fake. Scope: `AlpacaCore/tests/fake_*.h`. Checked by: review.
5. No fake method blocks unless a test sets an explicit hook. Scope: `AlpacaCore/tests/fake_*.h`. Checked by: review. <!-- allowed -->

## Alternatives rejected

- **Keep calling the singleton and test only on the rig.** The highest-risk paths are the ones a rig run rarely reaches (a throw from one named call, a hung SDK call), and QHY cannot be touched on a test runner at all.
- **A public virtual destructor on a shared-library seam.** `ToupTekSDK` has one because it predates the reasoning in point 3. It is harmless there and is not changed, but it lets `delete` through the interface compile, so a new seam does not copy it.
- **Thread-harden every fake instead of a decorator.** It puts locking into every scripted method of every fake, while single-path tests need none, and it hides which races belong to the fake.
- **One universal adapter for all vendors.** Rejected in [decision 0013](0013-designs-to-avoid.md), section 1: the seam is per vendor library, where the differences are.

## Consequences

- A connect-path or cleanup fix in a seamed driver comes with a fake test that throws from the call that regressed.
- Each new seam adds a fake, a decorator, a `[stress]` registration (`scripts/check_stress_registration.py`) and a contract-sweep row. The QHY seam's interface, decorator and sweep lists are compared by `scripts/check_docs_drift.py` check 6; the other seams have no such list check yet.
- A fake is evidence only as far as it is faithful (CONTEXT.md, "Fake evidence vs rig evidence"), so a seamed driver still needs ConformU on hardware for the claims a fake cannot make.
- To reverse for one vendor: delete its overloads, fake and decorator, and its tests go back to the rig.

## Adoption

The seams in place. Each row's source is the seam's test-only Locked decorator, which names the interface it wraps. `scripts/check_docs_drift.py` check 23 fails when a `locked_*.h` decorator has no row.

Scan: `AlpacaCore/tests/locked_*.h`

| Driver | Symbol | Source | State | Issue | Notes |
| --- | --- | --- | --- | --- | --- |
| ToupTek camera, filter wheel and focuser (and the Altair rebadge) | `ToupTekSDK` | `AlpacaCore/tests/locked_touptek_sdk.h` | adopted | #104 | public virtual destructor kept (Alternatives rejected) |
| ToupTek StellaVita powerbox | `TouptekPowerboxSDK` | `AlpacaCore/tests/locked_touptek_powerbox_sdk.h` | adopted | #1345 | |
| QHY camera and CFW | `QHYSDK` | `AlpacaCore/tests/locked_qhy_sdk.h` | adopted | #321 | |
| gphoto camera | `GPhotoSDK` | `AlpacaCore/tests/locked_gphoto_sdk.h` | adopted | #489, #546 | issue #489, PR #546 |
| gphoto raw decoder | `RawDecoder` | `AlpacaCore/tests/fake_raw_decoder.h` | adopted | #546 | fake only; libraw needs no decorator |
| SVBONY camera | `SVBSDK` | `AlpacaCore/tests/locked_svbony_sdk.h` | adopted | #1010 | |
| Player One camera | `PlayerOneSDK` | `AlpacaCore/tests/locked_playerone_sdk.h` | adopted | #1256 | |
| Player One Phoenix wheel | `PlayerOnePWSDK` | `AlpacaCore/tests/locked_playerone_pw_sdk.h` | adopted | #1256 | |
| ZWO camera | `ZWOSDK` | `AlpacaCore/tests/locked_zwo_sdk.h` | adopted | #1321 | |
| ZWO EFW | `ZWOEFWSDK` | `AlpacaCore/tests/locked_zwo_efw_sdk.h` | adopted | #1339 | |
| ZWO EAF | `ZWOEAFSDK` | `AlpacaCore/tests/locked_zwo_eaf_sdk.h` | adopted | #1356 | |
| ZWO CAA | `ZWOCAASDK` | `AlpacaCore/tests/locked_zwo_caa_sdk.h` | adopted | #1356 | |
| Astroasis focuser (HID) | `AstroasisHidTransport` | `AlpacaCore/tests/locked_astroasis_hid_transport.h` | adopted | #1398 | owned per-device transport |
| iOptron iGuider camera (V4L2) | `IGuiderV4L2` | `AlpacaCore/tests/locked_iguider_v4l2.h` | adopted | | kernel V4L2 API, not a vendor SDK; handle-based like the SDK seams |

Serial and socket drivers have no SDK and are tested through pty fakes and fake servers instead ([decision 0013](0013-designs-to-avoid.md), section 7).

## Links

- Issues [#104](https://github.com/open-astro/AlpacaBridge/issues/104) (the seam pattern), [#101](https://github.com/open-astro/AlpacaBridge/issues/101) (the stress harness the decorator serves), [#321](https://github.com/open-astro/AlpacaBridge/issues/321) (the QHY seam), [#339](https://github.com/open-astro/AlpacaBridge/issues/339) (the explicit blocking hook of point 5 and the QHY decorator's separate cancel lock); upstream design review [#584](https://github.com/open-astro/AlpacaBridge/issues/584).
- Rules: [testing instructions](../../.github/instructions/testing.instructions.md); module overview in [architecture](../architecture.md#modules).
- Related: [decision 0013](0013-designs-to-avoid.md) (designs to avoid).
