### Fixed
- **ToupTek and Altair cameras keep the pull-mode stream running between successful exposures** (AlpacaCore, issue #1396): a completed exposure thread remains joinable, which made the next StartExposure and an idle StopExposure or AbortExposure call Toupcam_Stop and force a stream restart; completed frames now reuse the established stream, while active, failed, timed-out, aborted or reconfigured exposures still stop and re-initialise it.

### Added (tests)
- **ToupTek persistent-stream regression** (AlpacaCore, issue #1396): a fake-SDK case covers two completed exposures followed by PHD2's idle AbortExposure and requires one StartPullMode call with no intermediate Stop.
