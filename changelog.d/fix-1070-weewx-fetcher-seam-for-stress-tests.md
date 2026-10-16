### Added (tests)

- **WeeWX fetcher seam for the stress suite** (AlpacaCore, issue #1070): `create_weewx_observingconditions` has an overload taking a `WeeWxFetcher`, and the existing overload delegates to it with the libcurl fetch, so production is unchanged. The `[stress]` cases now run over a scripted `FakeWeeWxFetcher`, so connect succeeds and the poll thread runs during the storm (asserted by `fake.calls() >= 2` and a canned temperature read), and a new case disconnects while a slow fetch is in flight.
