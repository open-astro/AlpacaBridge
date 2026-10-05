### Fixed (tests)
- **Sky-Watcher Tracking=false between a pulse restore ":I" and ":J" is now covered** (AlpacaCore tests): the fake mount can withhold the reply to one command (`hold_reply_to`), and a new case parks the pulse task after the restore ":I" and checks `Tracking=false` returns, RA stops and Tracking reads false. No driver change: the suspected race did not reproduce in 50 runs.
