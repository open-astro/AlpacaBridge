### Fixed

- **A SynScan Park no longer stops the mount on a single dropped status reply** (SynScan, issue #1122): the Park status poll now tolerates two consecutive failed `L` queries and fails the Park on the third, resetting the count on any good reply; `Slewing` and the synchronous slew wait already kept the last state on a failed poll. Covered by the two Park cases in `test_synscan_async_park.cpp`.
