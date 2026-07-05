# AlpacaBridge

<img src="https://www.openastro.net/wp-content/uploads/2026/01/AlpacaBridge.png" alt="AlpacaBridge logo" width="420">

AlpacaBridge turns a small Linux computer at the telescope into a complete ASCOM Alpaca server. Connect your gear once — cameras, mounts, focusers, filter wheels, rotators, switches, and weather stations — and control everything over the network from any Alpaca-compatible software, including N.I.N.A. and Sequence Generator Pro. Every driver ships ConformU-validated.

#### [2.2.0] - 2026-07-01 &middot; [Changelog](CHANGELOG.md)

## Features

- **Turnkey Alpaca server** — installs as a systemd service, starts on boot
- **[ConformU-validated drivers](SUPPORTED-DRIVERS.md)** — cameras, mounts, focusers, filter wheels, rotators, switches, and weather stations from iOptron, Player One Astronomy, ZWO, QHY, ToupTek, Skywatcher, Celestron, Gemini, SVBONY, and WeeWX
- **Auto-detection** — USB and Wi-Fi devices are discovered automatically, no manual port configuration required
- **Bundled vendor SDKs and udev rules** — plug in supported USB devices and go
- **Web management UI** — configure devices from any browser on your network

## AlpacaBridge Overview

AlpacaBridge runs on a Debian 13 arm64 machine — a Raspberry Pi at the mount, or an off-the-shelf astronomy control unit like an iOptron iMate, ZWO ASIAIR, or ToupTek StellaVita repurposed with a fresh Debian install. See the [Wiki](https://github.com/open-astro/AlpacaBridge/wiki) for setup guides covering these devices.

ScopeTrader covered the project launch and what it means for the Alpaca ecosystem: [OpenAstro AlpacaBridge Launches and Why It Matters](https://scopetrader.com/openastro-alpacabridge-launches-and-why-it-matters/).

Watch a full install on a ToupTek StellaVita: [Installing AlpacaBridge on a ToupTek StellaVita](https://youtu.be/nVAS45OTltA).

## Supported platforms

- **Debian 13 (Trixie)** on `arm64` (Raspberry Pi 3B+/4/5, Rockchip SBCs, OrangePi)

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
