### Breaking changes

- **Two kinds of malformed request that used to succeed are now refused** (AlpacaHTTP router, issues #1476, #1486). A JSON-body integer parameter with a fraction or outside `int` (`{"Position": 0.5}`) now answers `InvalidValue` where it was silently truncated or wrapped and applied. A `PUT`, `POST` or `DELETE` to `/management/apiversions` or `/management/v1/configureddevices` now answers HTTP 405 where it returned the listing. **After upgrading:** a client that sends integer parameters as JSON sends whole numbers within `int`, and reads these two paths with `GET`. The Alpaca API sends `PUT` parameters form-encoded, a path this does not change, and the web UI only reads these two paths with `GET`.

### Fixed

- **A JSON-body integer parameter that is not a whole `int` is refused** (AlpacaHTTP router, issue #1476): `0.5`, `4294967296` or `-2147483649` in a JSON body now answer `InvalidValue`, as the query and form paths already did (see Breaking changes). Before, the JSON path truncated `0.5` to 0 and wrapped values outside `int`, in all six device dispatchers (telescope, camera, switch, filter wheel, focuser, cover calibrator). One helper, `json_int_value()`, replaces the eleven copies; a whole number written as `0.0` is still accepted.
- **`/management/apiversions` and `/management/v1/configureddevices` answer 405 to a verb other than GET** (AlpacaHTTP router, issue #1486), with `InvalidOperation` in the envelope, as the other GET-only management paths do (see Breaking changes).
- **A description write the server cannot interpret answers HTTP 400** (AlpacaHTTP router, issue #1476): a missing body, invalid JSON, no known property or an invalid field value now get 400, like the `AllowedHosts` and self-lockout refusals. A valid write that fails still answers HTTP 200 with an error, and the refused write logs at DEBUG instead of ERROR.

### Fixed (tests)

- **The iGuider lost-link cases wait for the transport to close** (iOptron iGuider tests, issue #1487): the driver publishes the loss before it closes the fd, and the two cases checked the handle count at once, which failed most all-vendors CI runs since #1464. The fake gains a `close_delay` hook that holds the window open, so the old check fails every time and the new wait passes.

### Changed (docs)

- **Decision records and HTTP instructions follow the code** (docs, issues #1476, #1479, #1486): decision 0006 credits the ~5 s call budget to ConformU, since ASCOM sets no per-call limit; decision 0007 lists the Host allowlist 403 on `/management/apiversions`, the new 405s and the wider description 400; decision 0018 rule 3 names `faileddevices`, which lists failed-to-load devices since #576; the AlpacaHTTP conformance instructions say a missing or malformed parameter on a known method is HTTP 200 with `InvalidValue`.
