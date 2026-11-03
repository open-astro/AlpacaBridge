# iOptron iFR focuser-rotator: serial protocol

The iFR (2", 3", 4") is an electronic focuser and a camera angle adjuster (CAA, the rotator) in one body. AlpacaBridge
drives it as two ASCOM devices, a Focuser and a Rotator, over one serial link (`ioptron_ifr_protocol_wrapper.cpp`).
This is our own summary; no vendor code is reproduced.

## Sources

- **Rig captures** on an iFR 3" lent by iOptron, 2026-11-03: focuser and CAA firmware `260324`, `:DeviceInfo#` model
  05. Every reply format, ack and behaviour below marked "rig" was observed on it; the probe scripts and logs were run
  from a Debian 13 arm64 VM over USB passthrough.
- **INDI** `drivers/rotator/iafscaa.cpp` (iOptron iAFS Focuser Rotator, merged 2025-10-28): command set and field
  layout. It does not read the one-byte acks and hard-codes a 0..99999 focuser range; both are corrected below.
- **iOptron Commander and ASCOM Alpaca AIO Installer 10.0.2** (2026-10-10), which carries iOptron's own unified Alpaca
  driver ("Merged mount, focuser, filter wheel, and rotator driver into a single unified driver", 10.00). The program
  is a .NET NativeAOT build, so it cannot be decompiled; only its command literals and UI strings were read. It lists
  commands INDI does not use (`:FW2#`, `:FW3#`, `:RG`, `:RT`, `:RT4#`, `:RT5#`, `:RAS#`); their meaning is unknown and
  the driver sends none of them.
- The product page (ioptron.com.cn, "iFR系列 调焦站"): 25/25/26 mm travel, 0.4 um minimum focuser step, CAA
  0.004 deg/step and < 0.1 deg positioning, 6/4.5/3.4 deg/s maximum rotation speed, both Type-C ports usable as the
  control port with no extra power.

## Link

- **Two Type-C ports, either one controls both halves** (product page; rig). Each port is its own Prolific USB-serial
  chip, `067b:23a3` (`lsusb`: "ATEN Serial Bridge"; the descriptor: "USB-Serial Controller"), the same IDs as the
  iAFS2/3. **Both chips carry the same serial number** (`AHAOe14A312` on the rig unit), so udev gives both one
  `/dev/serial/by-id/` name and only one symlink survives; the scan's raw `/dev/ttyUSB*` pass finds the other. With
  one cable plugged in (the normal case) the unit is one port. VMware Fusion refuses to pass both chips to a VM at
  once (identical vendor, product and serial).
- 115200 8N1, raw. No reset or boot output on open (rig: nothing arrived in the 2 s after open); INDI's 2 s
  post-open sleep is not needed.
- Commands are `:XX...#`. Queries answer with a `#`-terminated reply in 4-30 ms (rig). Motion and setting commands
  answer **one byte and no `#`**: `1` accepted, `0` refused. The ack can arrive 60 ms or more after the command (rig):
  left unread it prefixes the next status reply (`1+0232171061036053716130#`) and breaks the fixed-width parse, the
  iEAF lesson, so the wrapper always reads it.

## Handshake and firmware

| Command | Reply | Notes |
| --- | --- | --- |
| `:DeviceInfo#` | `000000050100#` | `%6d%2d%4d`: a position field (always `000000` on the iFR, whatever the focuser position), model code, build. **05 = iAFS + CAA (the iFR)**; INDI: 02 iEAF, 03 iAFS, 04 iAFS without CAA. |
| `:FW1#` | `260324260324#` | 6 characters of focuser firmware, then 6 of CAA firmware. |
| `:FW2#`, `:FW3#` | (no reply) | Not answered by the iFR (rig); probably the mount's. |

## Focuser

| Command | Reply | Notes |
| --- | --- | --- |
| `:FI#` | `+0500000299960#` | `%7d%1d%5d%1d`: position (signed), moving, temperature in Kelvin x 100, direction flag. |
| `:FM%07u#` | `1` / `0` | Move to an absolute position. Zero-padded and space-padded (`:FM%7u#`) both work (rig). |
| `:FQ#` | `1` | Halt. The moving flag drops within about 0.5 s (rig). |
| `:FZ#` | `1` | The current position becomes 0. No motion. |
| `:FR#` | `1` | Toggles the direction flag; the position does not change (rig). Not sent by the driver. |

- **`0` means "already there" or "will not go there"** (rig): a move to the current position answers `0` (0 to 0,
  500 to 500), and so does a move toward a limit the focuser sits at. A target above the firmware's 0..99999 range
  near a position above it was refused as well (the unit arrived reading 132660, outside that range, and every
  `:FM` near there answered `0` until `:FZ#`).
- **Travel stops at the limit sensors** (product page: two electronic limits). Zeroed at the inner end, the 3"
  travelled 62,822 steps to the outer stop, which ended a move to 99999 at 62892 with the moving flag dropping on
  its own: 25 mm / 62,822 = 0.398 um per step, the 0.4 um the page gives. Speed about 1,200 steps/s (rig).
- The moving flag reads 1 on the first poll after the ack and 0 at the target (rig). The internal temperature rose
  2.2 C over one full-travel run (rig), so the sensor reads near the motor.
- There is no temperature compensation in the firmware, and no command for the maximum or the step size.
- Once, before the unit was zeroed at its inner end, moves toward 0 stopped at 70 and a move to 0 from 70 answered
  `0`. Not seen again after zeroing at the inner end; noted, not explained.

## Rotator (CAA)

| Command | Reply | Notes |
| --- | --- | --- |
| `:RI#` | `+0000000000012787633130#` | `%10d%1d%9d%1d%2d`: offset (signed), moving, position, direction (1 normal, 0 reversed), size in tenths of an inch (30 = 3.0"). Angles in 0.01 arcsec (360000 per degree). |
| `:RM%09u#` | `1` | Move to an angle in the **reported** (synced, possibly mirrored) frame. |
| `:RQ#` | `1` | Halt. |
| `:RY%09u#` | `1` | Sync: the reported position becomes the argument. |
| `:RR0#`, `:RR1#` | `1` | Reversed, normal. |
| `:RS%02u#` | `1` | Body size register (20/30/40), shown and changed by the vendor app. Not sent by the driver. |

- **The offset lives in the firmware and survives power loss** (rig: a 100.0 deg sync read back unchanged after both
  cables were out for 10 s). `:RM` takes the synced angle: with the offset at +64.49 deg, a move to 101.0 deg moved
  1 deg. Position = mechanical + offset; the offset field is Position - MechanicalPosition.
- **Moves never cross 0/360 of the synced frame** (rig): 1 to 359 deg went the long way (358 deg, 83 s), 359 to
  35.5 deg the long way back (75 s); with an offset, 100 to 30 deg (synced) went straight down 70 deg though the
  mechanical angle crossed 0. Speed 4.4 deg/s (rig; 4.5 on the page). A worst-case move is about 83 s.
- **Reverse mirrors the frame** (rig): with offset U in the normal frame, reversed Position = 360 - (mechanical + U)
  and the offset field reads -U. MechanicalPosition is the physical angle in both directions.
- **A `:RY` sent while reversed stores the wrong offset** (rig): reversed, a sync to 180 deg left the unit at 50 deg
  (310 deg once back to normal) though the position read 180 right after the command. A sync made in the normal
  frame is always consistent, so the driver syncs a reversed rotator as `:RR1#`, `:RY<360 - target>#`, `:RR0#`
  (verified: a normal-frame sync to 260 deg reads 100 deg reversed).
- The very first move after the unit arrived (a +2 deg move from 35.52 deg) ran down through 0 and back up to
  37.52 deg, 73 deg in 17 s. Not seen again, including after a full power cycle; noted, not explained.
- A move to the current angle is accepted and nudges one step (rig).

## Not yet known

- What `:FW2#`, `:FW3#`, `:RG`, `:RT`, `:RT4#`, `:RT5#` and `:RAS#` do (they are in iOptron's driver; `:RT4#`/`:RT5#`
  match the model codes of the iAFS without and with CAA, so they may set the unit type: never probed).
- Whether both Type-C ports answer the full command set when both are plugged in at once (VMware cannot pass both).
- The 2" and 4" bodies: only the 3" was on the bench.
