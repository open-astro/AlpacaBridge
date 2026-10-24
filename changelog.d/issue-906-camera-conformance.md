### Breaking changes
- **Camera ImageArrayVariant no longer returns pixels** (AlpacaHTTP, issue #906): affects clients using ImageArrayVariant with any camera vendor. **After upgrading:** request ImageArray instead, using JSON or ImageBytes.

### Fixed
- **Camera ImageArrayVariant follows the Alpaca contract** (AlpacaHTTP, issue #906): returns PropertyNotImplemented before image retrieval or serialization, regardless of the Accept header; disconnected cameras return NotConnected. For affected clients, see Breaking changes.
- **ASI6200MM Pro camera validation**: added the full ConformU 4.5.3 report for clean code commit `cae4b7ff` on Orange Pi 6, Armbian trixie arm64, USB 3, ASI SDK 1.41.0.0: no errors, warnings, issues or timing violations; Disconnect took 0.001 seconds. Listed the camera in the supported-device matrix and report inventory, and updated the README validated-device count.
