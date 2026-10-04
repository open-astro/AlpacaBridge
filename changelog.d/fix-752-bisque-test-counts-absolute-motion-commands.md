### Fixed (tests)

- **Bisque non-finite argument test counts motion commands as a delta** - Bisque telescope, test only. The case now compares `motion_commands_sent()` before and after each step instead of assuming the connect handshake sent none.
