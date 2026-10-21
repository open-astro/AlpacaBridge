### Fixed

- **OnStep: DoesRefraction reports what OnStepX actually does** (OnStep, issue #1216): the driver answered `False` from a local flag, and a write only changed that flag, although OnStepX built with topocentric coordinates (the default, for example the TeSeek HM-17) applies refraction to every RA/Dec it reports; at connect the driver now reads `:GXEE#` on OnStepX (firmware 10 or later) and reports `True` for topocentric and topocentric-strict builds and `False` for observed-place builds, a write of the other value throws `PropertyNotImplemented` because the mode is fixed when the firmware is built, legacy OnStep and an unanswered `:GXEE#` keep the settable flag, and reading or writing the property while disconnected now throws `NotConnected`. Not a breaking change: the old `False` was wrong and the old write never reached the mount, so no client could rely on either.

### Added (tests)

- **OnStep DoesRefraction cases** (issue #1216): topocentric, topocentric-strict and observed-place firmware, an unanswered `:GXEE#`, legacy firmware that must not be asked, the refused write, and `NotConnected` before connect and after disconnect, over the loopback fake mount.
