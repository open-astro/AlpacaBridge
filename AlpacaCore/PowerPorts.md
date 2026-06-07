# Power Port Setup

Per-device setup for AlpacaBridge controllers that expose on-board 12V power ports as ASCOM Switch channels. Each section below is copy-paste safe — the commands use `$USER` so you do not have to substitute your own username anywhere.

Pick the section that matches your hardware.

| Controller | Status | SBC | GPIO library |
|----|----|----|----|
| [ZWO ASIair Pro](#zwo-asiair-pro-raspberry-pi-4) | Available | Raspberry Pi 4 (BCM2711) | libgpiod v2 |
| [ZWO ASIair Plus (Pi CM4)](#zwo-asiair-plus-raspberry-pi-cm4) | Available — ConformU pending | Raspberry Pi CM4 (BCM2711) | libgpiod v2 |
| [ZWO ASIair Plus (RK3568)](#zwo-asiair-plus-rockchip-rk3568) | Available — ConformU pending | Rockchip RK3568 | ZWO `pwm_gpio.ko` ioctl |
| [ToupTek StellaVita](#touptek-stellavita-raspberry-pi-cm4) | Available — ConformU pending | Raspberry Pi CM4 (BCM2711) | libgpiod v2 |
| [iOptron iMate](#ioptron-imate) | Available — ConformU pending | OrangePi 3 LTS (Allwinner H6) | libgpiod v2 |

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

The CM4-based ASIair Plus is a **Raspberry Pi Compute Module 4 (BCM2711)** and drives its four 12V DC outputs through the exact same wiring as the Pi 4 ASIair Pro: GPIO 12, 13, 26, 18 on `/dev/gpiochip0` (`pinctrl-bcm2711`), active-high, default-on at boot. It therefore **reuses the existing libgpiod `asiair` driver** — there is no separate "Plus CM4" switch type. (This is the CM4 variant only; the Rockchip RK3568 ASIair Plus is a completely different controller — see the [RK3568 section](#zwo-asiair-plus-rockchip-rk3568).)

> **Mapping verified on real hardware.** Against a live stock-firmware unit (Port 1 ON, Port 2 dew heater 59%, Port 3 flat panel 34%, Port 4 ON), the BCM GPIO bank read back exactly: GPIO12 static-high (Port 1), GPIO13 pigpio PWM at 59% (Port 2), GPIO26 PWM at 34% (Port 3), GPIO18 static-high (Port 4). Identical to `default_asiair_pro_config()`.

| Port label | GPIO line | Default mode |
|----|----|----|
| Port 1 | 12 | boolean (on/off) |
| Port 2 | 13 | boolean (on/off) |
| Port 3 | 26 | boolean (on/off) |
| Port 4 | 18 | boolean (on/off) |

### 1. Re-image the device

The factory CM4 ASIair Plus ships **32-bit Raspbian (armv7l)** (confirmed: `uname -m` reports `armv7l`, kernel `5.10.27-v7l`). AlpacaBridge is arm64-only. Flash one of:

- Raspberry Pi OS (64-bit) Bookworm or Trixie, **or**
- Debian 13 (Trixie) arm64

onto the CM4's eMMC / microSD before continuing.

### 2. Install runtime dependencies and add yourself to the gpio group

Identical to the Pro — the CM4 Plus uses libgpiod v2:

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

> **Runtime libs** (`libgpiod3`, `libusb-1.0-0`, `libudev1`, `libcurl4`) are what AlpacaBridge links against at runtime — required even if you install only the `.deb`. `gpiod` provides the `gpioget` / `gpioinfo` command-line tools used by the verification step below. **Dev libs** (`libgpiod-dev`, `libusb-1.0-0-dev`, `libudev-dev`, `libcurl4-openssl-dev`, `nlohmann-json3-dev`, `catch2`) and **build tools** (`git`, `build-essential`, `cmake`) are needed only when building from source on the device via `build_and_run.sh`. They are harmless on a `.deb`-only target.

Log out and log back in (or reboot) so the new group membership applies. Verify with:

```bash
groups | tr ' ' '\n' | grep -x gpio
```

You should see `gpio` printed back.

### 3. Optional: preserve "default-on at boot" behaviour

The stock CM4 ASIair firmware turns the 12V outputs ON at boot via a kernel directive in `/boot/firmware/config.txt` (the factory line also lists extra control pins: `gpio=12,13,18,26,5,6,16,17=op,dh`). AlpacaBridge does not require this — but if you want gear plugged into the DC ports powered before the daemon starts, add the four DC-port lines:

```bash
sudo grep -q '^gpio=12,13,18,26=op,dh' /boot/firmware/config.txt \
  || echo 'gpio=12,13,18,26=op,dh' | sudo tee -a /boot/firmware/config.txt
```

Reboot for the change to take effect. Skip this step if you want the ports to come up OFF until the driver claims them.

### 4. Install AlpacaBridge

Install the AlpacaBridge `.deb` for arm64 per the [main install guide](../README.md). The service auto-starts on `:11111`.

### 5. Add the Switch device

Open `http://<your-asiair-plus-ip>:11111/` in a browser:

1. **Configure** tab → **Add Device**
2. **Device Type**: Switch
3. **Vendor**: ZWO
4. **Switch Type**: **ASIair Pro 12V Power Switch** — this is the libgpiod driver, and it serves both the Pi 4 Pro and the CM4 Plus. The pre-filled GPIO defaults (12/13/26/18) are already correct for the CM4 Plus; leave them as-is.
5. **Device Number**: 0 (or any unique number)
6. In the **Power Ports** table, tick the **PWM** checkbox on any port you'll use with a dew heater or flat panel, and rename channels to taste. Leave **GPIO chip device** and **PWM frequency** at their defaults.
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

### Advanced configuration, disconnect behavior, and stock-app coexistence

All identical to the Pi 4 ASIair Pro — see [Advanced configuration](#advanced-configuration), [Disconnect behavior](#disconnect-behavior--important-for-unattended-observatories), and [Coexistence with the stock ZWO app](#coexistence-with-the-stock-zwo-app) above. In short: per-port overrides use the same `gpioChip` / `pwmFrequencyHz` / `ports[]` schema; released lines keep their last-driven state (so set ports OFF in your client before disconnecting if you want a cold release); and you cannot run AlpacaBridge alongside the factory ASIair app, because the stock `zwoair_imager` claims the lines via `pigpiod` and libgpiod will fail with `EBUSY`.

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

The StellaVita is ToupTek's CM4-based observatory controller — a **Raspberry Pi Compute Module 4 (BCM2711)**. It drives four on-board 12V DC power ports over local GPIO (libgpiod v2). AlpacaBridge runs directly on the StellaVita and exposes all four ports as ASCOM Switch channels; each port can be a boolean on/off switch or a 0–100% software PWM channel (dew heater / flat panel use).

> **Mapping verified on real hardware** (StellaVita CM4 Rev 1.1, Debian 13 Trixie, kernel 6.12.75, libgpiod 2.2.1). The board's `/boot/firmware/config.txt` enables the outputs with `gpio=18,10,17,4,9,11=op,dh,pu` under `[all]`. The four DC ports map to BCM GPIO **18, 10, 17, 4** on `/dev/gpiochip0` (`pinctrl-bcm2711`), where the libgpiod line offset equals the BCM GPIO number. GPIO **9** and **11** are also driven high but power the on-board **Cypress USB hub** — they are deliberately **not** exposed as switch channels (cutting them would drop every USB device attached to the StellaVita).

| Port label | GPIO line | Default mode |
|----|----|----|
| Port 1 | 18 | boolean (on/off) |
| Port 2 | 10 | boolean (on/off) |
| Port 3 | 17 | boolean (on/off) |
| Port 4 | 4 | boolean (on/off) |

### 1. Use an arm64 OS

AlpacaBridge is arm64-only. The StellaVita CM4 must run a 64-bit OS — Raspberry Pi OS (64-bit) Bookworm or Trixie, or Debian 13 (Trixie) arm64. (The validated unit shipped on Debian 13 Trixie aarch64.) If your unit is on a 32-bit image, re-image the CM4's eMMC / microSD before continuing.

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

> **Runtime libs** (`libgpiod3`, `libusb-1.0-0`, `libudev1`, `libcurl4`) are what AlpacaBridge links against at runtime — required even if you install only the `.deb`. `gpiod` provides the `gpioget` / `gpioinfo` command-line tools used by the verification step below. **Dev libs** (`libgpiod-dev`, `libusb-1.0-0-dev`, `libudev-dev`, `libcurl4-openssl-dev`, `nlohmann-json3-dev`, `catch2`) and **build tools** (`git`, `build-essential`, `cmake`) are needed only when building from source on the device via `build_and_run.sh`. They are harmless on a `.deb`-only target.

Log out and log back in (or reboot) so the new group membership applies. Verify with:

```bash
groups | tr ' ' '\n' | grep -x gpio
```

You should see `gpio` printed back. If `/dev/gpiochip0` is not group-accessible (`ls -l /dev/gpiochip0` shows a group other than `gpio`), add a udev rule and reload:

```bash
echo 'KERNEL=="gpiochip[0-9]*", GROUP="gpio", MODE="0660"' \
  | sudo tee /etc/udev/rules.d/99-gpio.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

### 3. Confirm the 12V GPIO outputs are enabled

The StellaVita firmware enables the DC ports (and the USB-hub power lines) at boot via a directive in `/boot/firmware/config.txt`:

```ini
# Enable 12V GPIO output
gpio=18,10,17,4,9,11=op,dh,pu
```

This line should already be present under the `[all]` section. It configures the lines as outputs, driven high (so the ports are **powered on at boot**) with pull-ups. **Do not remove `9` or `11`** — they power the on-board Cypress USB hub, and dropping them disables all USB devices. If you want a DC port to come up OFF until the driver claims it, you can remove that port's GPIO from the line (e.g. drop `4` to leave Port 4 off at boot); the four DC ports are `18`, `10`, `17`, `4`. Reboot after editing.

### 4. Install AlpacaBridge

Install the AlpacaBridge `.deb` for arm64 per the [main install guide](../README.md). The service auto-starts on `:11111`.

### 5. Add the Switch device

Open `http://<your-stellavita-ip>:11111/` in a browser:

1. **Configure** tab → **Add Device**
2. **Device Type**: Switch
3. **Vendor**: ToupTek
4. **Device Number**: 0 (or any unique number)
5. The **Power Ports** table appears pre-filled with the four StellaVita ports (Port 1–4, GPIO 18/10/17/4). Tick the **PWM** checkbox on any port you intend to use with a dew heater or flat panel; this exposes that port to ASCOM clients (NINA, etc.) as a 0–100% slider under "Gauges" instead of a plain on/off toggle. Leave **GPIO Chip** (`/dev/gpiochip0`) and **PWM Frequency** at their defaults unless you know you need to override them.
6. Submit

Or `POST` the equivalent JSON to the management API:

```json
{
  "deviceType": "switch",
  "deviceNumber": 0,
  "vendor": "touptek"
}
```

### 6. Verify

Connect the device from the **Devices** tab. You should see four channels (Port 1–4). From a second SSH session, watch the live GPIO state while you toggle a port in the Web UI (libgpiod v2 syntax — `-c` addresses lines by offset on a chip):

```bash
gpioget --numeric -c gpiochip0 18 10 17 4
```

The value of the toggled line flips between `0` (off) and `1` (on). While connected, `gpioinfo -c gpiochip0 18` shows the line claimed by consumer `alpacabridge-stellavita-powerbox`.

### Dimmable ports (PWM)

Any of the four ports can be switched from plain on/off to **soft-PWM dimming** (0–100% duty), driven by a per-port worker thread that bit-bangs the line at a configurable frequency — the same mechanism as the ZWO ASIAIR and iOptron iMate switches. In the **Configure** form, tick **PWM** next to a port and set the **PWM Frequency** (default **100 Hz**). A PWM port then appears in ASCOM clients as a 0–100% slider instead of an on/off toggle. This dims both dew heaters and flat panels.

> **Frequency matters for panels.** A flat panel has its own LED driver with an input capacitor. At a **low frequency** the driver fully powers the LEDs during each on-period and goes dark during each off-period, so the panel visibly dims. **100 Hz tested best on the StellaVita** — it dims smoothly without the visible flicker some panels show at 50 Hz, which is why it is the default. At **~1 kHz** the input cap smooths the chopping into a steady reduced voltage that the driver gates on/off instead of dimming (a panel may just blink off). **Resistive loads (dew heaters) dim at any frequency.** Truly regulated gear — cameras, mounts — should stay on/off regardless.

The defaults match the StellaVita wiring. The configurable fields are the GPIO chip, the PWM frequency, and the per-port name / PWM flags (the GPIO line mapping is fixed):

```json
{
  "deviceType": "switch",
  "deviceNumber": 0,
  "vendor": "touptek",
  "gpioChip": "/dev/gpiochip0",
  "pwmFrequencyHz": 100,
  "ports": [
    { "name": "Mount Power", "pwm": false },
    { "name": "Camera Power", "pwm": false },
    { "name": "Flat Panel",  "pwm": true  },
    { "name": "Dew Heater",  "pwm": true  }
  ]
}
```

| Field | Type | Notes |
|----|----|----|
| `gpioChip` | string | Path to the gpiochip character device. Defaults to `/dev/gpiochip0` (the CM4's main BCM2711 bank). |
| `pwmFrequencyHz` | int | Soft-PWM frequency (1–100000) for any PWM port. Default `100` (tested best on StellaVita — dims flat panels smoothly without 50 Hz flicker). |
| `ports` | array | Positional overlay on `[Port 1, Port 2, Port 3, Port 4]`; each entry's optional `pwm` (bool) / `name` (string) is applied to that port. The GPIO line mapping (18/10/17/4) is fixed. |

### Disconnect behavior — important for unattended observatories

At connect the wrapper claims all four lines at their "on" default — so gear plugged into them is powered as soon as the device connects, preserving the board's boot-high state — and when the ASCOM client (or the Web UI) disconnects, the wrapper releases its libgpiod request **without driving a boolean line LOW first**. (A PWM port stops its worker and is first driven to a defined steady level — on for any duty > 0, off at 0% — so it never strands the line low mid-cycle.) Connecting and disconnecting therefore never power-cycle attached gear.

If you want a port OFF after disconnect, toggle it OFF in your client **before** disconnecting. For unattended setups, keep AlpacaBridge connected for the duration of the session rather than relying on a disconnected-but-powered state.

---

## iOptron iMate

The iMate is iOptron's embedded astronomy computer — an **OrangePi 3 LTS (Allwinner H6, arm64)**. OpenAstro replaces its aging stock OS with an **[Armbian](https://www.armbian.com/)-based Debian 13 (Trixie) image on a mainline kernel** (see the [OpenAstro image builder](https://github.com/open-astro/aw-flashtool)): flash the image to a microSD, boot once to let it self-install to the internal eMMC, then pull the SD. The result is a modern OS with the iMate WiFi AP, GPIO/power-port support, and `libgpiod` v2 already in place — AlpacaBridge then installs from apt.

AlpacaBridge runs directly on the iMate and drives its on-board DC power ports over local GPIO (libgpiod v2). The "iMate PowerBox" exposes three DC barrel jacks; only two are GPIO-switchable, the third is a hardwired always-on pass-through.

| Switch ID | ASCOM name | libgpiod line (`/dev/gpiochip1`) | H6 pin | Writable |
|----|----|----|----|----|
| 0 | `DC3 (always on)` | — (no GPIO) | — | No — read-only, always reports ON |
| 1 | `DC1` | 118 | PD22 | Yes (boolean) |
| 2 | `DC2` | 114 | PD18 | Yes (boolean) |

> **The main GPIO bank is `/dev/gpiochip1` on the OpenAstro (mainline) image.** The dead stock BSP kernel exposed the same H6 pinctrl (`300b000`) as `gpiochip0`; the mainline kernel numbers it `gpiochip1`. The line offsets are unchanged (118 = PD22, 114 = PD18). Confirm on your unit with `gpioinfo` — find the `300b000.pinctrl` chip and its `PD22` / `PD18` lines.

### 1. Install AlpacaBridge

The OpenAstro image already ships `libgpiod` v2 (`libgpiod3` plus the `gpiod` CLI tools). Install AlpacaBridge from the OpenAstro apt repository — the same as on every other platform:

```bash
sudo apt install alpacabridge
```

The service auto-starts on `:11111`. (Building from source on the device instead additionally needs the dev packages: `git build-essential cmake libgpiod-dev libusb-1.0-0-dev libudev-dev libcurl4-openssl-dev nlohmann-json3-dev catch2`.)

### 2. GPIO access is already configured

The OpenAstro image creates a `gpio` group and installs a udev rule (`KERNEL=="gpiochip[0-9]*", GROUP="gpio", MODE="0660"`) and adds the `alpacabridge` service user to that group, so the unprivileged daemon can drive the lines with no manual setup. On a hand-rolled (non-OpenAstro) install, replicate it:

```bash
sudo groupadd -f gpio
sudo usermod -aG gpio alpacabridge      # or "$USER" when running by hand
echo 'KERNEL=="gpiochip[0-9]*", GROUP="gpio", MODE="0660"' \
  | sudo tee /etc/udev/rules.d/99-openastro-gpio.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Confirm the chip is group-accessible:

```bash
ls -l /dev/gpiochip1     # group should be 'gpio', mode crw-rw----
```

> Running the AlpacaBridge service as root instead works too, but the udev-rule approach keeps the daemon unprivileged and is the recommended setup (and what the OpenAstro image ships).

### 3. Add the Switch device

Open `http://<your-imate-ip>:11111/` in a browser:

1. **Configure** tab → **Add Device**
2. **Device Type**: Switch
3. **Vendor**: iOptron
4. **Device Number**: 0 (or any unique number)
5. Leave **GPIO Chip** at its default (`/dev/gpiochip1`) unless you know you need to override it. There are no per-port fields — the three DC ports and their line mapping are fixed.
6. Submit

Or `POST` the equivalent JSON to the management API:

```json
{
  "deviceType": "switch",
  "deviceNumber": 0,
  "vendor": "ioptron"
}
```

### 4. Verify

Connect the device from the **Devices** tab. You should see three channels: `DC3 (always on)` (read-only, always ON), `DC1`, and `DC2`. From a second SSH session, watch the live GPIO state while you toggle DC1/DC2 in the Web UI (libgpiod v2 syntax — `-c` addresses lines by offset on a chip):

```bash
gpioget --numeric -c gpiochip1 118 114    # DC1 line 118, DC2 line 114
```

The value of the toggled line flips between `0` (off) and `1` (on). Attempting to write `DC3` returns an ASCOM "not implemented" error — it is a read-only pass-through.

### Dimmable ports (PWM)

DC1 and DC2 can each be switched from plain on/off to **soft-PWM dimming** (0–100% duty), driven by a per-port worker thread that bit-bangs the line at a configurable frequency — the same mechanism as the ZWO ASIAIR switch. In the **Configure** form, tick **PWM** next to DC1 and/or DC2 and set the **PWM Frequency** (default **50 Hz**). A PWM port then appears in ASCOM clients (NINA, etc.) as a 0–100% slider instead of an on/off toggle. This dims both **dew heaters** and **flat panels** (confirmed on iMate hardware against a real panel).

> **Frequency matters for panels.** A flat panel has its own LED driver with an input capacitor. At a **low frequency (~50 Hz)** the driver fully powers the LEDs during each on-period and goes dark during each off-period, so the panel visibly dims — this is why 50 Hz is the default (and what ZWO drives the ASIAIR Plus at). At **~1 kHz** the input cap smooths the chopping into a steady reduced voltage that the driver gates on/off instead of dimming (a panel may just blink off). **Resistive loads (dew heaters, a 12 V bulb) dim at any frequency.** Truly regulated gear — cameras, mounts — should stay on/off regardless. The always-on DC3 jack has no GPIO and can't be PWM.

The fixed DC3/DC1/DC2 layout is not remappable; the configurable fields are the GPIO chip, the PWM frequency, and the per-port PWM flags:

```json
{
  "deviceType": "switch",
  "deviceNumber": 0,
  "vendor": "ioptron",
  "gpioChip": "/dev/gpiochip1",
  "pwmFrequencyHz": 50,
  "ports": [ {}, { "pwm": true }, { "pwm": false } ]
}
```

| Field | Type | Notes |
|----|----|----|
| `gpioChip` | string | Path to the gpiochip character device. Defaults to `/dev/gpiochip1`. |
| `pwmFrequencyHz` | int | Soft-PWM frequency (1–100000) for any PWM port. Default `50` (dims flat panels; raise for dew-heater-only setups if you prefer). |
| `ports` | array | Positional overlay on `[DC3, DC1, DC2]`; each entry's optional `pwm` (bool) / `name` (string) is applied to that port. The DC3 entry's `pwm` is ignored (no GPIO). |

### Disconnect behavior

Same policy as the ASIair drivers: at connect the wrapper claims DC1/DC2 at their "on" default — so gear plugged into them is powered as soon as the device connects — and when the ASCOM client (or the Web UI) disconnects, the wrapper releases its libgpiod request **without driving the lines LOW first**. (A PWM port stops its worker and is first driven to a defined steady level — on for any duty > 0, off at 0% — so it never strands the line low mid-cycle.) Connecting and disconnecting therefore never power-cycle attached gear. The always-on DC3 jack is hardwired live and unaffected by the driver entirely. If you want DC1 or DC2 OFF after disconnect, toggle it OFF in your client **before** disconnecting.

> libgpiod releases the line on `close()`, so for unattended setups keep AlpacaBridge connected for the duration of the session rather than relying on a disconnected-but-powered state.

---

## See also

- [SUPPORTED-DRIVERS.md](../SUPPORTED-DRIVERS.md) — full list of validated drivers and hardware
- [AGENTS.md `### ZWO`](../AGENTS.md) — vendor-specific implementation notes
