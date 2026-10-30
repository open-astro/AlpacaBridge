### Fixed

- **A ClientID that is not a uint32 no longer gets its own per-client Connected registry entry** (AlpacaHTTP, issue #1738): `extract_client_key` counts a ClientID only when it is 1-10 digits with a value up to 4294967295 (a JSON integer from 0 to 4294967295); anything else, such as a 60 KiB string or `4294967296`, registers in the anonymous slot like a request with no ClientID, so one client cannot hold unbounded memory for the 10-minute stale window. Pinned by the issue #1738 block in `test_routing.cpp`.
