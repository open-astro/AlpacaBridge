### Fixed
- **SynScan and ZWO telescope position readback after a GOTO** (AlpacaCore, issue #774): report mount position feedback instead of replacing it with the requested slew destination.
- **SynScan and Celestron position readback after link failure** (AlpacaCore, issue #774): reject stale cached coordinates after failed polls and recover automatically when the handset responds again.
- **SynScan and Celestron tracking modes** (AlpacaCore, issue #774): choose Alt-Az or north/south equatorial tracking from the mount model and configured site latitude, preserve Celestron's legacy CGE mode mapping, and report alignment mode from known mount families. Ambiguous SynScan models refuse to guess; Park and SetPark retain the prior RA/Dec fallback for those models.
- **SynScan SetPark coordinate frame** (AlpacaCore, issue #774): retain the mechanical park direction as hour angle and declination for equatorial mounts, and azimuth/altitude for Alt-Az mounts.
- **Bisque PulseGuide state and duration** (AlpacaCore, issue #774): run TheSkyX DirectGuide asynchronously from the driver's perspective, expose its in-progress state, reject negative durations, and set a response timeout based on the requested duration.

### Added (tests)
- **SynScan and ZWO post-GOTO position regressions** (issue #774): cover async slews on both drivers and ZWO's blocking slew path using mounts that acknowledge GOTOs without moving.
- **SynScan and Celestron stale-position regressions** (issue #774): cover the three-failure latch, cache refusal, connected-state preservation and automatic recovery.
- **SynScan and Celestron tracking regressions** (issue #774): verify southern-hemisphere EQ and Alt-Az mode selection, including refusal to guess when SynScan's dual-mode AZ-EQ model is ambiguous.
- **SynScan SetPark regressions** (issue #774): simulate a six-hour sidereal shift, verify equatorial and Alt-Az park targets remain tied to the saved mechanical position, and preserve Park/SetPark for ambiguous and unknown models.
- **Bisque PulseGuide regression** (issue #774): check prompt initiation, observable state during a fake guide longer than the default timeout, completion and InvalidValue-before-NotConnected validation.
