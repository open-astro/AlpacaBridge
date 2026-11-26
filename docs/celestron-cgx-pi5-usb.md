# Celestron CGX direct USB on a Raspberry Pi 5

Issue #2234. On a Raspberry Pi 5, plugging the Celestron CGX's direct USB port (`protocol="cgx-aux"`) into
the Pi kills the Pi's whole USB controller while the stock `cdc_acm` driver binds the mount. This page
records the cause and an **opt-in** host workaround. AlpacaBridge does not install it: `scripts/host_setup.sh`
leaves the CGX on `cdc_acm`, for the reason given under [Trade-off](#trade-off).

## What happens

- The CGX enumerates as `15a2:a50f` (Freescale bridge, product `CGX`, serial `_CGX_v1`, full speed,
  self-powered). Interface 0 is CDC ACM control (`bmCapabilities 0x00`, interrupt IN 0x81), interface 1 is
  CDC Data (bulk IN 0x82, bulk OUT 0x03).
- When `cdc_acm` binds interface 0, its probe sends `SET_LINE_CODING`. The CGX never completes that control
  transfer, although its descriptor says it does not support line settings. The Pi 5's RP1 xHCI cannot stop the
  stuck transfer and gives up about 10 s after the plug-in:

  ```
  xhci-hcd xhci-hcd.1: xHCI host not responding to stop endpoint command
  xhci-hcd xhci-hcd.1: xHCI host controller not responding, assume dead
  xhci-hcd xhci-hcd.1: HC died; cleaning up
  ```

- Every device on that controller drops with it (cameras, focusers), unplugging the CGX does not bring them
  back, and only a reboot (or an unbind and bind of `xhci-hcd.N`) does. It happens with AlpacaBridge stopped:
  the bind alone triggers it, before any program opens the port. AlpacaBridge would trigger it again anyway,
  because setting the baud rate on open makes `cdc_acm` send `SET_LINE_CODING`.
- Isolated with pyusb and `cdc_acm` blocked: `GET_LINE_CODING`, `SET_CONTROL_LINE_STATE`, `GET_STATUS` and
  reads on 0x81 and 0x82 all succeed; `SET_LINE_CODING` (9600 8N1) alone times out and the controller dies.
- Reported on Raspberry Pi OS (Debian 13 trixie, arm64), kernels `6.18.34+rpt-rpi-2712` and
  `6.18.60+rpt-rpi-2712`, 9 of 9 plug-ins, on both RP1 controllers, direct and behind a USB 3.1 hub. Power was
  ruled out. The same mount and cable work on `cdc_acm` on an Orange Pi 6 Plus (Cix xHCI, kernel 6.18.45),
  the host of the published CGX ConformU report (#2224).

The cause is the kernel's `cdc_acm` and the RP1 xHCI meeting this device, not AlpacaBridge code. A kernel-side
fix (a `cdc_acm` quirk for `15a2:a50f` that skips `SET_LINE_CODING`) and a report to `raspberrypi/linux` are
outside this repository and not yet tested.

## Workaround (opt-in, Pi 5 only)

The generic `usbserial` driver sends no setup requests and uses only the data interface, which is all
`cgx-aux` needs: the baud rate does not matter on the CGX's USB link and the mount answers without DTR. The
files below keep `cdc_acm` off the CGX's control interface (`15a2:a50f`, interface 0) only; every other CDC
device (a ZWO AM mount, for example) still binds `cdc_acm`. They are the reporter's files from issue #2234,
validated on a Pi 5 with kernel 6.18.60: ConformU 4.6.0-rc.1 and the #2119 hardware checks passed with the
CGX on `usbserial`, and a ZWO AM5, ZWO EAF, ASI120MM Mini and QHY miniCam8 on the same host were unaffected.
(During the ConformU run itself `cdc_acm` was blocked entirely rather than for this device only.)

`/usr/local/sbin/cgx-cdc-acm-guard.sh` (mode 755):

```sh
#!/bin/sh
# Keep cdc_acm off the Celestron CGX's control interface (USB 15a2:a50f, interface 0): its probe sends
# SET_LINE_CODING, which the CGX never completes and which kills the Raspberry Pi 5's RP1 xHCI ("HC died").
# 1. New interfaces start unauthorized; 99-cgx-cdc-acm-guard.rules authorizes all but the CGX's interface 0.
# 2. A CGX already present (boot) gets its interface 0 deauthorized before cdc_acm binds.
for h in /sys/bus/usb/devices/usb*; do
  [ -w "$h/interface_authorized_default" ] && echo 0 > "$h/interface_authorized_default"
done
for d in /sys/bus/usb/devices/*; do
  [ -f "$d/idVendor" ] || continue
  if [ "$(cat "$d/idVendor")" = 15a2 ] && [ "$(cat "$d/idProduct")" = a50f ]; then
    for i in "$d"/*:*.0; do [ -w "$i/authorized" ] && echo 0 > "$i/authorized"; done
  fi
done
exit 0
```

`/etc/udev/rules.d/99-cgx-cdc-acm-guard.rules`:

```
# Celestron CGX on the Raspberry Pi 5 (see cgx-cdc-acm-guard.sh). Root hubs start interfaces unauthorized;
# authorize every USB interface except the CGX's interface 0, so cdc_acm never binds it. The CGX's data
# interface (1) goes to the generic usbserial driver (options in cgx-cdc-acm-guard.conf).
ACTION=="add", SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", KERNEL=="usb[0-9]*", RUN+="/bin/sh -c 'echo 0 > /sys%p/interface_authorized_default'"
ACTION!="add", GOTO="cgx_guard_end"
SUBSYSTEM!="usb", GOTO="cgx_guard_end"
ENV{DEVTYPE}!="usb_interface", GOTO="cgx_guard_end"
ATTRS{idVendor}=="15a2", ATTRS{idProduct}=="a50f", ATTR{bInterfaceNumber}=="00", GOTO="cgx_guard_end"
# Authorizing does not re-run driver matching (a driver already loaded tried the interface while it was
# unauthorized and gave up), so ask the kernel to probe it again.
ATTR{authorized}=="0", ATTR{authorized}="1", RUN+="/bin/sh -c 'echo %k > /sys/bus/usb/drivers_probe'"
LABEL="cgx_guard_end"
```

`/etc/modprobe.d/cgx-cdc-acm-guard.conf`:

```
# Celestron CGX on the Raspberry Pi 5: run the guard before cdc_acm loads, and let the generic usbserial
# driver take the CGX's data interface. Remove this file, 99-cgx-cdc-acm-guard.rules and
# /etc/modules-load.d/cgx-usbserial.conf to undo.
install cdc_acm /usr/local/sbin/cgx-cdc-acm-guard.sh; /sbin/modprobe --ignore-install cdc_acm $CMDLINE_OPTS
options usbserial vendor=0x15a2 product=0xa50f
```

Then, as root:

```bash
echo usbserial > /etc/modules-load.d/cgx-usbserial.conf
udevadm control --reload
reboot
```

After the reboot the CGX is a `ttyUSB*` device. Set the device's `portPath` to its by-id name,
`/dev/serial/by-id/usb-Celestron_CGX__CGX_v1-if01-port0`, not to `/dev/ttyUSB0`: the number can change between
boots and between other USB-serial devices. The kernel log then shows "Interface 0 is not authorized for usage"
once for each CDC device plugged in, and the generic driver's "only for testing" warning; it shows no
"HC died".

### Undo

Remove `/etc/modprobe.d/cgx-cdc-acm-guard.conf`, `/etc/udev/rules.d/99-cgx-cdc-acm-guard.rules`,
`/etc/modules-load.d/cgx-usbserial.conf` and `/usr/local/sbin/cgx-cdc-acm-guard.sh`, run
`udevadm control --reload` and reboot. Remove all of them together, and reboot before plugging devices in:
see the trade-off below.

## Trade-off

With the workaround in place every USB interface on the host gets its driver through the udev rule, because
the root hubs start new interfaces unauthorized. If the rule is removed while the root hubs still start them
unauthorized (the guard script or the modprobe file left behind, no reboot), newly plugged devices get no
driver at all. That is why this is a documented, opt-in host step for a Pi 5 with a CGX on direct USB and not
a default in `scripts/host_setup.sh`. The NexStar hand-controller connection (`protocol` not `cgx-aux`) does
not use the CGX's built-in USB bridge and is not affected.
