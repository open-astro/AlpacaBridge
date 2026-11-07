### Fixed

- **ZWO AM: a client write or MoveAxis stop no longer waits behind the whole telemetry poll cycle** (AlpacaCore, issue #2165): the poll's queries re-took the mount link before a waiting client command could, so over a slow Wi-Fi link a write (GuideRate, Tracking, SiteLatitude, UTCDate) or a `MoveAxis(…, 0)` stop waited 2–3.5 s. The poll now steps aside before each query while a client command is waiting, so the command goes out after at most the one query already in flight. USB timing is unchanged.
