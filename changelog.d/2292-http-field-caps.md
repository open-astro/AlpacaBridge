### Changed

- **Byte and count bounds on client input** (AlpacaHTTP router and the device catalog, issue #2292, ADR 0007 rules 4 and 10):
  - `Command` (`CommandBlind`, `CommandBool`, `CommandString`) over 1024 bytes, an `Action` name over 256 bytes and its `Parameters` over 8192 bytes are refused with `InvalidValue` before the driver is called.
  - A `Location` or `ProfileName` over 256 bytes in a `/management/v1/description` write is refused with 400, as the write's other invalid values are.
  - A switch name (`SetSwitchName`) or a filter name (`PUT names`) over 256 bytes is refused with `InvalidValue` before the driver is called.
  - A device holds at most 256 client registrations (address and ClientID). A new client's `Connected=true` past that is refused with `InvalidOperation` until a client leaves or goes 10 minutes without a request; a registered client is unaffected.
  - `configuredevice` keeps at most 128 entries. A new entry past that is refused with `InvalidOperation` until one is removed.
  - A `host` over 253 bytes or a `portPath`/`hidPath` over 4095 bytes is refused from `configuredevice`. A saved entry keeps the value with a warning, so its connect refuses it.
  - The network mounts (Bisque, Celestron, iOptron, Sky-Watcher, SynScan, ZWO) refuse a `tcpPort`/`udpPort` outside 1 to 65535 for a network entry, as OnStep already did. A saved entry keeps the port with a warning and connects to port 0, which fails, instead of the port wrapping to another number.
