---
applyTo: "AlpacaCore/src/vendors/remotealpaca/**,AlpacaCore/include/alpacacore/vendor/remotealpaca/**,AlpacaCore/tests/*remotealpaca*,AlpacaCore/tests/fake_alpaca_server.h"
---

### remotealpaca

Devices: none yet. The vendor serves a device of an upstream Alpaca server as a proxied device on this server
(`CONTEXT.md`: native device vs proxied device). Its contract is fixed by
[decision 0023](../../docs/decisions/0023-outbound-clients-in-vendor-wrappers.md) and issue #1753; read both before
adding a device type.

No SDK: the wire is the ASCOM Alpaca Device API over `util::HttpClient`, built under `ALPACACORE_ENABLE_REMOTEALPACA`
(needs libcurl). No remotealpaca file names libcurl (decision 0023 rule 2).

#### The upstream client (`RemoteAlpacaClient`, issue #1753 slice 2)

- One client per upstream device. Calls are serialized on the client's own mutex, so a driver's poll and its
  commands never overlap on one upstream device.
- Every call sends the client's own ClientID and the next ClientTransactionID, never a caller's. Parameter names go
  out as the caller writes them, in canonical Alpaca casing.
- A non-zero `ErrorNumber` is thrown unchanged with `<host>:<port>: ` in front of the upstream's message (decision
  0023 rule 5). Every other failed call is a `RemoteTransportError` (`DriverException`); the driver decides whether
  it is a lost link (`NotConnected`) or one failed call.
- The reply's JSON is read by the wrapper's own reader in `remote_alpaca_client.cpp`; no JSON type leaves the wrapper
  (decision 0013 section 4). The per-call timeout is `kRemoteCallTimeout` (5 s), not a config field.

Tests use the loopback `FakeAlpacaServer` (`AlpacaCore/tests/fake_alpaca_server.h`, 127.0.0.1 only, with stall,
drop, HTTP 500, bad JSON and no-ErrorNumber modes) over the real libcurl client, and `FakeHttpClient` where a case
counts calls. ConformU against the ASCOM Alpaca Simulators (OmniSim) runs on the rig host, never in CI.
