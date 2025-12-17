cmake_minimum_required(VERSION 3.20)
project(AlpacaCore VERSION 0.1.0 LANGUAGES CXX)

# C++20 required
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options
option(ALPACACORE_BUILD_TESTS "Build unit tests" ON)
option(ALPACACORE_ENABLE_ALL_VENDORS "Enable all vendor drivers" OFF)
option(ALPACACORE_ENABLE_ZWO "Enable ZWO vendor support" OFF)
option(ALPACACORE_ENABLE_QHY "Enable QHY vendor support" OFF)
option(ALPACACORE_ENABLE_IOPTRON "Enable iOptron vendor support" OFF)

# If ALPACACORE_ENABLE_ALL_VENDORS is set, enable all implemented vendor drivers
# Note: Only enable vendors that are actually implemented (have source files)
if(ALPACACORE_ENABLE_ALL_VENDORS)
    # ZWO and QHY are not yet implemented, so don't enable them
    # set(ALPACACORE_ENABLE_ZWO ON)
    # set(ALPACACORE_ENABLE_QHY ON)
    set(ALPACACORE_ENABLE_IOPTRON ON)
endif()

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

# Create namespace alias for consistent target naming
add_library(AlpacaCore::alpacacore ALIAS alpacacore)

# Vendor support (conditional)
# Only add subdirectories for vendors that are actually implemented
# Check if CMakeLists.txt exists and is not empty before adding
if(ALPACACORE_ENABLE_ZWO)
    # ZWO not yet implemented - skip for now
    # add_subdirectory(src/vendors/zwo)
    # if(TARGET alpacacore_zwo)
    #     target_link_libraries(alpacacore PRIVATE alpacacore_zwo)
    # endif()
    message(WARNING "ZWO vendor support is not yet implemented")
endif()

if(ALPACACORE_ENABLE_QHY)
    # QHY not yet implemented - skip for now
    # add_subdirectory(src/vendors/qhy)
    # if(TARGET alpacacore_qhy)
    #     target_link_libraries(alpacacore PRIVATE alpacacore_qhy)
    # endif()
    message(WARNING "QHY vendor support is not yet implemented")
endif()

if(ALPACACORE_ENABLE_IOPTRON)
    add_subdirectory(src/vendors/ioptron)
    if(TARGET alpacacore_ioptron)
        target_link_libraries(alpacacore PRIVATE alpacacore_ioptron)
    endif()
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

# Install vendor libraries if they exist
if(ALPACACORE_ENABLE_ZWO AND TARGET alpacacore_zwo)
    install(TARGETS alpacacore_zwo
        EXPORT AlpacaCoreTargets
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
    )
endif()

if(ALPACACORE_ENABLE_QHY AND TARGET alpacacore_qhy)
    install(TARGETS alpacacore_qhy
        EXPORT AlpacaCoreTargets
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
    )
endif()

if(ALPACACORE_ENABLE_IOPTRON AND TARGET alpacacore_ioptron)
    install(TARGETS alpacacore_ioptron
        EXPORT AlpacaCoreTargets
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
    )
endif()

install(DIRECTORY include/alpacacore
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

install(EXPORT AlpacaCoreTargets
    FILE AlpacaCoreTargets.cmake
    NAMESPACE AlpacaCore::
    DESTINATION lib/cmake/AlpacaCore
)

