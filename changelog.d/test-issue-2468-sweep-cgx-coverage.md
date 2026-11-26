### Added (tests)

- **Contract sweep parks the ZWO, OnStep and CGX fakes** (tests, issue #2468): each of these fakes now reaches AtPark after a Park, so the sweep's "InvalidValue wins while parked" case runs on those rows (USB and TCP for the CGX) instead of skipping them. The parked case waits at most 15 s for AtPark and 5 s for each probe, so a probe a driver accepted cannot stall the sweep. A new CGX case parks through a crawling RA home search and checks the one re-home and the position and elapsed time its INFO line reports.
