# Vendor SDKs Directory

This directory contains vendor SDKs required to build vendor-specific drivers for AlpacaCore.

## Quick Start

1. **Download the vendor SDK** from the vendor's official website
2. **Extract the SDK archive** - the vendor's SDK will typically extract into a folder (e.g., `ASICamera2_SDK/`, `qhy_sdk/`, etc.)
3. **Place the extracted SDK folder** directly into this `external/` directory
   - No need to create subdirectories or reorganize files
   - Use the SDK structure exactly as provided by the vendor
4. **Build with vendor support**:
   ```bash
   mkdir build && cd build
   cmake .. -DALPACACORE_ENABLE_<VENDOR>=ON
   cmake --build . --parallel
   ```

**For a comprehensive driver development guide, see [docs/development/driver-development.md](../docs/development/driver-development.md)**

## Important Notes

- **Vendor SDK files are NOT tracked in Git** - binaries, libraries, headers, and other SDK files are excluded from version control
- **Documentation files ARE tracked** - markdown files (`.md`), README files, and `.gitkeep` files in this directory are tracked in Git
- The `.gitignore` file is configured to exclude SDK files while allowing documentation to be tracked

## What's Tracked vs. Not Tracked

### Tracked in Git
- `README.md` files (this file and any vendor-specific READMEs)
- Markdown documentation files (`.md`) in this directory and subdirectories
- `.gitkeep` files (if used to preserve empty directories)

### Not tracked in Git
- Vendor SDK binaries (`.dylib`, `.so`, `.dll`, `.a`, `.lib`)
- SDK header files (`.h`, `.hpp`)
- SDK source files (`.c`, `.cpp`)
- SDK example code
- Any other vendor-provided SDK files

This keeps the repository clean while allowing important documentation to be version controlled.

## Example: ZWO SDK

If you're building a ZWO camera driver:

1. Download the ZWO ASI Camera SDK from [ZWO's website](https://www.zwoastro.com/)
2. Extract the SDK archive - it might create a folder like `ASICamera2_SDK/` or similar
3. Place that entire folder into `external/`
   - Your `external/` directory will now contain the vendor's SDK folder
   - No renaming or reorganization needed
4. Build with `cmake .. -DALPACACORE_ENABLE_ZWO=ON`

## Building Drivers

For complete instructions on building vendor-specific drivers:

- **See [Driver Development Guide](../docs/development/driver-development.md)** for:
  - Three-layer architecture overview
  - Step-by-step driver implementation
  - CMake configuration
  - Testing your driver
  - Troubleshooting

- **AI-Assisted Development**: Point your AI assistant at the Driver Development Guide for patterns and examples

## License Compliance

**Important**: Vendor SDKs are subject to their own licenses. Ensure you:

- Review and comply with each vendor's SDK license
- Do not redistribute SDK files without proper authorization
- Document any license requirements in your driver implementation

## Additional Resources

- [Driver Development Guide](../docs/development/driver-development.md) - Complete guide to building drivers
- [Architecture Guide](../docs/development/architecture.md) - AlpacaCore architecture overview
- [Building Guide](../docs/building/building.md) - Build instructions
