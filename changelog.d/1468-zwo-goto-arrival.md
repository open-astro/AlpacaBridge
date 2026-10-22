### Added (tests)
- **ZWO AM GOTO arrival ownership coverage** (AlpacaCore, issue #1468): exercise failed final RA and Dec reads after a stopped, non-tracking status; preserve retargeting until arrival is published, refuse it after arrival, and verify that an explicit Tracking=false command refuses it even before arrival.
