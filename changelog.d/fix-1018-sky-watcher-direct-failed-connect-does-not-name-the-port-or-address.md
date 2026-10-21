### Fixed

- Sky-Watcher direct mount: a refused connect now says which serial port or host:port it tried and why (no such device, permission denied, no reply to the handshake) and which setting to check, in the error and in `LastConnectError`, instead of "Failed to connect to Sky-Watcher motor controller" (issue #1018).
