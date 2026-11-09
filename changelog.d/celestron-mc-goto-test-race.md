### Fixed (tests)
- **Celestron completion-poll test no longer races the second axis GOTO** (AlpacaCore): the test muted the fake mount after the first motor-controller GOTO, so the second GOTO could time out and the driver correctly reported a dispatch failure instead of the completion-poll failure the test waits for. The test now waits for both GOTOs.
