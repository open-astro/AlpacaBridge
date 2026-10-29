### Fixed

- **Sky-Watcher stale link verdict closing a new session**, Sky-Watcher driver, issue #1707. A status poll that saw a stale fault on the old link could latch "lost" on the session a relink had just opened; the stale verdict is now re-checked under the health lock before it latches, so the new session stays connected with no lost-at stamp or kept fault text. Covered by "SkyWatcher link loss - a stale verdict does not latch a session relinked after it" in `AlpacaCore/tests/test_skywatcher_link_loss.cpp`.
