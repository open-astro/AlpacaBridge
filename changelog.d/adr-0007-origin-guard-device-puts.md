### Security

- **Every Device API PUT now carries the cross-origin guard** (AlpacaHTTP, decision 0007 rule 2). A PUT to `/api/v1/<type>/<n>/<method>` whose `Origin` host differs from `Host` gets HTTP 403 with `ErrorMessage` "Cross-origin device method requests are not allowed" before the driver runs; before, only the wrong-verb path, `UTCDate` and the three `Site*` setters were guarded, so a slew, exposure, switch or `Connected` PUT reached the driver. Requests with no `Origin` header (NINA, ConformU, other native clients) and same-origin web UI requests are unchanged, and GETs are never refused. The `UTCDate` and `Site*` refusals now say "device method" instead of naming the member.

### Added (tests)

- **Cross-origin device PUT case** (decision 0007 rule 2): `test_routing.cpp` sends a cross-origin, a same-origin and a no-Origin PUT to telescope `slewtocoordinatesasync`, camera `startexposure`, focuser `move`, switch `setswitch`, `connected` and `action`, and checks the 403 never reaches the driver.
