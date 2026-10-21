# CGX direct-USB identity evidence

On 2026-10-13, a CGX on the RPi4 USB port answered three repeated read-only
identity rounds at 115200 baud, application source `0x00`. Every accepted frame
had a validated length, checksum, source, destination and command.

| Query | Request | Reply | Payload |
| --- | --- | --- | --- |
| RA firmware | `3b030010feef` | `3b071000fe07110019ba` | `07 11 00 19` |
| Dec firmware | `3b030011feee` | `3b071100fe07110019b9` | `07 11 00 19` |
| RA model | `3b03001005e8` | `3b05100005178847` | `17 88` |
| Dec model | `3b03001105e7` | `3b05110005178846` | `17 88` |
| Address 0x30 firmware | `3b030030fecf` | `3b073000fe010017b4ff` | `01 00 17 b4` |
| Address 0x31 firmware | `3b030031fece` | No reply within 3 s | — |

Independent private-protocol evidence, fetched on the same date:

- [INDI AUX commands](https://github.com/indilib/indi-3rdparty/blob/master/indi-celestronaux/auxproto.h): `MC_GET_MODEL = 0x05`.
- [INDI AUX response sizes](https://github.com/indilib/indi-3rdparty/blob/master/indi-celestronaux/auxproto.cpp): motor `GET_VER` is four bytes and `MC_GET_MODEL` is two bytes.
- [INDI model IDs](https://github.com/indilib/indi-3rdparty/blob/master/indi-celestronaux/celestronaux.h): CGX is `0x1788`.
- [INDI version formatting](https://github.com/indilib/indi-3rdparty/blob/master/indi-celestronaux/celestronaux.cpp): major/minor plus `byte[2] * 256 + byte[3]`, giving **7.17.25** for both motors.

The original driver applied an older two-byte firmware convention to raw USB
and required replies from assumed separate switch-controller addresses. These
were fake-backed assumptions, not proof of the real bus topology. Do not infer
that `0x30` is a home-switch controller from its firmware reply alone.

The modern profile validates both motor model replies before publishing
Connected. It sends only firmware and model GETs on a fresh Connect. It does not
establish a home reference, initialize encoder labels, or qualify motion. The
existing legacy two-byte/switch-probe profile is retained without any new hardware
claim. Only model `0x1788` is verified here; do not guess a modern CGX-L ID.

Regression coverage tests the version build's high byte, exact motor model on
either axis, malformed lengths, mixed versions, and the read-only handshake.

## Verification

- Native arm64 focused CGX suite (driver, protocol, geometry and modern-profile
  lifecycle/destruction stress): 57 cases, 685 assertions passed.
- Final driver-only rebuild after exact error-code assertions: 28 cases, 512
  assertions passed. The new positive identity case linked against the pre-fix
  library fails with the original malformed motor identity error.
- clang-format 18, changelog fragment validation and documentation drift passed.
- The rebuilt USB server was installed on the Pi; installed/build SHA-256 matched.
  `PUT /api/v1/telescope/0/connected` returned ErrorNumber 0, and Connected read true.
  Management reported `RA 7.17.25, Dec 7.17.25`.
- Read-only API checks returned Slewing/Tracking/IsPulseGuiding false and the saved
  GPS site coordinates. These are driver-state reads, not independent physical
  tracking measurements. AtHome and AtPark are false; RA/Dec correctly refuse
  because this session has no encoder reference. DeviceState omits those unknown
  coordinates and keeps the available operational fields.
- No FindHome, slew, tracking, guide, Sync or Park commands, and no ConformU run.
  Connection evidence does not qualify motion or geometry.

The bullets above describe the initial connection-only checks. Later supervised
CGX lab motion and guide measurements, including a polarity failure and its fix,
are recorded in [the Dec guide polarity record](failures/0012-cgx-dec-guide-polarity.md).
The subsequent [larger-movement session](celestron-cgx-usb-motion.md) records both
flip directions, the +60-degree target grid, BeyondPole guiding, recovery fixes
and the stopped first-issue ConformU run.
