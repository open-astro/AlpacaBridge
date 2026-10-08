### Breaking changes
- **Malformed camera frames now return an error** (AlpacaHTTP, issue #898): camera responses with invalid rank/dimensions or mismatched data length no longer produce empty, padded, or truncated success. After upgrading: fix the camera ROI or format before requesting the image.

### Changed
- **Camera ImageBytes use less memory and transfer faster** (AlpacaHTTP, issue #898): pack pixels directly into the final payload, move temporary response bodies into ownership, and send headers and bodies with one vectored write. The malformed-frame response change is described under Breaking changes.

### Added (tests)
- **ImageBytes and large binary response coverage** (AlpacaHTTP, issue #898): verify pixel ordering and element encodings, arithmetic validation, and large-body framing, keep-alive, embedded NULs, and peer disconnects.
