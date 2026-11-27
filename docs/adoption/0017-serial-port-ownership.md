# Serial port ownership: adoption

The roster of [decision 0017](../decisions/0017-serial-port-ownership.md). It is kept out of the record so a PR that
adds or changes a row needs no `ADR authorized` comment; `scripts/check_docs_drift.py` check 23
compares it with the code.

## Adoption

`scripts/check_docs_drift.py` check 23 holds this roster to the code: an `adopted` row fails when none of its sources names `SerialLink`, and a scanned wrapper that names it without a row fails too. [#1448](https://github.com/open-astro/AlpacaBridge/issues/1448) ported the `stable/5.0` wrappers to `main` and converted the `main`-only Celestron CGX wrapper. The SynScan handset probe has its own row because it sits outside the scan.

Scan: `AlpacaCore/src/vendors/*/*_protocol_wrapper.cpp`
Symbol: `SerialLink`

| Driver | Source | State | Issue | Notes |
| --- | --- | --- | --- | --- |
| Celestron NexStar | `AlpacaCore/src/vendors/celestron/celestron_protocol_wrapper.cpp` | adopted | #1448 | Ported from #1435 |
| Celestron CGX mount, AUX focuser | `AlpacaCore/src/vendors/celestron/celestron_cgx_protocol_wrapper.cpp` | adopted | #1448 | `main` only; one owner name per device; `SerialExclusive::Required`; keeps its link across a logical disconnect ([decision 0011](0011-cgx-logical-reference-continuity.md) rule 1) |
| Gemini focuser, flat panel, PDH | `AlpacaCore/src/vendors/gemini/gemini_protocol_wrapper.cpp`, `AlpacaCore/src/vendors/gemini/gemini_flatpanel_protocol_wrapper.cpp`, `AlpacaCore/src/vendors/gemini/gemini_pdh_protocol_wrapper.cpp` | adopted | #1448 | Ported from #1435 |
| iOptron mount, iEAF, iEFW | `AlpacaCore/src/vendors/ioptron/ioptron_protocol_wrapper.cpp`, `AlpacaCore/src/vendors/ioptron/ioptron_ieaf_protocol_wrapper.cpp`, `AlpacaCore/src/vendors/ioptron/ioptron_iefw_protocol_wrapper.cpp` | adopted | #1448 | Ported from #1435; replaces the #995 exclusive hold |
| iOptron iFR focuser-rotator | `AlpacaCore/src/vendors/ioptron/ioptron_ifr_protocol_wrapper.cpp` | adopted | #2002 | one `SerialLink` per unit, shared by the focuser and the rotator through `IfrLink::acquire()` under one owner name (`iOptron iFR`); the second device joins the open session instead of claiming the port again |
| OnStep | `AlpacaCore/src/vendors/onstep/onstep_protocol_wrapper.cpp` | adopted | #1448, #2223, #2221, #2222 | Ported from #1435; keeps `main`'s 0009 link loss (#1241, #1289); one `SerialLink` per controller, shared by the telescope, the focusers, the rotator (#2221) and the switch (#2222) through `OnStepProtocolWrapper::acquire()` under one owner name (`OnStep`, #2223); a later device joins the open link instead of claiming the port again |
| QHY CFW3, Q-Focuser | `AlpacaCore/src/vendors/qhy/qhy_cfw3_protocol_wrapper.cpp`, `AlpacaCore/src/vendors/qhy/qhy_qfocuser_protocol_wrapper.cpp` | adopted | #1448 | Ported from #1435 |
| Sky-Watcher | `AlpacaCore/src/vendors/skywatcher/skywatcher_protocol_wrapper.cpp` | adopted | #1448 | Ported from #1435; keeps `main`'s 0009 link loss (#981) |
| SynScan | `AlpacaCore/src/vendors/synscan/synscan_protocol_wrapper.cpp` | adopted | #1448 | Ported from #1435 |
| SynScan handset probe | `AlpacaCore/include/alpacacore/util/synscan_handset_probe.h` | adopted | #1448 | Ported from #1435; outside the scan, so listed on its own |
| WandererAstro cover, box, filter wheel, rotator | `AlpacaCore/src/vendors/wandererastro/wandererastro_protocol_wrapper.cpp`, `AlpacaCore/src/vendors/wandererastro/wandererastro_box_protocol_wrapper.cpp`, `AlpacaCore/src/vendors/wandererastro/wandererastro_filterwheel_protocol_wrapper.cpp`, `AlpacaCore/src/vendors/wandererastro/wandererastro_rotator_protocol_wrapper.cpp` | adopted | #1448 | Ported from #1435 |
| ZWO mount | `AlpacaCore/src/vendors/zwo/zwo_mount_protocol_wrapper.cpp` | adopted | #1448 | Ported from #1435 |
