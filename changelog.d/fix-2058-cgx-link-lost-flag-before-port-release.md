### Fixed

- **Celestron CGX link loss releases the serial port before it reports Disconnected**, Celestron CGX driver, issue #2058. `CelestronCgxProtocol::lose_link_locked()` now closes the port (TIOCEXCL and the serial-port registry claim) before it latches `link_lost_`, so a client that sees `Connected` turn false can reopen the port at once. The existing `util::is_serial_port_in_use` assertion in `AlpacaCore/tests/test_celestron_cgx_telescope.cpp` covers the order.
