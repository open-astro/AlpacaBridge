# Power Port Setup

Per-device setup for AlpacaBridge controllers that expose on-board 12V power ports as ASCOM Switch channels. Each section below is copy-paste safe — the commands use `$USER` so you do not have to substitute your own username anywhere.

Pick the section that matches your hardware.

| Controller | Status | SBC | GPIO library |
|----|----|----|----|
| [ZWO ASIair Pro](#zwo-asiair-pro-raspberry-pi-4) | Available | Raspberry Pi 4 (BCM2711) | libgpiod v2 |
| [ZWO ASIair Plus (Pi CM4)](#zwo-asiair-plus-raspberry-pi-cm4) | Pending hardware | Raspberry Pi CM4 | libgpiod v2 |
| [ZWO ASIair Plus (RK3568)](#zwo-asiair-plus-rockchip-rk3568) | Available — ConformU pending | Rockchip RK3568 | ZWO `pwm_gpio.ko` ioctl |
| [ToupTek StellaVita](#touptek-stellavita-raspberry-pi-cm4) | Pending hardware | Raspberry Pi CM4 | TBD |
| [iOptron iMate](#ioptron-imate) | Pending hardware | TBD | TBD |

---

## ZWO ASIair Pro (Raspberry Pi 4)

Four 12V DC outputs on GPIO 12, 13, 26, 18. Each port can be configured independently as a boolean on/off switch or a 0–100% software PWM channel (dew heater / flat panel use).

| Port label | GPIO line | Default mode |
|----|----|----|
| Port 1 | 12 | boolean (on/off) |
| Port 2 | 13 | boolean (on/off) |
| Port 3 | 26 | boolean (on/off) |
| Port 4 | 18 | boolean (on/off) |

### 1. Re-image the device

The factory ASIair Pro ships **32-bit Raspbian Buster (armv7l)**. AlpacaBridge is arm64-only. Flash one of:

- Raspberry Pi OS (64-bit) Bookworm or Trixie, **or**
- Debian 13 (Trixie) arm64

onto the ASIair Pro's microSD card before continuing.

### 2. Install runtime dependencies and add yourself to the gpio group

```bash
sudo apt update
sudo apt install -y \
  git build-essential cmake \
  libgpiod3 gpiod \
  libusb-1.0-0 libudev1 libcurl4 \
  libgpiod-dev libusb-1.0-0-dev libudev-dev libcurl4-openssl-dev \
  nlohmann-json3-dev catch2
sudo usermod -aG gpio "$USER"
```

> **Runtime libs** (`libgpiod3`, `libusb-1.0-0`, `libudev1`, `libcurl4`) are what AlpacaBridge links against at runtime — required even if you install only the `.deb`. `gpiod` provides the `gpioget` / `gpioinfo` command-line tools used by the verification step below. **Dev libs** (`libgpiod-dev`, `libusb-1.0-0-dev`, `libudev-dev`, `libcurl4-openssl-dev`, `nlohmann-json3-dev`, `catch2`) and **build tools** (`git`, `build-essential`, `cmake`) are needed when building from source on the device via `build_and_run.sh`. They are harmless on a `.deb`-only target.

Log out and log back in (or reboot) so the new group membership applies. Verify with:

```bash
groups | tr ' ' '\n' | grep -x gpio
```

You should see `gpio` printed back.

### 3. Optional: preserve "default-on at boot" behaviour

The stock ASIair firmware turns all four 12V outputs ON at boot via a kernel directive in `/boot/firmware/config.txt`. AlpacaBridge does not require this — but if you want gear plugged into the DC ports to be powered before the AlpacaBridge daemon starts, add the same line:

```bash
sudo grep -q '^gpio=18,12,13,26=op,dh,pu' /boot/firmware/config.txt \
  || echo 'gpio=18,12,13,26=op,dh,pu' | sudo tee -a /boot/firmware/config.txt
```

Reboot for the change to take effect. Skip this step if you want the ports to come up OFF until the driver claims them.

### 4. Install AlpacaBridge

Install the AlpacaBridge `.deb` for arm64 per the [main install guide](../README.md). The service auto-starts on `:11111`.

### 5. Add the Switch device

Open `http://<your-asiair-ip>:11111/` in a browser:

1. **Configure** tab → **Add Device**
2. **Device Type**: Switch
3. **Vendor**: ZWO
4. **Switch Type**: **ASIair Pro 12V Power Switch**
5. **Device Number**: 0 (or any unique number)
6. **Power Ports** table appears with the four ports pre-filled to the Pi 4 ASIair Pro defaults. Tick the **PWM** checkbox on any port you intend to use with a dew heater or flat panel; this exposes that port to ASCOM clients (NINA, etc.) as a 0–100% slider under "Gauges" instead of a plain on/off toggle. Rename channels in the **Channel name** column if you want NINA to label them by role (e.g. "Mount", "Dew Heater"). Leave **GPIO chip device** and **PWM frequency** at their defaults unless you know you need to override them.
7. Submit

Or `POST` the equivalent JSON to the management API:

```json
{
  "deviceType": "switch",
  "deviceNumber": 0,
  "vendor": "zwo",
  "switchType": "asiair"
}
```

### 6. Verify

Connect the device from the **Devices** tab. You should see four channels (Port 1–4). From a second SSH session, watch the live GPIO state while you toggle a port in the Web UI:

```bash
gpioget gpiochip0 12 13 26 18
```

The value of the toggled pin will flip between `inactive` and `active`.

### Advanced configuration

The defaults match the Pi 4 ASIair Pro wiring. For non-default deployments (different SBC, different pin map, PWM channels), override per-port in the device config:

```json
{
  "deviceType": "switch",
  "deviceNumber": 0,
  "vendor": "zwo",
  "switchType": "asiair",
  "gpioChip": "/dev/gpiochip0",
  "pwmFrequencyHz": 1000,
  "ports": [
    { "name": "Mount Power",  "gpio": 12, "pwm": false },
    { "name": "Camera Power", "gpio": 13, "pwm": false },
    { "name": "Flat Panel",   "gpio": 26, "pwm": true  },
    { "name": "Dew Heater",   "gpio": 18, "pwm": true  }
  ]
}
```

| Field | Type | Notes |
|----|----|----|
| `gpioChip` | string | Path to the gpiochip character device. Defaults to `/dev/gpiochip0`. |
| `pwmFrequencyHz` | integer | Soft-PWM frequency for any port with `pwm: true`. Range 1–100000. Default 1000. |
| `ports[].name` | string | Human-readable channel name shown to ASCOM clients (NINA, etc.). |
| `ports[].gpio` | integer | BCM GPIO line number. |
| `ports[].pwm` | boolean | `true` for analog 0–100% (dew heater, flat panel). `false` for boolean on/off. |

### Disconnect behavior — important for unattended observatories

When the ASCOM client (or the AlpacaBridge Web UI) disconnects the device, the driver releases its hold on the four GPIO lines but **does not drive them LOW first**. The lines stay in whatever state they were last in.

Combined with the `gpio=18,12,13,26=op,dh,pu` boot directive in `/boot/firmware/config.txt`, which configures the lines as outputs with default-high and pull-up enabled, this means **released lines stay HIGH**. The 12V outputs remain powered after disconnect — your camera, mount, and dew heaters keep getting voltage.

This is intentional and consistent with the boot-time default-on behavior the line was designed for: gear is supposed to be powered as soon as the Pi has booted, and remain powered through driver lifecycle events.

If you want a port OFF when you disconnect:

1. Toggle the port OFF in your client (NINA, Web UI, etc.) **before** disconnecting the Switch device.
2. Then disconnect — the port stays OFF because the line was already driven LOW before the release.

For an unattended shutdown sequence (e.g. observatory close-up), set all four ports to OFF in NINA, then disconnect, then issue your shutdown. The released lines keep their LOW state until the next reboot (when the kernel `gpio=` directive drives them HIGH again).

### Coexistence with the stock ZWO app

Not supported. The stock `zwoair_imager` claims the GPIO via `pigpiod`, and AlpacaBridge's libgpiod request will fail with `EBUSY`. If you ever boot back into the factory ASIair OS, AlpacaBridge cannot run; conversely, the stock ZWO app does not run on the re-imaged 64-bit OS.

---

## ZWO ASIair Plus (Raspberry Pi CM4)

*Pending hardware validation. The ASIair Plus uses a Pi CM4 with on-board I²C current/voltage monitoring on each port; the GPIO mapping and per-channel telemetry will be documented after first ConformU validation.*

---

## ZWO ASIair Plus (Rockchip RK3568)

Four 12V DC outputs controlled through ZWO's custom `pwm_gpio.ko` kernel module, which registers a misc-device character node at `/dev/pwm-gpio-misc` and exposes per-port boolean and PWM control through a small set of ioctls. This is fundamentally different from the Pi 4 ASIair Pro — there is no `gpiochip` involved on the user-facing side.

> **PWM is implemented as userspace soft-PWM** — not as the kernel module's native `SET_MODE(PWM)` path. We attempted that path first and found it dead end: the kernel module accepts every `SET_MODE(PWM) + ENABLE + SET_CONFIG` call (`GET_CONFIG` even echoes back the exact period/duty we wrote), but the load shows no visible duty-cycle response at any frequency. The kernel module's hrtimer dispatch is unreachable from any documented ioctl ordering, and the RK3568's hardware PWM peripherals can't mux to the airplus-gpios pins anyway (they sit on GPIO bank 4, which the RK3568 PWM controllers don't reach). So the AlpacaBridge driver bypasses `SET_MODE(PWM)` entirely and instead spawns a per-port worker thread that uses `SET_MODE(GPIO) + ENABLE + SET_LEVEL` ioctls at the configured frequency to generate the PWM waveform itself. This is the same approach the stock ZWO `zwoair_imager` daemon takes (confirmed by extracting and analyzing the binary). Verified end-to-end against real hardware — both boolean toggle AND duty-cycle dimming produce the expected physical behavior on a 12 V flat-panel load. Tick the PWM checkbox freely; PWM works.

| Port label | Kernel ioctl index | Default mode |
|----|----|----|
| Port 1 | 4 | boolean (on/off) |
| Port 2 | 5 | boolean (on/off) |
| Port 3 | 6 | boolean (on/off) |
| Port 4 | 7 | boolean (on/off) |

### 1. Use a compatible OS image

The ASIair Plus RK3568 ships with ZWO's heavily customised stock OS. AlpacaBridge requires **Debian 13 (Trixie) on aarch64** with the original ZWO kernel (4.19.219) — specifically, the `pwm_gpio.ko` module **must** remain loaded, because it owns the device-tree `airplus-gpios` node and is the only way to drive the four DC ports.

If you've reflashed the device, do it with a tool that preserves the kernel + ZWO kernel modules (e.g. the project tracked at `rk-flashtool`). Standard mainline RK3568 distros do **not** ship `pwm_gpio.ko` and will leave the ports unmanageable.

### 2. Install runtime dependencies and the gpio-group rule

```bash
sudo apt update
sudo apt install -y \
  git build-essential cmake \
  libusb-1.0-0 libudev1 libcurl4 \
  libusb-1.0-0-dev libudev-dev libcurl4-openssl-dev \
  nlohmann-json3-dev catch2
sudo groupadd -f gpio
sudo usermod -aG gpio "$USER"
```

> **Runtime libs** (`libusb-1.0-0`, `libudev1`, `libcurl4`) are what AlpacaBridge links against at runtime — required even if you install only the `.deb`. **Dev libs** (`libusb-1.0-0-dev`, `libudev-dev`, `libcurl4-openssl-dev`, `nlohmann-json3-dev`, `catch2`) and **build tools** (`git`, `build-essential`, `cmake`) are needed when building from source on the device via `build_and_run.sh`. They are harmless on a `.deb`-only target. The Plus driver does **not** use libgpiod (unlike the Pro), so no `libgpiod*` packages are required.

The udev rule shipped with AlpacaBridge (`/etc/udev/rules.d/99-zwo-asiair-plus.rules`) grants the `gpio` group `0660` access to `/dev/pwm-gpio-misc`. Without it, only root can open the device — and AlpacaBridge intentionally runs unprivileged. The rule is installed automatically by `build_and_run.sh` (or by the `.deb` postinst), which also handles the `groupadd` / `usermod` steps shown above — the manual commands here just make the dependency explicit for first-time deployments.

After adding yourself to the `gpio` group, log out and back in (or `newgrp gpio`) so membership applies to your current shell.

### 3. Install AlpacaBridge

Install the AlpacaBridge `.deb` for arm64 per the [main install guide](../README.md). The service auto-starts on `:11111`.

### 4. Add the Switch device

Open `http://<your-asiair-plus-ip>:11111/` in a browser:

1. **Configure** tab → **Add Device**
2. **Device Type**: Switch
3. **Vendor**: ZWO
4. **Switch Type**: **ASIair Plus 12V Power Switch (RK3568)**
5. **Device Number**: 0
6. The **Power Ports** table is simpler than the Pro's — only the channel name and PWM checkbox are configurable per port. The kernel module fixes the ioctl index for each port (DC1=4, DC2=5, DC3=6, DC4=7), so there's no GPIO-line field. Tick **PWM** on any port you intend to use with a dew heater or flat panel.
7. Submit

Or via the management API:

```json
{
  "deviceType": "switch",
  "deviceNumber": 0,
  "vendor": "zwo",
  "switchType": "asiair-plus-rk3568"
}
```

### 5. Verify

Connect the device from the **Devices** tab. From a second SSH session you can confirm the kernel module is being driven by inspecting `/proc/modules`:

```bash
grep pwm_gpio /proc/modules        # should show non-zero refcount while connected
sudo cat /sys/kernel/debug/gpio    # shows the airplus-gpios assignments
```

### Advanced configuration

For non-default deployments (e.g. you've remapped the device node), the full config schema is:

```json
{
  "deviceType": "switch",
  "deviceNumber": 0,
  "vendor": "zwo",
  "switchType": "asiair-plus-rk3568",
  "devicePath": "/dev/pwm-gpio-misc",
  "pwmFrequencyHz": 1000,
  "ports": [
    { "name": "Mount Power",  "pwm": false },
    { "name": "Camera Power", "pwm": false },
    { "name": "Flat Panel",   "pwm": true  },
    { "name": "Dew Heater",   "pwm": true  }
  ]
}
```

The driver's userspace soft-PWM accepts any value in 1–100,000 Hz, but the practical ceiling on stock Linux without busy-waiting is around 2 kHz — above that, `nanosleep`/`sleep_until` jitter is large enough relative to the period that the duty cycle distorts visibly. The default `50 Hz` matches what ZWO's stock `zwoair_imager` daemon uses — verified against a live stock-firmware ASIair Plus by reading the kernel module's per-port config via `PWM_GPIO_GET_CONFIG`, which returned `period_ns = 20,000,000` (= 20 ms = 50 Hz) on every PWM-enabled port (37%, 43%, 100% duties exactly matched the user's settings). 50 Hz also matches mains frequency in China where ZWO is based, likely chosen to avoid beating against AC ripple in the 12 V input. For loads that visibly flicker at 50 Hz (some passive LED panels viewed off-axis), bump to 100–500 Hz via `pwmFrequencyHz`.

### Disconnect behavior

Identical to the ASIair Pro driver: `close()` releases our fd without driving the lines LOW first, so released ports stay in their last-driven state. The kernel module retains per-port mode + level across opens, so a disconnect from the ASCOM client does **not** power-cycle anything. Set each port OFF in your client before disconnecting if you want a cold release.

### What about the USB power ports and the button?

The same `pwm_gpio.ko` module also controls the two USB2 ports, two USB3 ports, the two status LEDs, and the physical button (ioctl indices 0, 1, 2, 8, 9, 10, 11). They are **not** exposed by the v1 AlpacaBridge Switch driver — the v1 surface is 4 DC ports only, matching the four-channel ASCOM Switch interface most clients expect. Extending the driver to control them is straightforward (the kernel ioctls are identical) — track interest in `AGENTS.md`.

---

## ToupTek StellaVita (Raspberry Pi CM4)

*Pending hardware validation. The StellaVita is a CM4-based controller from ToupTek; GPIO mapping, GPIO chip path, and any vendor-specific quirks will be documented after first ConformU validation.*

---

## iOptron iMate

*Pending hardware validation. Hardware layout, GPIO mapping, and any vendor-specific quirks will be documented after first ConformU validation.*

---

## See also

- [SUPPORTED-DRIVERS.md](../SUPPORTED-DRIVERS.md) — full list of validated drivers and hardware
- [AGENTS.md `### ZWO`](../AGENTS.md) — vendor-specific implementation notes
