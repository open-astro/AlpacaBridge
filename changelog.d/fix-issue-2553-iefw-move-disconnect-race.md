### Fixed

- **iOptron iEFW: a move racing a Disconnect reports NotConnected**, issue #2553. A `Position` write that passed the connection check just as a Disconnect cleared the wheel's slot count threw `DriverException` "slot count unavailable"; the connection is now checked under the same lock that the Disconnect clears the slot count under. The iEFW lifecycle stress case failed on this about 1 run in 300 under load.
