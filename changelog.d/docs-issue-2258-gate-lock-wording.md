### Changed (docs)

- **Telescope command gate: the header says which lock a driver holds when it calls the gate** (AlpacaCore, issue #2258): it said a driver calls the gate under its own state lock, which one adopter cannot do because its check callbacks take that lock themselves. It now says a driver holds the lock its check callbacks expect: the state lock when they read under a lock the caller holds, none when each takes it itself. Comment only.
