### Fixed
- **SVBONY camera: a link loss no longer closes the camera under a pulse in flight** (AlpacaCore, issue #2748): a getter that found the camera removed closed the SDK handle at once, even while `SVBPulseGuide` still ran on it. While a pulse may be in the SDK, a loss now closes nothing, and the pulse body closes the camera once, after its pulse returns. A loss the pulse finds itself behaves the same, and an exposure body that meets a loss during a pulse leaves the close to the pulse body. With nothing in flight, a loss still closes the camera at once. Checked on the fake SDK only.

### Added (tests)
- **SVBONY loss-during-pulse cases** (issue #2748): a getter's loss during a held pulse closes the camera only after the pulse returns (or after a Disconnect joins it), a loss the pulse finds closes the camera once, and an exposure body's loss during a pulse leaves the close to the pulse body. The fake SDK counts every close of an id a pulse still runs on.
