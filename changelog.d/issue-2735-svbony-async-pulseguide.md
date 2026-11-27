### Fixed
- **SVBONY camera: PulseGuide returns at once** (AlpacaCore, issue #2735): `SVBPulseGuide` returns only after the pulse, and PulseGuide ran it on the request thread, so every pulse took as long as the pulse and a 2 s pulse missed ASCOM's 1 s response target. The SDK call now runs in a background operation slot, as on the QHY camera: IsPulseGuiding reads true until the pulse ends, a second PulseGuide while one runs returns at once and guides after it, a failed pulse is raised by IsPulseGuiding until the next PulseGuide or a reconnect, and a Disconnect waits for the pulse in flight. Checked on the fake SDK only: no SVBONY camera with pulse guiding has a ConformU report.

### Added (tests)
- **SVBONY async PulseGuide cases** (issue #2735): PulseGuide returns before the pulse ends on the fake SDK's clock, a Disconnect joins the pulse body, a second pulse queues, and a failed pulse is kept; the fake ConformU SVBONY camera row now runs, with its report.
