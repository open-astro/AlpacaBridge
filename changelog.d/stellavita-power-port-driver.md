### Changed

- **The ToupTek StellaVita power-port switch now uses the shared power-port Switch driver** (AlpacaCore, issue #2264 follow-on). It was the last GPIO power-port board with its own copy of the connect gates, power-on state, value checks and error order. Its factory now builds the shared driver over a StellaVita board profile, and its power box (or a test's injected one) stays its own. Clients see no change: the same name, description, UniqueID, capabilities, error codes, catalog fields and saved configs.
