### Fixed
- **ZWO AM: coherent Park and FindHome completion** (AlpacaCore, issue #1704): AtPark and AtHome no longer report completion while the existing five-second startup guard still reports Slewing; completion still requires accepted stopped telemetry, and Park errors remain visible immediately.
- **ZWO AM: completion uses accepted post-guard telemetry for every motion window** (AlpacaCore, issue #1704): GOTO, MoveAxis and Unpark share this rule; a successful AbortSlew during any guarded window clears the command error immediately, but Slewing remains true until a subsequent accepted stopped sample, normally the next poll.

### Fixed (tests)
- **ZWO AM: wait for telemetry polling after GOTO setup** (issue #1704): fault-countdown and retargeting tests wait for polling to resume instead of treating a completed protocol exchange as worker completion; fault tests also verify that three clock steps perform exactly three polls.
