### Fixed (tests)

- **Contract sweep parks the Sky-Watcher and CGX rows on the fake clock** (tests, issue #2584): the "InvalidValue wins while parked" case now builds the Sky-Watcher UDP and serial rows and the CGX USB and TCP rows on a `FakeTaskClock` and steps it until AtPark, as the ZWO and OnStep rows already did, instead of waiting out the drivers' park timers in real time (decision 0005 rule 2). The case also steps the clock through the disconnect at its end.
