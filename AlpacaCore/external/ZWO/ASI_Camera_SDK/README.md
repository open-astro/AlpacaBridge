# ZWO ASI Camera SDK (Vendored Subset)

This folder contains a **minimal, vendored subset** of the ZWO ASI Camera SDK
needed to build AlpacaCore's ZWO camera driver across supported OS/architectures.

## Contents

- `include/ASICamera2.h` - SDK header (C API)
- `license.txt` - ZWO SDK license (per vendor)
- `lib/windows/{x64,x86}/ASICamera2.{dll,lib}`
- `lib/linux/{x64,x86,armv6,armv7,armv8}/libASICamera2.a`
- `lib/linux/asi.rules` - udev rules (Linux USB permissions)
- `lib/mac/{x64,arm64}/libASICamera2.a`

## Notes

- These files are sourced from ZWO SDK v1.40 and are included for convenience.
- The original vendor archives may still exist in this directory but are **not**
  used by the build system.
- If you update the SDK version, update this README and keep the set of files
  minimal (header + platform libraries only).
- On macOS and Linux, ensure `libusb-1.0` is available and install `lib/linux/asi.rules` on Linux
  for USB device permissions.
