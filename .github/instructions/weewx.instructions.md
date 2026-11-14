---
applyTo: "AlpacaCore/src/vendors/weewx/**,AlpacaCore/include/alpacacore/vendor/weewx/**,AlpacaCore/tests/*weewx*,AlpacaCore/conformu/WeeWX/**"
---

### WeeWX

Devices: ObservingConditions.

No external SDK — reads weather data from a local WeeWX weather station instance.

#### Link loss (decision 0009, issue #1984)

The feed is HTTP polled every `poll_interval` (900 s by default), so the shared link-health rules apply with two
WeeWX deltas ([decision 0009](../../docs/decisions/0009-link-loss-and-relink-policy.md), WeeWX Adoption row):

- **No HTTP failure is a loss by itself.** A libcurl error (refused, dropped or timed-out connection), a status of 400
  or more and a body that does not parse each count as one failed poll fetch for `util::PolledLinkHealth`: a web
  server restart is fixed by the next poll. Three in a row latch the fault (sensor reads and `TimeSinceLastUpdate`
  throw `DriverException` with the fault text, `Connected` stays true). Only the poll counts: a failed `Refresh`
  reports its own error, a good one clears the fault.
- **The staleness bound is `max(util::kLinkStalenessBound, 2 x poll_interval)`**, from the latch stamp on the task
  clock, checked after each failed poll. The 30 s default would lose a 15-minute feed at its first poll after the
  latch. With the default interval the feed latches at the third failed poll and is lost at the fifth: `Connected`
  false, every member `NotConnected`, the poll ends and the cached snapshot is dropped. Only the client's Connect
  relinks; `get_link_fault()` keeps the fault text until the next Connect or Disconnect.

Tests run the poll on `FakeTaskClock` (`test_weewx_observingconditions.cpp`, with `FakeWeeWxFetcher` or the loopback
`FakeWeeWxFeed`, whose libcurl fetch the production factory reaches through its trailing clock argument); the
staleness loss under contention is a `[stress]` case in `test_weewx_concurrency_stress.cpp`.
