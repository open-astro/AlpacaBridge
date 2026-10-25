### Fixed (tests)

- **Astroasis focuser failed-move tests no longer flake**, Astroasis focuser, issue #1647. The two tests `the next poll measures the device after a failed move` and `... after a failed halt` accept the held frame (1234) or the overlapped poll's post-exchange measurement (5555) right after the failed command, because that poll sampled after the exchange released the wrapper; they still require the next poll to publish 5555 with the right moving state. Test-only change in `AlpacaCore/tests/test_astroasis_focuser.cpp`; the driver is unchanged.
