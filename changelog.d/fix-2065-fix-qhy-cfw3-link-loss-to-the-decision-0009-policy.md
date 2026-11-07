### Fixed

- **QHY CFW3 link loss follows decision 0009**, QHY CFW3 filter wheel, issue #2065. After the serial port dies, `get_link_fault()` now gives one text (`QHYCFW3 link lost on <port>`) until the next Connect or Disconnect, and the loss is stamped once (`util::LinkLostStamp`). `Connected` false and `NotConnected` members were already the behaviour. Covered by `QHY CFW3 Filter Wheel Driver - Link loss follows decision 0009 (#2065)` in `AlpacaCore/tests/test_qhy_cfw3_filterwheel.cpp`; the policy is [decision 0009](docs/decisions/0009-link-loss-and-relink-policy.md).
