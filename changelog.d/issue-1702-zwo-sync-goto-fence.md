### Fixed
- **ZWO AM: keep Sync and GOTO targets coherent** (AlpacaCore, issue #1702): Sync cancels pending GOTO setup and retries before changing the mount's target. Both legacy Sync fallbacks and each GOTO attempt serialize their target writes against other target writers; retries restore their captured coordinates rather than consuming another command's target.
