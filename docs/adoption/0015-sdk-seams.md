# SDK seams: adoption

The roster of [decision 0015](../decisions/0015-sdk-seams.md). It is kept out of the record so a PR that
adds or changes a row needs no `ADR authorized` comment; `scripts/check_docs_drift.py` check 23
compares it with the code.

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
| WeeWX observing conditions (HTTP) | `HttpClient` | `AlpacaCore/tests/locked_http_client.h` | adopted | #1753 | the first HTTP seam; real client `util::make_curl_http_client()`, fake `FakeHttpClient` ([decision 0023](../decisions/0023-outbound-clients-in-vendor-wrappers.md)) |
| remotealpaca (HTTP) | `HttpClient` | `AlpacaCore/tests/locked_http_client.h` | adopted | #1753 | built on the seam ([decision 0023](../decisions/0023-outbound-clients-in-vendor-wrappers.md)); fake `FakeHttpClient` answering from the scripted upstream `AlpacaUpstreamScript` |

Serial and socket drivers have no SDK and are tested through pty fakes and fake servers instead ([decision 0013](../decisions/0013-designs-to-avoid.md), section 7).
