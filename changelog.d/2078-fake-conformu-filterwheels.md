### Changed (CI)

- **Fake ConformU: three filter wheels run**, issue #2078 (slice 4, filter wheels). `fake_conformu_server` hosts the ZWO EFW, QHYCFW3, Player One Phoenix and WandererAstro SFW drivers over their fakes (`AlpacaCore/tests/conformu_filterwheel_factories.h`); the two SDK wheels report Position -1 for three reads after a move, so ConformU sees the wheel turn. The ZWO, QHY and Player One rows run, with their reports under each vendor's `Fake` folder. The WandererAstro row stays pending: its fake streams one fixed status frame and never applies a move. Fake evidence only.
