# AlpacaBridge

<img src="https://www.openastro.net/wp-content/uploads/2026/01/AlpacaBridge.png" alt="AlpacaBridge logo" width="420">

AlpacaBridge is a growing suite of native ASCOM Alpaca drivers for Linux, every one ConformU-verified, that turns any single-board computer into a gear-control server. There's no ASCOM Platform to install and no vendor COM drivers to chase down. Just flash it onto a Raspberry Pi, iOptron iMate, ToupTek StellaVita, or ZWO ASIAIR, connect your equipment once, and control gear like cameras, mounts, focusers, filter wheels, rotators, cover calibrators, switches, and weather stations over the network from any Alpaca-compatible app, including N.I.N.A., Sequence Generator Pro, and SharpCap.

#### [3.0.1] - 2026-07-14 &middot; [Changelog](CHANGELOG.md)

## Features

- **Native Alpaca drivers** — no ASCOM Platform, no Windows box, no vendor COM drivers to chase down; everything runs on the SBC at your telescope
- **Vendor support** — manufacturers including iOptron, Player One Astronomy, QHY, and ToupTek Astro supply hardware directly for driver development
- **[ConformU-verified](SUPPORTED-DRIVERS.md)** — every driver ships validated against the official ASCOM conformance suite, on the hardware it supports; see the full device matrix
- **Eleven hardware brands** — iOptron, Player One Astronomy, ZWO, QHY, ToupTek Astro, Sky-Watcher, Celestron, Gemini, SVBONY, WandererAstro, and WeeWX weather stations
- **Plug in and go** — bundled vendor SDKs and udev rules, with USB and Wi-Fi devices auto-detected; no port hunting, no driver installs
- **Web management UI** — configure every device from a browser anywhere on your network
- **Set-and-forget service** — installs from the OpenAstro APT repository, runs as a systemd service, starts on boot, updates with `apt upgrade`

## In the news

ScopeTrader covered the project launch and what it means for the Alpaca ecosystem: [OpenAstro AlpacaBridge Launches and Why It Matters](https://scopetrader.com/openastro-alpacabridge-launches-and-why-it-matters/).

ScopeTrader on the manufacturer support rallying behind the project — iOptron, Player One, QHY, and ToupTek Astro are all supplying hardware for driver development: [Open Astro Project Expands Alpaca Bridge Astronomy Hardware Support](https://scopetrader.com/open-astro-project-expands-alpaca-bridge-astronomy-hardware-support/).

Watch a full install on a ToupTek StellaVita: [Installing AlpacaBridge on a ToupTek StellaVita](https://youtu.be/nVAS45OTltA).

## Supported hardware

AlpacaBridge runs on **Debian 13 (Trixie)** `arm64`. The [Wiki](https://github.com/open-astro/AlpacaBridge/wiki) has a step-by-step setup guide for every supported machine:

| Machine | Setup guide |
|---------|-------------|
| Raspberry Pi 3B+, 4 & 5 | [Raspberry Pi](https://github.com/open-astro/AlpacaBridge/wiki/Raspberry-Pi-%283,-4,-%26-5%29) |
| iOptron iMate | [iOptron iMate](https://github.com/open-astro/AlpacaBridge/wiki/iOptron-iMate) |
| ToupTek StellaVita (Pi CM4) | [ToupTek StellaVita](https://github.com/open-astro/AlpacaBridge/wiki/ToupTek-StellaVita-%28Pi-CM4%29) |
| ZWO ASIAIR Pro | [ZWO ASIAIR Pro](https://github.com/open-astro/AlpacaBridge/wiki/ZWO-ASIAIR-Pro) |
| ZWO ASIAIR Plus (Pi CM4) | [ZWO ASIAIR Plus — CM4](https://github.com/open-astro/AlpacaBridge/wiki/ZWO-ASIAIR-Plus-%28Pi-CM4%29) |
| ZWO ASIAIR Plus (RK3568) | [ZWO ASIAIR Plus — RK3568](https://github.com/open-astro/AlpacaBridge/wiki/ZWO-ASIAIR-Plus-%28RK3568%29) |

Other Rockchip and OrangePi arm64 boards running Debian 13 work with the standard Raspberry Pi instructions.

## Learn more

- [Wiki](https://github.com/open-astro/AlpacaBridge/wiki) — setup guides for Raspberry Pi, StellaVita, ASIAIR, iMate, and troubleshooting
- [SUPPORTED-DRIVERS.md](SUPPORTED-DRIVERS.md) — validated driver matrix
- [Development Guide](docs/development.md) — building from source, writing drivers, running tests

## Installation

AlpacaBridge is distributed via the OpenAstro APT repository at [apt.openastro.net](https://apt.openastro.net).

### 1. Add the OpenAstro signing key

```sh
sudo curl -fsSL https://apt.openastro.net/repo/openastro-archive-keyring.gpg \
    | sudo gpg --dearmor -o /usr/share/keyrings/openastro-archive-keyring.gpg
```

### 2. Add the repository

```sh
echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/openastro-archive-keyring.gpg] \
https://apt.openastro.net trixie main" \
    | sudo tee /etc/apt/sources.list.d/openastro.list
```

### 3. Install

```sh
sudo apt update
sudo apt install alpacabridge
```

The service starts automatically and runs as the `alpacabridge` system user.

## Using AlpacaBridge

Once installed, open:

- **Driver Setup:** http://localhost:6800/

From another machine on your network, substitute the hostname or IP address of the server.

### Updating

```sh
sudo apt update
sudo apt upgrade alpacabridge
```

### Uninstalling

```sh
sudo apt remove alpacabridge
```

## Support the project

AlpacaBridge is developed and maintained independently. Contributions help fund hardware access, testing, and ongoing standards-compliance work.

[![PayPal](https://img.shields.io/badge/Donate-PayPal-blue.svg)](https://www.paypal.com/paypalme/joeytroynm)

## License

AlpacaBridge is licensed under the [GNU AGPL v3 or later](LICENSE) — derived versions stay open even when the bridge is embedded in a device and served over the network rather than shipped. An additional permission appended to the license (the vendor-SDK linking exception) allows combining AlpacaBridge with the proprietary device-vendor SDKs it needs to operate hardware.
