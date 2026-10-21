### Changed (tests)

- **Celestron retried async slew now requires exactly one GOTO** (AlpacaCore tests, PR #857): the case "Celestron async - a GOTO the hand controller never acknowledges surfaces through Slewing (#575)" checks `goto_count == 1` after the slew completes, so a duplicate GOTO at any time during the retried slew fails the test. PR #857 had relaxed the GOTO wait from `== 1` to `> 0` without a stated reason, and a wait alone stops reading the count once it is satisfied.
