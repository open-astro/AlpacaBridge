# CGX USB interference from another daemon's serial probe

## Incident and recovery

On 2026-10-13, bounded lab tests on the northern CGX 7.17.25 first lost an AUX
position reply during an RA tracking baseline, before the requested guide pulse
was sent. AbortSlew and Tracking=false could not recover the link. The node stayed
present and the kernel logged no USB removal. A privileged diagnostic, with the
daemon stopped, sent zero-rate/stop commands, received acknowledgements from both
motors, and confirmed no active guide/home/slew operation.

A forced idle read timeout recovered successfully, and 500 USB-backed idle reads
passed. However, a later six-second guide-cancellation test failed again. Adding
the actual reconnect exception revealed `cannot open ...: Device or resource busy`
on each recovery attempt, rather than an unexplained warning. The first fallback
diagnostic did not verify both axes; a subsequent stop-only diagnostic received
all four stop/zero-rate acknowledgements and confirmed both controllers idle.

These failed motion tests are not passes, and a clean rerun did not resolve the
underlying defect.

## Confirmed competing owner

`strace` on the running OpenAstro Ara service (main PID 2357960) captured its thread
2749988 doing the following on `/dev/ttyACM1`, the CGX USB endpoint:

```text
openat(..., "/dev/ttyACM1", O_RDWR|O_NOCTTY|O_NONBLOCK|O_CLOEXEC) = 323
ioctl(323, TIOCEXCL) = 0
ioctl(323, TCSETS, {... c_cflag=B9600|CS8|CREAD|CLOCAL ...}) = 0
ioctl(323, FIONREAD, [0]) = 0   # repeated during probing
ioctl(323, TIOCNXCL) = 0
```

The probe is intermittent, so point-in-time `fuser` checks had shown no competing
owner. The Ara service runs as `openastroara`, with only CAP_SYS_TIME in its
capability bounding set; it cannot bypass TIOCEXCL with CAP_SYS_ADMIN. GPSD was
checked separately and held only the configured u-blox receiver at `/dev/ttyACM0`.

## Fix and sibling sweep

The CGX wrapper previously had only an in-process registry. Its kernel tty remained
available to external probes, which could change line settings, consume replies,
and claim exclusivity while AlpacaBridge already held its original descriptor.

The raw USB wrapper now checks TIOCEXCL immediately after opening, before line
configuration. One cleanup region covers every subsequent initialization failure,
including cache allocation, dropping exclusivity, closing the fd and removing the
registry claim. Disconnect and reconnect explicitly drop TIOCEXCL with TIOCNXCL
before closing. Stop recovery now logs the thrown reconnect reason.

Sibling set: initial open/configure, every post-open failure, explicit disconnect,
destructor (delegates to disconnect), same-wrapper reconnect, and the emergency
stop reopen path. All use the same ownership lifecycle. No vendor SDK, HTTP API,
or OpenAstro Ara configuration change is part of this fix.

## Regression

`Celestron CGX AUX link - claims kernel exclusive ownership and releases it on reconnect`
holds an observer fd open, checks TIOCGEXCL, checks a non-root second open returns
EBUSY, reconnects the wrapper, then checks exclusivity clears and a new open works.
The observer prevents last-close behavior from hiding a missing TIOCNXCL. The old
code fails the claim and second-open assertions. Removing the TIOCEXCL claim or
the disconnect TIOCNXCL is a falsifying mutation. Root runs still check the kernel
flag and release, but skip the second-open refusal because root may bypass it.

This record requires post-fix rig verification before claiming the cancellation
or small-slew-abort tests passed.

## Post-fix verification

- Native arm64 focused suite with the ownership regression: 58 cases / 704
  assertions passed. Later stop/Sync additions expanded it to 62 / 725.
- The kernel-exclusive build was installed on the Pi. While connected, a
  competing non-root open of the actual by-id CGX endpoint failed with EBUSY.
  The capability-limited Ara service is subject to the same kernel check.
- Repeating the formerly failed long-guide cancellation returned successfully:
  a requested six-second North guide was cancelled in its first packet;
  AbortSlew returned in 2.2856 s, total guide movement was +19.6981430 arcsec,
  IsPulseGuiding became false, and subsequent drift was one encoder count.
  No remaining chunks ran. Tracking remained active at an approximately sidereal
  encoder rate, then was explicitly disabled before FindHome returned the mount.
- An idle disconnect released exclusivity, a non-root open then succeeded, and
  reconnect reclaimed the link. RA/Dec refused the intentionally reset home
  reference until FindHome restored it.
- A 30-second targeted Ara trace during the successful cancellation contained no
  new open of the CGX endpoint; it therefore is not evidence of a rejected Ara
  probe itself. The manual competing-open test is the rig refusal evidence.

Guide cancellation waiting for the active motor-timed packet is intentional here,
not proof that AbortSlew always meets a one-second conformance timing target.
