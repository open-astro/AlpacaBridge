# Driver Development Guide

Complete guide for building vendor-specific drivers for AlpacaCore.

## Overview

AlpacaCore makes it easy to build drivers for any vendor device. The system follows a clean **three-layer architecture** that isolates vendor SDK complexity:

1. **Pure Virtual Interface** - Standard device driver API
2. **SDK Wrapper Layer** - Clean C++ interface wrapping vendor SDKs
3. **Vendor Implementation** - Concrete driver using the wrapper

## Quick Start

1. **Place your vendor SDK** in the `external/` directory (see [external/README.md](../../external/README.md) for details)
2. **Use AI to build your driver** - Reference the comprehensive driver build guide and let AI help you:
   - Create the SDK wrapper layer
   - Implement the driver following AlpacaCore patterns
   - Configure CMake for your vendor SDK
3. **Test thoroughly** - Verify all functionality works as expected

## SDK Placement

See [external/README.md](../../external/README.md) for detailed information on:
- How to place vendor SDKs in the `external/` directory
- What files are tracked vs. not tracked in Git
- SDK folder structure requirements

**Quick Summary**: Simply extract the vendor SDK folder directly into `external/` - no reorganization needed.

## Architecture

### Three-Layer Architecture

AlpacaCore drivers follow a strict three-layer architecture:

#### Layer 1: Pure Virtual Interface

**Location**: `include/alpacacore/<device>_driver.h`

This is the public API that all drivers implement. It already exists - you don't need to modify it.

Example:
```cpp
// include/alpacacore/camera_driver.h
class CameraDriver : public AlpacaDriver {
public:
    virtual ~CameraDriver() = default;
    virtual void start_exposure(double duration, bool light) = 0;
    virtual ImageArray get_image_array() const = 0;
    // ... more methods
};
```

#### Layer 2: SDK Wrapper

**Location**: `include/alpacacore/vendor/<vendor>/<vendor>_sdk_wrapper.h`

Create a clean C++ interface that wraps the messy vendor SDK. This layer:
- Hides SDK complexity from the driver implementation
- Provides clean, standard C++ types
- Isolates SDK dependencies

Example:
```cpp
// include/alpacacore/vendor/zwo/zwo_sdk_wrapper.h
namespace alpacacore::vendor::zwo {

struct ZWOCameraInfo {
    int camera_id;
    std::string name;
    int max_width;
    int max_height;
    bool is_color;
};

class ZWOSDKWrapper {
public:
    static ZWOSDKWrapper& instance();
    std::vector<ZWOCameraInfo> enumerate_cameras();
    bool open_camera(int camera_id);
    // ... more methods
};

} // namespace alpacacore::vendor::zwo
```

**Implementation**: `src/vendors/<vendor>/<vendor>_sdk_wrapper.cpp`

This is the **only place** where vendor SDK headers should be included.

#### Layer 3: Vendor Implementation

**Location**: `src/vendors/<vendor>/<vendor>_<device>_driver.cpp`

Implement the pure virtual interface using the wrapper layer. Never touch the raw SDK directly.

Example:
```cpp
// src/vendors/zwo/zwo_camera_driver.cpp
#include <alpacacore/camera_driver.h>
#include <alpacacore/vendor/zwo/zwo_sdk_wrapper.h>

namespace alpacacore::vendor::zwo {

class ZWOCameraDriver : public CameraDriver {
public:
    void start_exposure(double duration, bool light) override {
        auto& sdk = ZWOSDKWrapper::instance();
        if (!sdk.start_exposure(camera_id_, duration, light)) {
            throw AlpacaException("Failed to start exposure");
        }
    }
    // ... implement all CameraDriver methods
};

} // namespace alpacacore::vendor::zwo
```

### Architecture Principles

- **Vendor SDKs stay in `external/`** - never in core code
- **Wrapper layer isolates SDK complexity** - clean API only
- **Drivers use wrapper, not raw SDK** - maintainability
- **Platform-specific code in CMake** - not in C++ source
- **Standard library only in core** - no vendor dependencies leak

### Gold-Standard Driver Behavior (Required)

To deliver 1:1 Alpaca behavior across vendors, every driver should follow the same runtime semantics:

- **Async connect/disconnect**: Implement `connect()` and `disconnect()` as asynchronous operations that return immediately and perform work in a background task.
- **Connecting state**: Implement `get_connecting()` to report true while connect/disconnect is in progress.
- **Device state snapshot**: Implement `get_device_state()` and return a populated list of Name/Value pairs that represent key device telemetry (e.g., Connected, Tracking, Slewing, Position, Temperatures).
- **Connected property**: Keep `set_connected()` synchronous to support legacy clients; `connect()`/`disconnect()` provide the async workflow.

Use the iOptron telescope driver as the reference implementation for these behaviors.

## Step-by-Step Implementation

### Step 1: Place SDK in `external/`

For ZWO, AlpacaCore uses a **vendored SDK subset** at `external/ASI_Camera_SDK/`,
so no manual extraction is required. For other vendors, extract the SDK folder
directly into `external/`:
```bash
external/
`-- ASI_Camera_SDK/             # Vendored ZWO SDK subset
   |-- include/
   |   `-- ASICamera2.h
   |-- lib/
   |   `-- ...
```

### Step 2: Create Wrapper Header

```bash
mkdir -p include/alpacacore/vendor/zwo
touch include/alpacacore/vendor/zwo/zwo_sdk_wrapper.h
```

Define a clean C++ interface that wraps the SDK (see Layer 2 example above).

### Step 3: Implement Wrapper

```bash
touch src/vendors/zwo/zwo_sdk_wrapper.cpp
```

This is where you include the vendor SDK header and implement the wrapper methods.

### Step 4: Implement Driver

```bash
touch src/vendors/zwo/zwo_camera_driver.cpp
```

Implement all pure virtual methods from `CameraDriver` using the wrapper.

### Step 5: Create Factory Function

```cpp
// In zwo_camera_driver.cpp or separate header
std::unique_ptr<CameraDriver> create_zwo_camera(int device_number, int camera_id) {
    return std::make_unique<ZWOCameraDriver>(device_number, camera_id);
}
```

### Step 6: Configure CMake

Create `src/vendors/zwo/CMakeLists.txt`:

```cmake
if(ALPACACORE_ENABLE_ZWO)
    set(ZWO_SDK_ROOT "${CMAKE_SOURCE_DIR}/external/ASI_Camera_SDK")
    if(NOT EXISTS "${ZWO_SDK_ROOT}/include/ASICamera2.h")
        message(FATAL_ERROR "ZWO SDK header not found in external/ASI_Camera_SDK")
    endif()

    # Select platform/arch library (x64, armv7, armv8, etc.)
    # (See src/vendors/zwo/CMakeLists.txt for the full selection logic.)

    add_library(alpacacore_zwo STATIC
        zwo_sdk_wrapper.cpp
        zwo_camera_driver.cpp
    )
    
    # Include directories
    target_include_directories(alpacacore_zwo PRIVATE
        ${ZWO_SDK_DIR}/include
        ${CMAKE_SOURCE_DIR}/include
    )
    
    # Link SDK library
    target_link_libraries(alpacacore_zwo PRIVATE
        ${ZWO_LIB_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}${ZWO_LIB_NAME}${CMAKE_STATIC_LIBRARY_SUFFIX}
    )
    
    # Link to main library
    # Use PRIVATE to avoid circular dependency warnings
    # The vendor library uses alpacacore headers but doesn't expose them to consumers
    target_link_libraries(alpacacore_zwo PRIVATE alpacacore)
endif()
```

### Step 7: Test Your Driver

See [Testing Guide](testing.md) for comprehensive testing instructions.

## File Organization

```
external/                       # Vendor SDK folders or vendored SDK assets
|-- ASI_Camera_SDK/             # ZWO SDK subset (vendored)
|   `-- SDK files...

include/alpacacore/
|-- <device>_driver.h           # Pure virtual interface
`-- vendor/<vendor>/
    `-- <vendor>_sdk_wrapper.h  # Clean wrapper API

src/
|-- drivers/
|   `-- <device>_driver.cpp     # Base implementation (optional)
`-- vendors/<vendor>/
    |-- <vendor>_sdk_wrapper.cpp      # Wrapper implementation
    |-- <vendor>_<device>_driver.cpp  # Driver implementation
    `-- CMakeLists.txt          # Build configuration
```

## AI-Assisted Development

AlpacaCore is designed for AI-assisted development. When building a driver:

1. **Share the Driver Development Guide** with your AI assistant
   - It contains the architecture rules, patterns, and step-by-step instructions

2. **Describe your SDK and target device**:
   - Example: "I placed the [Vendor] SDK in external/. Help me build a [device type] driver following the Driver Development Guide."

3. **Verify the generated code**:
   - Confirm the wrapper layer isolates SDK headers
   - Confirm the driver implements the full Alpaca interface

## Testing Your Driver

After implementing your driver:

1. **Create test file** in `tests/`:
   ```bash
   touch tests/test_zwo_camera.cpp
   ```

2. **Add to `tests/CMakeLists.txt`**:
   ```cmake
   set(TEST_SOURCES
       # ... existing tests ...
       test_zwo_camera.cpp
   )
   ```

3. **Write tests** following patterns in the [Testing Guide](testing.md)

4. **Build and run**:
   ```bash
   mkdir build
   cd build
   cmake .. -DALPACACORE_ENABLE_ZWO=ON -DALPACACORE_BUILD_TESTS=ON
   cmake --build . --parallel
   ctest
   ```

## Examples

### ZWO Camera Driver

- **SDK Location**: `external/ASI_Camera_SDK/` (vendored subset)
- **Wrapper API**: `ZWOSDKWrapper::enumerate_cameras()`, `ZWOSDKWrapper::open_camera()`, etc.
- **CMake**: Use the fixed `external/ASI_Camera_SDK` path and select the library by OS/arch

### iOptron Telescope Driver

- **Protocol Docs**: `external/ioptron/` (command language markdown)
- **Wrapper API**: `iOptronProtocolWrapper::connect()`, `iOptronProtocolWrapper::get_position()`, etc.
- **CMake**: No SDK discovery needed; just build the wrapper + driver sources

## Troubleshooting

### SDK Not Found

**Error**: `FATAL_ERROR: ZWO SDK not found`

**Solution**: 
1. Verify the vendored SDK subset exists in `external/ASI_Camera_SDK/`
2. Check that the SDK folder contains the expected header file (`include/ASICamera2.h`)
3. Verify CMake is pointing at `external/ASI_Camera_SDK/`

### Linker Errors

**Error**: `undefined reference to SDK functions`

**Solution**:
1. Verify library path in CMakeLists.txt
2. Check platform-specific library naming
3. Ensure library type (STATIC/SHARED) matches SDK

### Include Path Issues

**Error**: `fatal error: ASICamera2.h: No such file or directory`

**Solution**:
1. Check `target_include_directories` in CMakeLists.txt
2. Verify SDK header location
3. Use absolute paths in CMake for reliability

## Best Practices Checklist

- [ ] SDK wrapper uses PIMPL pattern
- [ ] No SDK headers included outside wrapper implementation
- [ ] All SDK errors converted to `AlpacaException`
- [ ] Thread safety documented and handled
- [ ] Platform-specific code only in CMake
- [ ] Unit tests for driver functionality
- [ ] Documentation comments for public API
- [ ] License header in all source files
- [ ] SDK version documented in comments
- [ ] CMakeLists.txt documents which SDK files are used

## Additional Resources

- [external/README.md](../../external/README.md) - SDK placement guide
- [Testing Guide](testing.md) - Comprehensive testing guide
- [Architecture Guide](architecture.md) - Architecture overview
- [ASCOM ConformU](https://github.com/ASCOMInitiative/ConformU/releases) - Official Alpaca conformance tool for validating driver behavior
- [ASCOM Alpaca API Specification](https://ascom-standards.org/api/)

## Quick Reference

### Build Commands

```bash
# Build with vendor support
cmake .. -DALPACACORE_ENABLE_ZWO=ON
cmake --build . --parallel

# Build with tests
cmake .. -DALPACACORE_ENABLE_ZWO=ON -DALPACACORE_BUILD_TESTS=ON
cmake --build . --parallel
ctest
```

### Namespace Convention

```cpp
namespace alpacacore::vendor::zwo {
    // ZWO-specific code
}

namespace alpacacore {
    // Core AlpacaCore code (no vendor dependencies)
}
```

---

**Happy driver building.**
