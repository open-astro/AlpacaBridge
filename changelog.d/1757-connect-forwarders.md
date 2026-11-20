### Changed

- **One macro writes every driver's connect forwarders** (AlpacaCore, issue #1757 slice 2b). The 43 drivers that mix in `AsyncConnectable` wrote `connect()`, `disconnect()` and `get_connecting()` by hand; they now use `ALPACA_ASYNC_CONNECT_FORWARDERS()` from `async_connectable.h`, and `scripts/check_connect_error_hook.py` fails a driver without it, as it already does for `ALPACA_EXPOSE_CONNECT_ERROR()`. No behaviour changes.
