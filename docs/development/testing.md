# Testing Guide

This guide covers building, running, and writing tests for AlpacaCore.

## Prerequisites

To build and run tests, you need a testing framework installed:

- **Catch2** (recommended): 
  - macOS: `brew install catch2`
  - Linux: Use your package manager (e.g., `sudo apt-get install catch2`)
  - Windows: Use vcpkg or download from https://github.com/catchorg/Catch2
- **doctest** (fallback): Alternative testing framework

The build system will automatically detect and use whichever is available.

## Building Tests

Tests are built by default when `ALPACACORE_BUILD_TESTS` is enabled (default: ON):

```bash
mkdir build
cd build
cmake .. -DALPACACORE_BUILD_TESTS=ON
cmake --build . --parallel
```

To disable tests:

```bash
cmake .. -DALPACACORE_BUILD_TESTS=OFF
```

## Running Tests

### Using CTest

After building with tests enabled:

```bash
cd build
ctest
```

For verbose output:

```bash
ctest --verbose
```

### Running Test Executable Directly

```bash
./build/tests/alpacacore_tests
```

### Running Specific Tests

Run tests by tag:

```bash
# Run only ZWO camera tests
./build/tests/alpacacore_tests [zwo][camera]

# Run only unit tests (skip hardware tests)
./build/tests/alpacacore_tests [unit]

# Run integration tests
./build/tests/alpacacore_tests [integration]

# Exclude hardware tests
./build/tests/alpacacore_tests ~[hardware]
```

Run specific test by name:

```bash
./build/tests/alpacacore_tests "ZWO Camera Driver - Exposure Control"
```

### Test Output Options

```bash
# List all tests
./build/tests/alpacacore_tests --list-tests

# Verbose output
./build/tests/alpacacore_tests --success

# XML output for CI
./build/tests/alpacacore_tests --reporter xml > test_results.xml

# JUnit format
./build/tests/alpacacore_tests --reporter junit > junit.xml
```

## Writing Tests

AlpacaCore uses **Catch2** as its testing framework. Tests are located in the `tests/` directory.

### Basic Test Structure

```cpp
// tests/test_zwo_camera.cpp
#include <catch2/catch_all.hpp>
#include <alpacacore/camera_driver.h>
#include <alpacacore/vendor/zwo/zwo_camera_driver.h>

TEST_CASE("ZWO Camera Driver - Basic Operations", "[zwo][camera]") {
    auto driver = create_zwo_camera(0, 0);
    
    SECTION("Device information") {
        REQUIRE(driver->get_device_type() == DeviceType::Camera);
        REQUIRE_FALSE(driver->get_name().empty());
    }
    
    SECTION("Connection state") {
        REQUIRE(driver->get_connected() == false);
    }
}
```

### Test Tags

Use tags to organize and filter tests:

- **Vendor tags**: `[zwo]`, `[qhy]`, `[ioptron]` - Identify vendor-specific tests
- **Device tags**: `[camera]`, `[telescope]`, `[focuser]` - Identify device type
- **Test type tags**: `[unit]`, `[integration]`, `[hardware]` - Identify test category

Example:

```cpp
TEST_CASE("ZWO Camera - Exposure Test", "[zwo][camera][unit]") {
    // Unit test
}

TEST_CASE("ZWO Camera - Hardware Integration", "[zwo][camera][integration][hardware]") {
    // Integration test requiring hardware
}
```

### Common Assertions

```cpp
REQUIRE(condition);                    // Assert and stop on failure
CHECK(condition);                     // Assert and continue on failure
REQUIRE_THROWS_AS(expr, Exception);   // Assert exception type
REQUIRE_THROWS_WITH(expr, "message"); // Assert exception message
REQUIRE_NOTHROW(expr);                // Assert no exception
SKIP("reason");                        // Skip test with reason
```

### Adding Tests to Test Suite

1. **Create test file** in `tests/`:
   ```bash
   touch tests/test_zwo_camera.cpp
   ```

2. **Add to `tests/CMakeLists.txt`**:
   ```cmake
   set(TEST_SOURCES
       test_camera.cpp
       test_telescope.cpp
       test_management.cpp
       test_units.cpp
       test_zwo_camera.cpp      # Add your test
   )
   ```

3. **Write tests** following the patterns above

## Testing Patterns

### Testing Device Information

```cpp
TEST_CASE("ZWO Camera Driver - Device Information", "[zwo][camera][unit]") {
    auto driver = create_zwo_camera(0, 0);
    
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_device_type() == DeviceType::Camera);
    REQUIRE_FALSE(driver->get_name().empty());
    REQUIRE_FALSE(driver->get_unique_id().empty());
}
```

### Testing Connection States

```cpp
TEST_CASE("ZWO Camera Driver - Connection Management", "[zwo][camera][unit]") {
    auto driver = create_zwo_camera(0, 0);
    
    SECTION("Initial state is disconnected") {
        REQUIRE(driver->get_connected() == false);
    }
    
    SECTION("Connect when hardware available") {
        if (has_zwo_camera_available()) {
            driver->set_connected(true);
            REQUIRE(driver->get_connected() == true);
        } else {
            SKIP("No ZWO camera available");
        }
    }
}
```

### Testing Error Conditions

```cpp
TEST_CASE("ZWO Camera Driver - Error Conditions", "[zwo][camera][unit]") {
    auto driver = create_zwo_camera(0, 0);
    
    SECTION("Start exposure when disconnected throws") {
        REQUIRE_THROWS_AS(
            driver->start_exposure(1.0, true),
            AlpacaException
        );
    }
    
    SECTION("Invalid exposure duration throws") {
        if (has_zwo_camera_available()) {
            driver->set_connected(true);
            REQUIRE_THROWS_AS(
                driver->start_exposure(-1.0, true),
                AlpacaException
            );
        }
    }
}
```

## Hardware Testing

For tests that require physical hardware:

1. **Check hardware availability** before running hardware tests
2. **Use SKIP** instead of REQUIRE when hardware is unavailable
3. **Tag with `[hardware]`** to allow filtering in CI
4. **Add timeouts** for operations that may hang

Example:

```cpp
TEST_CASE("ZWO Camera - Hardware Integration", "[zwo][camera][integration][hardware]") {
    if (!has_zwo_camera_available()) {
        SKIP("No ZWO camera available for hardware testing");
    }
    
    auto driver = create_zwo_camera(0, 0);
    driver->set_connected(true);
    
    // Hardware test code...
}
```

## CI/CD Integration

Tests can be integrated into CI/CD pipelines. See the [Architecture Guide](architecture.md) for CI/CD examples and best practices.

## Best Practices

1. **Test Independence**: Each test should be independent - no shared state
2. **Setup and Teardown**: Handle setup/teardown in each test
3. **Use SKIP for Missing Hardware**: Don't fail tests when hardware unavailable
4. **Test Both Positive and Negative Cases**: Test success and error conditions
5. **Test Boundary Conditions**: Test min, max, zero, negative values
6. **Clear Test Names**: Use descriptive test names
7. **Tag Appropriately**: Use tags to organize and filter tests

## Troubleshooting

### Tests Not Discovered

**Problem**: Tests don't appear when running `ctest`

**Solution**:
1. Verify test file is in `tests/CMakeLists.txt` `TEST_SOURCES`
2. Rebuild CMake: `cmake ..`
3. Check Catch2 is found: `cmake ..` should show Catch2 location

### Linker Errors

**Problem**: `undefined reference to vendor driver functions`

**Solution**:
1. Ensure vendor library is linked in `tests/CMakeLists.txt`
2. Ensure vendor is enabled: `-DALPACACORE_ENABLE_<VENDOR>=ON`

### Hardware Tests Failing in CI

**Problem**: Hardware tests fail because hardware isn't available

**Solution**:
1. Use `SKIP` instead of `REQUIRE` when hardware unavailable
2. Filter out hardware tests in CI: `ctest -E "\[hardware\]"`
3. Mark hardware tests with `[hardware]` tag

## Additional Resources

- [Catch2 Documentation](https://github.com/catchorg/Catch2)
- [Driver Development Guide](driver-development.md) - For testing vendor drivers
- [Architecture Guide](architecture.md) - For CI/CD integration examples
