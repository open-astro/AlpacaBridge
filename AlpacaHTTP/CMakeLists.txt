cmake_minimum_required(VERSION 3.15)

# Read version from workspace VERSION file
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" PROJECT_VERSION LIMIT_COUNT 1)
string(STRIP "${PROJECT_VERSION}" PROJECT_VERSION)

project(AlpacaHTTP VERSION ${PROJECT_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options
option(ALPACAHTTP_BUILD_TESTS "Build test suite" ON)
option(ALPACAHTTP_ENABLE_DISCOVERY "Enable Alpaca Discovery" ON)

# Find dependencies
find_package(nlohmann_json 3.2.0 QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# Find AlpacaCore
# Try to find AlpacaCore as a package first, then fall back to subdirectory
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../AlpacaCore/CMakeLists.txt")
    # AlpacaCore is in the same workspace
    add_subdirectory(../AlpacaCore ${CMAKE_BINARY_DIR}/AlpacaCore)
    
    # Propagate AlpacaCore vendor targets as compile definitions for AlpacaHTTP
    # This keeps AlpacaHTTP in sync with the drivers that actually built.
    if(TARGET alpacacore_ioptron)
        add_definitions(-DALPACACORE_ENABLE_IOPTRON)
    endif()
    # The iMate PowerBox Switch is only compiled into the iOptron vendor lib
    # when libgpiod (>= 2.0) was available; guard the router branch to match.
    if(ALPACACORE_IOPTRON_POWERBOX)
        add_definitions(-DALPACACORE_IOPTRON_POWERBOX)
    endif()
    if(TARGET alpacacore_synscan)
        add_definitions(-DALPACACORE_ENABLE_SYNSCAN)
    endif()
    if(TARGET alpacacore_onstep)
        add_definitions(-DALPACACORE_ENABLE_ONSTEP)
    endif()
    if(TARGET alpacacore_zwo)
        add_definitions(-DALPACACORE_ENABLE_ZWO)
    endif()
    if(TARGET alpacacore_qhy)
        add_definitions(-DALPACACORE_ENABLE_QHY)
    endif()
    if(TARGET alpacacore_weewx)
        add_definitions(-DALPACACORE_ENABLE_WEEWX)
    endif()
    if(TARGET alpacacore_gemini)
        add_definitions(-DALPACACORE_ENABLE_GEMINI)
    endif()
    if(TARGET alpacacore_wandererastro)
        add_definitions(-DALPACACORE_ENABLE_WANDERERASTRO)
    endif()
    if(TARGET alpacacore_svbony)
        add_definitions(-DALPACACORE_ENABLE_SVBONY)
    endif()
    if(TARGET alpacacore_celestron)
        add_definitions(-DALPACACORE_ENABLE_CELESTRON)
    endif()
    if(TARGET alpacacore_bisque)
        add_definitions(-DALPACACORE_ENABLE_BISQUE)
    endif()
    if(TARGET alpacacore_touptek)
        add_definitions(-DALPACACORE_ENABLE_TOUPTEK)
    endif()
    # The StellaVita Switch is only compiled into the ToupTek vendor lib when
    # libgpiod (>= 2.0) was available; guard the router branch to match.
    if(ALPACACORE_TOUPTEK_STELLAVITA)
        add_definitions(-DALPACACORE_TOUPTEK_STELLAVITA)
    endif()
    if(TARGET alpacacore_playerone)
        add_definitions(-DALPACACORE_ENABLE_PLAYERONE)
    endif()
    if(TARGET alpacacore_astroasis)
        add_definitions(-DALPACACORE_ENABLE_ASTROASIS)
    endif()
else()
    # AlpacaCore is installed system-wide
    find_package(AlpacaCore REQUIRED)
    # For installed packages, vendor support is determined by what was built
    # Check if vendor libraries exist (this is a best-effort check)
    if(TARGET alpacacore_ioptron)
        add_definitions(-DALPACACORE_ENABLE_IOPTRON)
    endif()
    # The iMate PowerBox Switch is only compiled into the iOptron vendor lib
    # when libgpiod (>= 2.0) was available; guard the router branch to match.
    if(ALPACACORE_IOPTRON_POWERBOX)
        add_definitions(-DALPACACORE_IOPTRON_POWERBOX)
    endif()
    if(TARGET alpacacore_synscan)
        add_definitions(-DALPACACORE_ENABLE_SYNSCAN)
    endif()
    if(TARGET alpacacore_onstep)
        add_definitions(-DALPACACORE_ENABLE_ONSTEP)
    endif()
    if(TARGET alpacacore_zwo)
        add_definitions(-DALPACACORE_ENABLE_ZWO)
    endif()
    if(TARGET alpacacore_qhy)
        add_definitions(-DALPACACORE_ENABLE_QHY)
    endif()
    if(TARGET alpacacore_weewx)
        add_definitions(-DALPACACORE_ENABLE_WEEWX)
    endif()
    if(TARGET alpacacore_gemini)
        add_definitions(-DALPACACORE_ENABLE_GEMINI)
    endif()
    if(TARGET alpacacore_wandererastro)
        add_definitions(-DALPACACORE_ENABLE_WANDERERASTRO)
    endif()
    if(TARGET alpacacore_svbony)
        add_definitions(-DALPACACORE_ENABLE_SVBONY)
    endif()
    if(TARGET alpacacore_celestron)
        add_definitions(-DALPACACORE_ENABLE_CELESTRON)
    endif()
    if(TARGET alpacacore_bisque)
        add_definitions(-DALPACACORE_ENABLE_BISQUE)
    endif()
    if(TARGET alpacacore_touptek)
        add_definitions(-DALPACACORE_ENABLE_TOUPTEK)
    endif()
    # The StellaVita Switch is only compiled into the ToupTek vendor lib when
    # libgpiod (>= 2.0) was available; guard the router branch to match.
    if(ALPACACORE_TOUPTEK_STELLAVITA)
        add_definitions(-DALPACACORE_TOUPTEK_STELLAVITA)
    endif()
    if(TARGET alpacacore_playerone)
        add_definitions(-DALPACACORE_ENABLE_PLAYERONE)
    endif()
    if(TARGET alpacacore_astroasis)
        add_definitions(-DALPACACORE_ENABLE_ASTROASIS)
    endif()
endif()

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Source files
set(ALPACAHTTP_SOURCES
    src/core/request.cpp
    src/core/response.cpp
    src/core/config.cpp
    src/core/json_utils.cpp
    src/http/router.cpp
    src/http/server.cpp
    src/util/threading.cpp
    src/util/logging_adapter.cpp
    src/util/error_mapping.cpp
    src/util/wifi_manager.cpp
)

if(ALPACAHTTP_ENABLE_DISCOVERY)
    list(APPEND ALPACAHTTP_SOURCES src/discovery/discovery.cpp)
    add_definitions(-DALPACAHTTP_ENABLE_DISCOVERY)
endif()

# Custom target to clean everything including CMake cache
# Usage: cmake --build . --target alpacahttp-clean-all
# Note: Named with project prefix to avoid conflicts with AlpacaCore subdirectory
if(NOT TARGET alpacahttp-clean-all)
    add_custom_target(alpacahttp-clean-all
        COMMAND ${CMAKE_COMMAND} --build . --target clean
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/CMakeFiles
        COMMAND ${CMAKE_COMMAND} -E remove ${CMAKE_BINARY_DIR}/CMakeCache.txt
        COMMAND ${CMAKE_COMMAND} -E remove ${CMAKE_BINARY_DIR}/cmake_install.cmake
        COMMAND ${CMAKE_COMMAND} -E remove ${CMAKE_BINARY_DIR}/Makefile
        COMMAND ${CMAKE_COMMAND} -E remove ${CMAKE_BINARY_DIR}/CTestTestfile.cmake
        COMMAND ${CMAKE_COMMAND} -E remove ${CMAKE_BINARY_DIR}/DartConfiguration.tcl
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/Testing
        COMMENT "Cleaning all AlpacaHTTP build files and CMake cache"
    )
    # Only create alias when building standalone (not as subdirectory)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT TARGET clean-all)
        add_custom_target(clean-all ALIAS alpacahttp-clean-all)
    endif()
endif()

# Create library
add_library(alpacahttp STATIC ${ALPACAHTTP_SOURCES})
target_compile_definitions(alpacahttp PUBLIC ALPACAHTTP_VERSION=\"${PROJECT_VERSION}\")

# zlib: gzip framing for the combined log download endpoint
find_package(ZLIB REQUIRED)

# libsystemd: sd-bus client for the NetworkManager-backed wifi endpoints
find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

target_link_libraries(alpacahttp
    PUBLIC
        nlohmann_json::nlohmann_json
        AlpacaCore::alpacacore
        ZLIB::ZLIB
        PkgConfig::SYSTEMD
)

# Both QHY and ZWO SDKs define isLeapYear() at file scope — silence the
# duplicate-symbol error that arises when both vendor libs are linked together.
if(UNIX AND TARGET alpacacore_qhy AND TARGET alpacacore_zwo)
    target_link_options(alpacahttp PUBLIC "LINKER:--allow-multiple-definition")
endif()

# Install
install(TARGETS alpacahttp
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/alpacahttp
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# Example application
add_executable(alpacahttp_server examples/simple_server/main.cpp)
target_link_libraries(alpacahttp_server alpacahttp)

# Tests
if(ALPACAHTTP_BUILD_TESTS)
    enable_testing()
    
    # Test executables
    add_executable(test_routing tests/test_routing.cpp)
    target_link_libraries(test_routing alpacahttp)
    add_test(NAME test_routing COMMAND test_routing)

    # Threaded storm over the router's persisted_devices_ store (audit 3.0.1
    # follow-up): configure/remove/configureddevices from concurrent threads,
    # mirroring the per-request server threads. Most valuable under TSan, but
    # a pre-mutex router crashes/corrupts here even without it.
    add_executable(test_persisted_devices_concurrency tests/test_persisted_devices_concurrency.cpp)
    target_link_libraries(test_persisted_devices_concurrency alpacahttp)
    add_test(NAME test_persisted_devices_concurrency COMMAND test_persisted_devices_concurrency)
    
    # Socket-level tests for the Server request-read path (read_request), which
    # test_routing.cpp bypasses. Covers the header-size (431) boundary (#129).
    add_executable(test_server_socket tests/test_server_socket.cpp)
    target_link_libraries(test_server_socket alpacahttp)
    add_test(NAME test_server_socket COMMAND test_server_socket)

    add_executable(test_json tests/test_json.cpp)
    target_link_libraries(test_json alpacahttp)
    add_test(NAME test_json COMMAND test_json)
    
    if(ALPACAHTTP_ENABLE_DISCOVERY)
        add_executable(test_discovery tests/test_discovery.cpp)
        target_link_libraries(test_discovery alpacahttp)
        add_test(NAME test_discovery COMMAND test_discovery)
    endif()
    
    add_executable(test_config tests/test_config.cpp)
    target_link_libraries(test_config alpacahttp)
    add_test(NAME test_config COMMAND test_config)
endif()
