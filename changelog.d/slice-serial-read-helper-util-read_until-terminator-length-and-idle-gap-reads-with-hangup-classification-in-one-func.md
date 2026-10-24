### Changed

- **Serial read helper**, Gemini focuser, Gemini flat panel, iOptron iEAF and iEFW, issue #1604. The four copies of the read-to-`#` loop and the one-byte ack drains are replaced by one `util::read_until()` (`AlpacaCore/include/alpacacore/util/serial_read.h`, tested in `AlpacaCore/tests/test_serial_read.cpp` over a pty pair and a socketpair). A timeout and an over-long reply throw the same text as before; a reply read that hits a hung-up port now throws `serial link lost (errno N)` instead of waiting out the timeout. The link is not latched or disconnected by this change.
