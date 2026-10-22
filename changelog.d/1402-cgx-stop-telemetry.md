### Fixed
- **Keep CGX stop progress responsive** (Celestron telescope, issue #1402): let Slewing report a pending AbortSlew or Unpark without waiting behind stop-command exchanges, and check the intermediate state with a held-ACK regression rather than a scheduling-dependent assertion.
