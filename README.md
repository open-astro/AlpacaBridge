# AlpacaBridge

<img src="docs/image/ab.png" alt="AlpacaBridge logo" width="420">

[![CI](https://github.com/open-astro/AlpacaBridge/actions/workflows/ci.yml/badge.svg)](https://github.com/open-astro/AlpacaBridge/actions/workflows/ci.yml)
[![License: AGPL-3.0-or-later](https://img.shields.io/badge/License-AGPL--3.0--or--later-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Debian%2013%20arm64-orange.svg)](#supported-hardware)
[![ConformU](https://img.shields.io/badge/ConformU-validated%20on%20hardware-success.svg)](SUPPORTED-DRIVERS.md)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg)](docs/development.md)

**The engine at the telescope.**

AlpacaBridge turns a single-board computer into a control server for your entire rig. Native ASCOM Alpaca drivers, running right at the telescope. No ASCOM Platform. No Windows box. No vendor drivers to chase down.

Flash it. Plug in your gear. Image from anywhere on your network with N.I.N.A., APT, CCDciel, Sequence Generator Pro, SharpCap, or [Ara](https://www.openastro.net).

#### [3.5.0] - 2026-08-16 &middot; [Changelog](CHANGELOG.md)

## Why AlpacaBridge

- **Proven, not promised.** Every driver is [validated with ASCOM ConformU](SUPPORTED-DRIVERS.md), on the actual hardware it supports.
- **50 drivers. Fourteen brands. One server.** Astroasis, Celestron, Gemini, iOptron, OnStep, Player One Astronomy, QHY, Sky-Watcher, SVBONY, ToupTek Astro, Unihedron SQM-LE (WeeWX plugin), WandererAstro, WeeWX, and ZWO.
- **Plug in and go.** Vendor SDKs and udev rules come bundled. USB and Wi-Fi devices are auto-detected. No port hunting.
- **Manage it from a browser.** Configure every device from the built-in web UI, from any machine on your network.
- **Set it and forget it.** Installs from the OpenAstro APT repository, runs as a systemd service, starts on boot, updates with `apt upgrade`.
- **Built with the manufacturers.** iOptron, Player One Astronomy, QHY, and ToupTek Astro supply hardware directly for driver development.

## Supported hardware

AlpacaBridge runs on **Debian 13 (Trixie)** `arm64`. The [OpenAstro docs](https://www.openastro.net/docs/sbc-install/overview) have a step-by-step guide for every supported machine.

### Unlock your device.

Give commercial astro gear a second life:

| Machine | | Setup guide |
|---------|---|-------------|
| iOptron iMate | Built hand-in-hand with iOptron, ready for OpenAstro | [iOptron iMate](https://www.openastro.net/docs/sbc-install/ioptron-imate) |
| ToupTek StellaVita (Pi CM4) | The same Pi CM4 inside, set free | [ToupTek StellaVita](https://www.openastro.net/docs/sbc-install/touptek-stellavita) |
| ZWO ASIAIR | The one that started it all; flash a fresh card and keep the original | [ZWO ASIAIR](https://www.openastro.net/docs/sbc-install/zwo-asiair) |
| ZWO ASIAIR Pro | Installs to a fresh card, leaves the stock card untouched | [ZWO ASIAIR Pro](https://www.openastro.net/docs/sbc-install/zwo-asiair-pro) |
| ZWO ASIAIR Plus (Pi CM4) | Back up the eMMC, then flash | [ZWO ASIAIR Plus (CM4)](https://www.openastro.net/docs/sbc-install/zwo-asiair-plus-cm4) |
| ZWO ASIAIR Plus (RK3568) | Backs up the stock software automatically | [ZWO ASIAIR Plus (RK3568)](https://www.openastro.net/docs/sbc-install/zwo-asiair-plus-rk3568) |

### Build your own.

Start from a bare board and pair it with the gear you choose:

| Machine | | Setup guide |
|---------|---|-------------|
| Orange Pi 4 Pro | The best value build; faster than a Pi 4 at a lower price, with a ready-made image | [Orange Pi 4 Pro](https://www.openastro.net/docs/sbc-install/orange-pi-4-pro) |
| Raspberry Pi 3B+ | The budget build; USB 2.0 only, so camera downloads take longer | [Raspberry Pi 3B+](https://www.openastro.net/docs/sbc-install/raspberry-pi-3) |
| Raspberry Pi 4 | The classic build; USB 3.0 speed on power any mount can supply | [Raspberry Pi 4](https://www.openastro.net/docs/sbc-install/raspberry-pi-4) |
| Raspberry Pi 5 | The fastest build; bring a 5V/5A supply to feed it | [Raspberry Pi 5](https://www.openastro.net/docs/sbc-install/raspberry-pi-5) |

Other Rockchip and Orange Pi arm64 boards running Debian 13 work with the standard Raspberry Pi instructions.

## Installation

Three commands from the [OpenAstro APT repository](https://apt.openastro.net), and it stays current with `apt upgrade`.

**1. Add the OpenAstro signing key**

```sh
sudo curl -fsSL https://apt.openastro.net/repo/openastro-archive-keyring.gpg \
    | sudo gpg --dearmor -o /usr/share/keyrings/openastro-archive-keyring.gpg
```

**2. Add the repository**

```sh
echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/openastro-archive-keyring.gpg] \
https://apt.openastro.net trixie main" \
    | sudo tee /etc/apt/sources.list.d/openastro.list
```

**3. Install**

```sh
sudo apt update
sudo apt install alpacabridge
```

The service starts automatically and runs as the `alpacabridge` system user. Then open **http://localhost:6800/** (or your server's hostname from any machine on the network) and set up your devices.

<details>
<summary>Updating and uninstalling</summary>

```sh
sudo apt update && sudo apt upgrade alpacabridge   # update
sudo apt remove alpacabridge                       # uninstall
```

</details>

## In the news

- ScopeTrader on the launch: [OpenAstro AlpacaBridge Launches and Why It Matters](https://scopetrader.com/openastro-alpacabridge-launches-and-why-it-matters/)
- ScopeTrader on the manufacturers rallying behind the project: [Open Astro Project Expands Alpaca Bridge Astronomy Hardware Support](https://scopetrader.com/open-astro-project-expands-alpaca-bridge-astronomy-hardware-support/)
- On video: [Installing AlpacaBridge on a ToupTek StellaVita](https://youtu.be/nVAS45OTltA)

## Learn more

- [OpenAstro docs](https://www.openastro.net/docs/intro): setup guides and the [FAQ](https://www.openastro.net/docs/faq)
- [SUPPORTED-DRIVERS.md](SUPPORTED-DRIVERS.md): the validated driver matrix
- [Development Guide](docs/development.md): building from source, writing drivers, running tests

## License

AlpacaBridge is licensed under the [GNU AGPL v3 or later](LICENSE). Derived versions stay open even when the bridge is embedded in a device and served over the network rather than shipped. An additional permission appended to the license (the vendor-SDK linking exception) allows combining AlpacaBridge with the proprietary device-vendor SDKs it needs to operate hardware.
