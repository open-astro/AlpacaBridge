### Changed

- **The SynScan and Celestron NexStar wire helpers live once**, `util/nexstar_frame.h`, issue #1757. `parse_mode_byte`, `parse_axis_pair_response` and the set-location command (`location_command`) move out of both protocol wrappers, next to the hex helpers that #1741 already shared, with their first table tests. The Celestron copy of the axis-pair parser lacked the SynScan #785 trim, so a stale hex byte left in front of a precise position reply could shift the Celestron position by one digit; both mounts now drop it.
