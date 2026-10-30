### Fixed
- **Cap the WeeWX response size and timeout** (WeeWX observing conditions, issue #1718): a fetch whose response body exceeds 1 MiB now fails like any other fetch error instead of buffering without limit, and `timeoutMs` above 60000 is refused ("timeoutMs must be at most 60000", InvalidValue) instead of letting one fetch block for up to 24 days.
