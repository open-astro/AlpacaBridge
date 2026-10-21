### Fixed (tests)

- **QHY CFW3 reconnect-mid-move test no longer flakes under the coverage build**, issue #976: the case waits until the fake wheel has read the goto byte before it disconnects, because on a pty `tcdrain` returns while the byte is still unread and the held-port reconnect's `tcflush` could discard it (`AlpacaCore/tests/test_qhy_cfw3_filterwheel.cpp`, "A reconnect mid-move waits out the arrival byte"). Test-only; the Sky-Watcher UDP link-health case of the issue is not reproduced and stays open.
