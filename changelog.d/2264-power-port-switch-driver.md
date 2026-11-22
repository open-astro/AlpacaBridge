### Changed

- **One power-port Switch driver for the ASIAIR Pro and Plus CM4, the ASIAIR Plus RK3568 and the iOptron iMate PowerBox** (AlpacaCore, issue #2264). The three drivers were near copies, so every fix to their connect gates, power-on state, value checks or error order had to land three times. They are now one class template over each board's port wrapper, and each vendor's factory gives it a board profile. Clients see no change: the same names, descriptions, UniqueIDs, capabilities, error codes, catalog fields and saved configs.
