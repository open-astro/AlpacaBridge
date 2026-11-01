### Fixed

- **A `%` without two hex digits in a GET query or PUT form value stays literal** (AlpacaHTTP, issue #1693): `50%off` and `%4G` no longer decode to NUL-containing bytes, and a key such as `%zz` can be looked up again; both paths now share `alpacahttp::util::url_decode`. Covered by `case_url_decode_invalid_and_trailing_escapes` in `test_device_call.cpp`.
