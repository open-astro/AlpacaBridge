### Changed (validation)

- **Celestron CGX direct USB ConformU report refreshed on v6.0.0-beta.2** (issue #2119): ConformU 4.6.0-rc.1 passes with 0 errors, 0 warnings, 0 issues and no timing misses on the bare northern CGX, on the v6.0.0-beta.2 release package (`stable/6.0` `61cade85`) on the OPi6-class SBC with the CGX on `cdc_acm`. Both guide rates were at the 0.5x default and every guiding check passed, so the beta.1 and beta.2 notes' "0.25x passed, 0.5x stopped at a guiding check" no longer holds. `AlpacaCore/conformu/Celestron/CGX/Linux-arm64-usb.txt`.
