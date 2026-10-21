### Breaking changes
- **Celestron CGX USB: a stop takes priority over a guide-to-motion handoff** (AlpacaCore, issue #1156): an initiator interrupted by an accepted AbortSlew or Unpark now returns InvalidOperation instead of starting its motion after the guide drains. **After upgrading:** clients issuing concurrent commands must wait for Slewing to become false before deliberately issuing another motion request.

### Fixed
- **Celestron CGX USB: accept AbortSlew during motion handoff** (AlpacaCore, issue #1156): queue the existing asynchronous stop while a motion initiator drains a guide packet, coalesce repeated stop requests, and preserve stop ownership through the interrupted handoff; see Breaking changes for concurrent callers.
- **Telescope DeviceState: include known UTCDate** (AlpacaCore/AlpacaHTTP, issue #1156): include UTCDate when its getter succeeds and use the same UTC string formatter for DeviceState and the individual UTCDate endpoint; failed reads remain omitted and disconnected devices still return an empty state list.
- **Telescope DeviceState: keep coordinate reads together** (AlpacaCore, issue #1156): read live guide status after the coordinate properties so its serial round trips do not unnecessarily expire the CGX stationary coordinate cache mid-response; cache lifetime, link-health checks, control reads and returned fields are unchanged.

### Fixed (diagnostics)
- **Celestron CGX USB: retain the first AUX transaction failure in logs** (AlpacaCore, issue #1156): report command/address, elapsed time, timeout budget and original exception before later link-health or cleanup failures obscure the cause; protocol errors, retries, deadlines and guide behavior are unchanged.
