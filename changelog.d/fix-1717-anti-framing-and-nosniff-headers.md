### Fixed

- **Every HTTP response now carries `X-Frame-Options: DENY`, `Content-Security-Policy: frame-ancestors 'none'` and `X-Content-Type-Options: nosniff`** (HTTP, issue #1717): `Response::to_header_string()` adds them unless the response set that header itself, so the web UI cannot be framed (clickjacking) and error responses are covered; `test_response_utf8` asserts the three values on HTML, JSON and error-style responses and that a header the response set is kept once.
