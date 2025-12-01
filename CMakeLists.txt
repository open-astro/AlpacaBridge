cmake_minimum_required(VERSION 3.20)
project(AlpacaCore VERSION 0.1.0 LANGUAGES CXX)

# C++20 required
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options
option(ALPACACORE_BUILD_TESTS "Build unit tests" ON)
option(ALPACACORE_ENABLE_ZWO "Enable ZWO vendor support" OFF)
option(ALPACACORE_ENABLE_QHY "Enable QHY vendor support" OFF)
option(ALPACACORE_ENABLE_IOPTRON "Enable iOptron vendor support" OFF)

# Compiler warnings
if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# Include directories
include_directories(include)

# Core library sources
set(ALPACACORE_CORE_SOURCES
    src/core/alpacadriver.cpp
    src/core/alpaca_defs.cpp
    src/core/alpaca_errors.cpp
    src/core/alpaca_json.cpp
    src/core/managementdriver.cpp
    src/core/device_capabilities.cpp
    src/core/util/logging.cpp
    src/core/util/threading.cpp
    src/core/util/error_handling.cpp
    src/core/util/units.cpp
)

# Driver sources
set(ALPACACORE_DRIVER_SOURCES
    src/drivers/camera_driver.cpp
    src/drivers/telescope_driver.cpp
    src/drivers/filterwheel_driver.cpp
    src/drivers/focuser_driver.cpp
    src/drivers/rotator_driver.cpp
    src/drivers/dome_driver.cpp
    src/drivers/shutter_driver.cpp
    src/drivers/switch_driver.cpp
    src/drivers/covercalibrator_driver.cpp
    src/drivers/observingconditions_driver.cpp
    src/drivers/safetymonitor_driver.cpp
)

# Management sources
set(ALPACACORE_MANAGEMENT_SOURCES
    src/management/discovery_service.cpp
    src/management/device_registry.cpp
)

# Create the main library
add_library(alpacacore STATIC
    ${ALPACACORE_CORE_SOURCES}
    ${ALPACACORE_DRIVER_SOURCES}
    ${ALPACACORE_MANAGEMENT_SOURCES}
)

target_include_directories(alpacacore PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Vendor support (conditional)
if(ALPACACORE_ENABLE_ZWO)
    add_subdirectory(src/vendors/zwo)
    target_link_libraries(alpacacore PRIVATE alpacacore_zwo)
endif()

if(ALPACACORE_ENABLE_QHY)
    add_subdirectory(src/vendors/qhy)
    target_link_libraries(alpacacore PRIVATE alpacacore_qhy)
endif()

if(ALPACACORE_ENABLE_IOPTRON)
    add_subdirectory(src/vendors/ioptron)
    target_link_libraries(alpacacore PRIVATE alpacacore_ioptron)
endif()

# Tests
if(ALPACACORE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Examples
add_subdirectory(examples)

# Installation
install(TARGETS alpacacore
    EXPORT AlpacaCoreTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/alpacacore
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

install(EXPORT AlpacaCoreTargets
    FILE AlpacaCoreTargets.cmake
    NAMESPACE AlpacaCore::
    DESTINATION lib/cmake/AlpacaCore
)

