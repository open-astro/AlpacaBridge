### Fixed

- **Sky-Watcher direct: Tracking=true during a guide pulse no longer leaves RA stopped** (issue #821). An RA pulse dispatched with Tracking off stopped the RA axis at its end even when the client had set Tracking=true during the pulse, so Tracking read true while RA was stopped. The pulse end now restarts the RA drive when Tracking is on.
