# Power Port Setup

Per-device setup for AlpacaBridge controllers that expose on-board 12V power ports as ASCOM Switch channels. Each section below is copy-paste safe — the commands use `$USER` so you do not have to substitute your own username anywhere.

Pick the section that matches your hardware.

| Controller | Status | SBC | GPIO library |
|----|----|----|----|
| [ZWO ASIair Pro](#zwo-asiair-pro-raspberry-pi-4) | Available | Raspberry Pi 4 (BCM2711) | libgpiod v2 |
| [ZWO ASIair Plus (Pi CM4)](#zwo-asiair-plus-raspberry-pi-cm4) | Pending hardware | Raspberry Pi CM4 | libgpiod v2 |
| [ZWO ASIair Plus (RK3568)](#zwo-asiair-plus-rockchip-rk3568) | Pending hardware | Rockchip RK3568 | libgpiod v2 |
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
sudo apt install -y libgpiod3 libgpiod-dev gpiod libusb-1.0-0 libudev1 libcurl4
sudo usermod -aG gpio "$USER"
```

> `libgpiod3` is the runtime shared library AlpacaBridge links against. `libgpiod-dev` is needed if you build from source on the device (and is harmless to install on a `.deb`-only target). `gpiod` provides the `gpioget` / `gpioinfo` command-line tools used by the verification step below.

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

### Coexistence with the stock ZWO app

Not supported. The stock `zwoair_imager` claims the GPIO via `pigpiod`, and AlpacaBridge's libgpiod request will fail with `EBUSY`. If you ever boot back into the factory ASIair OS, AlpacaBridge cannot run; conversely, the stock ZWO app does not run on the re-imaged 64-bit OS.

---

## ZWO ASIair Plus (Raspberry Pi CM4)

*Pending hardware validation. The ASIair Plus uses a Pi CM4 with on-board I²C current/voltage monitoring on each port; the GPIO mapping and per-channel telemetry will be documented after first ConformU validation.*

---

## ZWO ASIair Plus (Rockchip RK3568)

*Pending hardware validation. The RK3568 variant uses kernel IIO ADC channels (`/sys/bus/iio/devices/iio:device0/in_voltage*_raw`) for current/voltage telemetry instead of I²C. GPIO mapping and the libgpiod chip path will be documented after first ConformU validation.*

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
