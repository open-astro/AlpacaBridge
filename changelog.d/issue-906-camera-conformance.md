### Breaking changes
- **Camera ImageArrayVariant no longer returns pixels** (AlpacaHTTP, issue #906): affects clients using ImageArrayVariant with any camera vendor. **After upgrading:** request ImageArray instead, using JSON or ImageBytes.

### Fixed
- **Camera ImageArrayVariant follows the Alpaca contract** (AlpacaHTTP, issue #906): returns PropertyNotImplemented before image retrieval or serialization, regardless of the Accept header; disconnected cameras return NotConnected. For affected clients, see Breaking changes.
