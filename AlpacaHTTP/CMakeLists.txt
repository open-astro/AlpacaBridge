cmake_minimum_required(VERSION 3.18)

# Read version from workspace VERSION file: project() takes the numeric part
# (ALPACABRIDGE_BASE_VERSION), the version defines below keep the full string
# (ALPACABRIDGE_VERSION, 5.0.0~beta1 for a beta).
include("${CMAKE_CURRENT_SOURCE_DIR}/../AlpacaCore/cmake/AlpacaBridgeVersion.cmake")
alpacabridge_read_version("${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")

project(AlpacaHTTP VERSION ${ALPACABRIDGE_BASE_VERSION} LANGUAGES CXX)

# Git branch/commit for the running build, shown in the web UI so a dev build
# checked out on a feature branch doesn't read as an official release
# (VERSION stays a static release number regardless of what's checked out).
find_package(Git QUIET)
set(ALPACAHTTP_GIT_BRANCH "unknown")
set(ALPACAHTTP_GIT_COMMIT "unknown")
set(ALPACAHTTP_GIT_DIRTY "0")
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE ALPACAHTTP_GIT_BRANCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE ALPACAHTTP_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND ${GIT_EXECUTABLE} diff --quiet HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RESULT_VARIABLE ALPACAHTTP_GIT_DIRTY_RESULT
        ERROR_QUIET
    )
    if(NOT ALPACAHTTP_GIT_DIRTY_RESULT EQUAL 0)
        set(ALPACAHTTP_GIT_DIRTY "1")
    endif()
    if(ALPACAHTTP_GIT_BRANCH STREQUAL "")
        set(ALPACAHTTP_GIT_BRANCH "unknown")
    endif()
    if(ALPACAHTTP_GIT_COMMIT STREQUAL "")
        set(ALPACAHTTP_GIT_COMMIT "unknown")
    endif()
    # A release build is identified by sitting exactly on a vX.Y.Z tag, not by
    # branch name -- packaging checks out the tag, which is detached HEAD
    # (rev-parse --abbrev-ref HEAD returns literal "HEAD", not "main").
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --exact-match --match "v[0-9]*.[0-9]*.[0-9]*" HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RESULT_VARIABLE ALPACAHTTP_GIT_TAG_RESULT
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(ALPACAHTTP_GIT_TAG_RESULT EQUAL 0)
        set(ALPACAHTTP_GIT_IS_RELEASE "1")
    else()
        set(ALPACAHTTP_GIT_IS_RELEASE "0")
    endif()

    # Normalize the origin remote to a browsable https://github.com/<owner>/<repo>
    # URL (handles both https and ssh remote forms) so the build badge can link
    # straight to the commit, without hardcoding a fork's own path. The commit
    # and not the branch: a PR head fetched under an arbitrary local name has
    # no matching branch ref on the remote, but the commit object always
    # resolves there (see updateHeaderBuildBadge in web/app.js).
    execute_process(
        COMMAND ${GIT_EXECUTABLE} remote get-url origin
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE ALPACAHTTP_GIT_REMOTE_RAW
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    set(ALPACAHTTP_GIT_REMOTE_URL "")
    if(ALPACAHTTP_GIT_REMOTE_RAW MATCHES "^git@github\\.com:(.+)$")
        set(ALPACAHTTP_GIT_REMOTE_URL "https://github.com/${CMAKE_MATCH_1}")
    elseif(ALPACAHTTP_GIT_REMOTE_RAW MATCHES "^(https://github\\.com/.+)$")
        set(ALPACAHTTP_GIT_REMOTE_URL "${CMAKE_MATCH_1}")
    endif()
    if(ALPACAHTTP_GIT_REMOTE_URL MATCHES "\\.git$")
        string(REGEX REPLACE "\\.git$" "" ALPACAHTTP_GIT_REMOTE_URL "${ALPACAHTTP_GIT_REMOTE_URL}")
    endif()
endif()
if(NOT DEFINED ALPACAHTTP_GIT_IS_RELEASE)
    set(ALPACAHTTP_GIT_IS_RELEASE "0")
endif()
if(NOT DEFINED ALPACAHTTP_GIT_REMOTE_URL)
    set(ALPACAHTTP_GIT_REMOTE_URL "")
endif()

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
    if(TARGET alpacacore_skywatcher)
        add_definitions(-DALPACACORE_ENABLE_SKYWATCHER)
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
    if(TARGET alpacacore_gphoto)
        add_definitions(-DALPACACORE_ENABLE_GPHOTO)
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
    if(TARGET alpacacore_altair)
        add_definitions(-DALPACACORE_ENABLE_ALTAIR)
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
    if(TARGET alpacacore_skywatcher)
        add_definitions(-DALPACACORE_ENABLE_SKYWATCHER)
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
    if(TARGET alpacacore_gphoto)
        add_definitions(-DALPACACORE_ENABLE_GPHOTO)
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
    if(TARGET alpacacore_altair)
        add_definitions(-DALPACACORE_ENABLE_ALTAIR)
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
    src/core/catalog_json.cpp
    src/http/router.cpp
    src/http/device_call.cpp
    src/http/image_payload.cpp
    src/http/server.cpp
    src/http/thread_join.cpp
    src/util/threading.cpp
    src/util/logging_adapter.cpp
    src/util/error_mapping.cpp
    src/util/host_timezone.cpp
    src/util/wifi_manager.cpp
    src/util/software_update.cpp
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
target_compile_definitions(alpacahttp PUBLIC ALPACAHTTP_VERSION=\"${ALPACABRIDGE_VERSION}\")
# The join test hook (ScopedJoinHooksForTest) is process-wide mutable state and
# sits on every production stop()/wait() join path, so it is compiled only into
# test builds. debian/rules configures with ALPACAHTTP_BUILD_TESTS=OFF, so no
# shipped package contains it. PUBLIC because the tests include the header.
if(ALPACAHTTP_BUILD_TESTS)
    target_compile_definitions(alpacahttp PUBLIC ALPACAHTTP_ENABLE_TEST_HOOKS)
endif()
target_compile_definitions(alpacahttp PUBLIC ALPACAHTTP_GIT_BRANCH="${ALPACAHTTP_GIT_BRANCH}" ALPACAHTTP_GIT_COMMIT="${ALPACAHTTP_GIT_COMMIT}" ALPACAHTTP_GIT_DIRTY=${ALPACAHTTP_GIT_DIRTY} ALPACAHTTP_GIT_IS_RELEASE=${ALPACAHTTP_GIT_IS_RELEASE} ALPACAHTTP_GIT_REMOTE_URL=\"${ALPACAHTTP_GIT_REMOTE_URL}\")

# zlib: gzip framing for the combined log download endpoint
find_package(ZLIB REQUIRED)

# libsystemd: sd-bus client for the NetworkManager-backed wifi endpoints and
# for starting the software-update helper unit (docs/software-update.md)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

# libcurl: the software-update check fetches the APT Packages index
# (already a build dependency through the WeeWX driver; debian/control and
# every CI job install libcurl4-openssl-dev)
find_package(CURL REQUIRED)

target_link_libraries(alpacahttp
    PUBLIC
        nlohmann_json::nlohmann_json
        AlpacaCore::alpacacore
        AlpacaCore::alpacacore_builtins
        ZLIB::ZLIB
        PkgConfig::SYSTEMD
        CURL::libcurl
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
# --version prints alpacacore::kVersion, which needs the definition AlpacaCore sets directory-scoped.
target_compile_definitions(alpacahttp_server PRIVATE ALPACACORE_VERSION="${ALPACABRIDGE_VERSION}")

# Tests
if(ALPACAHTTP_BUILD_TESTS)
    enable_testing()
    if(TARGET alpacacore_svbony)
        add_test(NAME svbony_server_symbol_isolation
            COMMAND "${CMAKE_NM}" -D --defined-only "$<TARGET_FILE:alpacahttp_server>")
        set_tests_properties(svbony_server_symbol_isolation PROPERTIES
            FAIL_REGULAR_EXPRESSION "[ \t]libusb_")
    endif()
    
    # Test executables
    add_executable(test_routing tests/test_routing.cpp)
    target_link_libraries(test_routing alpacahttp)
    # Own working directory (open-astro#274 review): the router resolves its
    # state file as the relative path config/registered_devices.json, so every
    # test binary sharing a cwd shares that file. test_routing now asserts on
    # the file's contents surviving a Router construction, while
    # test_persisted_devices_concurrency storms configuredevice/removedevice,
    # each of which rewrites the whole file -- and ctest -j runs them at the
    # same time. Separate directories make both deterministic.
    set(TEST_ROUTING_DIR ${CMAKE_CURRENT_BINARY_DIR}/test_routing_cwd)
    file(MAKE_DIRECTORY ${TEST_ROUTING_DIR})
    add_test(NAME test_routing COMMAND test_routing WORKING_DIRECTORY ${TEST_ROUTING_DIR})
    # open-astro#664: the devicecatalog endpoint case reads the committed
    # fixture tests/fixtures/devicecatalog.json, located from the source tree
    # the same way test_route_table finds its type-name fixture.
    target_compile_definitions(test_routing PRIVATE
        ALPACAHTTP_ROUTER_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")

    # Test-only server for the hardware-free ConformU job: a test catalog
    # descriptor over FakeSkyWatcherMount. Not installed and not linked into
    # alpacabridge or alpacahttp_server. The fake header lives under
    # AlpacaCore/tests, so that directory is on this target's include path only.
    if(TARGET alpacacore_skywatcher)
        add_executable(fake_skywatcher_server tests/fake_skywatcher_server.cpp)
        target_link_libraries(fake_skywatcher_server alpacahttp alpacacore_skywatcher)
        target_include_directories(fake_skywatcher_server PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../AlpacaCore/tests)
    endif()

    # #646: route-table snapshot, dispatch reach and rejection fixture. Own
    # working directory for the same reason as test_routing (relative
    # config/registered_devices.json), and its own process because the
    # DeviceRegistry is process-wide and this test claims device number 4646
    # for one stub per device type.
    add_executable(test_route_table tests/test_route_table.cpp)
    target_link_libraries(test_route_table alpacahttp)
    target_compile_definitions(test_route_table PRIVATE
        ALPACAHTTP_ROUTER_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")
    set(TEST_ROUTE_TABLE_DIR ${CMAKE_CURRENT_BINARY_DIR}/test_route_table_cwd)
    file(MAKE_DIRECTORY ${TEST_ROUTE_TABLE_DIR})
    add_test(NAME test_route_table COMMAND test_route_table WORKING_DIRECTORY ${TEST_ROUTE_TABLE_DIR})

    # DeviceCall (parameters, envelope, 400-text policy) on a parsed Request:
    # no Router, no driver, no shared registry or config file.
    add_executable(test_device_call tests/test_device_call.cpp)
    target_link_libraries(test_device_call alpacahttp)
    add_test(NAME test_device_call COMMAND test_device_call)

    # Camera imagearray payload (ImageBytes and JSON ImageArray) on synthetic images.
    add_executable(test_image_payload tests/test_image_payload.cpp)
    target_link_libraries(test_image_payload alpacahttp)
    add_test(NAME test_image_payload COMMAND test_image_payload)

    # Threaded storm over the router's persisted_devices_ store (audit 3.0.1
    # follow-up): configure/remove/configureddevices from concurrent threads,
    # mirroring the per-request server threads. Most valuable under TSan, but
    # a pre-mutex router crashes/corrupts here even without it.
    add_executable(test_persisted_devices_concurrency tests/test_persisted_devices_concurrency.cpp)
    target_link_libraries(test_persisted_devices_concurrency alpacahttp)
    set(TEST_PERSISTED_DIR ${CMAKE_CURRENT_BINARY_DIR}/test_persisted_devices_cwd)
    file(MAKE_DIRECTORY ${TEST_PERSISTED_DIR})
    add_test(NAME test_persisted_devices_concurrency COMMAND test_persisted_devices_concurrency
             WORKING_DIRECTORY ${TEST_PERSISTED_DIR})
    
    # Socket-level tests for the Server request-read path (read_request), which
    # test_routing.cpp bypasses. Covers the header-size (431) boundary (#129).
    # Both this target and test_thread_join need src/ on the include path
    # because http/thread_join.h is an internal seam, not public API under
    # include/.
    add_executable(test_server_socket tests/test_server_socket.cpp)
    target_link_libraries(test_server_socket alpacahttp)
    target_include_directories(test_server_socket PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME test_server_socket COMMAND test_server_socket)

    # Unit tests for the join_or_abandon() fallback seam (issue #561).
    add_executable(test_thread_join tests/test_thread_join.cpp)
    target_link_libraries(test_thread_join alpacahttp)
    target_include_directories(test_thread_join PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME test_thread_join COMMAND test_thread_join)

    # open-astro#664: the JSON bridge of the device catalog (src/core/catalog_json.h,
    # an internal seam like thread_join.h, hence src/ on the include path).
    add_executable(test_catalog_json tests/test_catalog_json.cpp)
    target_link_libraries(test_catalog_json alpacahttp)
    target_include_directories(test_catalog_json PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME test_catalog_json COMMAND test_catalog_json)

    # open-astro#765: device config cannot choose the GPIO chip, line or device node. The test
    # replaces libgpiod with recording stubs, so it needs a libgpiod board driver (ZWO ASIAIR,
    # iMate PowerBox or StellaVita) and its own working directory (relative
    # config/registered_devices.json).
    if(TARGET alpacacore_zwo OR ALPACACORE_IOPTRON_POWERBOX OR ALPACACORE_TOUPTEK_STELLAVITA)
        add_executable(test_gpio_config_pin tests/test_gpio_config_pin.cpp)
        target_link_libraries(test_gpio_config_pin alpacahttp)
        target_include_directories(test_gpio_config_pin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        set(TEST_GPIO_CONFIG_PIN_DIR ${CMAKE_CURRENT_BINARY_DIR}/test_gpio_config_pin_cwd)
        file(MAKE_DIRECTORY ${TEST_GPIO_CONFIG_PIN_DIR})
        add_test(NAME test_gpio_config_pin COMMAND test_gpio_config_pin WORKING_DIRECTORY ${TEST_GPIO_CONFIG_PIN_DIR})
    endif()

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

    add_executable(test_log_text tests/test_log_text.cpp)
    target_link_libraries(test_log_text alpacahttp)
    add_test(NAME test_log_text COMMAND test_log_text)

    # open-astro#1333: log level of the router's line for a driver exception.
    # Own working directory, like test_routing: constructing a Router reads
    # config/registered_devices.json relative to the cwd.
    add_executable(test_router_log_level tests/test_router_log_level.cpp)
    target_link_libraries(test_router_log_level alpacahttp)
    set(TEST_ROUTER_LOG_LEVEL_DIR ${CMAKE_CURRENT_BINARY_DIR}/test_router_log_level_cwd)
    file(MAKE_DIRECTORY ${TEST_ROUTER_LOG_LEVEL_DIR})
    add_test(NAME test_router_log_level COMMAND test_router_log_level WORKING_DIRECTORY ${TEST_ROUTER_LOG_LEVEL_DIR})

    # open-astro#1756: log-level spellings, ALPACAHTTP_PORT range, non-string loglevel.
    add_executable(test_loglevel_port tests/test_loglevel_port.cpp)
    target_link_libraries(test_loglevel_port alpacahttp)
    set(TEST_LOGLEVEL_PORT_DIR ${CMAKE_CURRENT_BINARY_DIR}/test_loglevel_port_cwd)
    file(MAKE_DIRECTORY ${TEST_LOGLEVEL_PORT_DIR}/config)
    add_test(NAME test_loglevel_port COMMAND test_loglevel_port WORKING_DIRECTORY ${TEST_LOGLEVEL_PORT_DIR})

    # open-astro#1688, #1689: settings files survive a failed write and a corrupt read.
    # Own working directory: the router resolves config/ relative to the cwd.
    add_executable(test_atomic_settings tests/test_atomic_settings.cpp)
    target_link_libraries(test_atomic_settings alpacahttp)
    set(TEST_ATOMIC_SETTINGS_DIR ${CMAKE_CURRENT_BINARY_DIR}/test_atomic_settings_cwd)
    file(MAKE_DIRECTORY ${TEST_ATOMIC_SETTINGS_DIR})
    add_test(NAME test_atomic_settings COMMAND test_atomic_settings WORKING_DIRECTORY ${TEST_ATOMIC_SETTINGS_DIR})

    add_executable(test_yaml_comment tests/test_yaml_comment.cpp)
    target_link_libraries(test_yaml_comment alpacahttp)
    add_test(NAME test_yaml_comment COMMAND test_yaml_comment)

    add_executable(test_wifi_manager tests/test_wifi_manager.cpp)
    target_link_libraries(test_wifi_manager alpacahttp)
    add_test(NAME test_wifi_manager COMMAND test_wifi_manager)

    add_executable(test_wifi_ssid tests/test_wifi_ssid.cpp)
    target_link_libraries(test_wifi_ssid alpacahttp)
    add_test(NAME test_wifi_ssid COMMAND test_wifi_ssid)

    # Issue #764: the JSON sink must not throw on non-UTF-8 bytes (Wi-Fi SSIDs).
    add_executable(test_response_utf8 tests/test_response_utf8.cpp)
    target_link_libraries(test_response_utf8 alpacahttp)
    add_test(NAME test_response_utf8 COMMAND test_response_utf8)

    # Software update: dpkg version ordering, Packages index parsing and the
    # manager's policy over a scripted backend (docs/software-update.md).
    add_executable(test_software_update tests/test_software_update.cpp)
    target_link_libraries(test_software_update alpacahttp)
    # It reads debian/alpacabridge-software-update and the two unit files to
    # pin the transcript path on both sides of the root helper.
    target_compile_definitions(test_software_update PRIVATE
        ALPACAHTTP_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
    add_test(NAME test_software_update COMMAND test_software_update)

    # Issue #1015: the server binary's command line. --help and --version must
    # print and exit before anything starts (TIMEOUT: a regression that starts
    # the server hangs, so it fails instead); a flag it does not know and an
    # unreadable config path must exit non-zero.
    # PASS_REGULAR_EXPRESSION would make CTest ignore the exit status, so every
    # case goes through sh and pins the exact exit status and the stream.
    # $1 server, $2 expected exit status, $3 expected text, rest the arguments.
    set(SERVER_CLI_EXPECT_OUT [=[server=$1; want=$2; text=$3; shift 3
out=$("$server" "$@" 2>/dev/null); rc=$?
test "$rc" = "$want" && printf '%s' "$out" | tr '\n' ' ' | grep -q -- "$text"]=])
    set(SERVER_CLI_EXPECT [=[server=$1; want=$2; text=$3; shift 3
err=$("$server" "$@" 2>&1 >/dev/null); rc=$?
test "$rc" = "$want" && printf '%s' "$err" | grep -q -- "$text"]=])
    add_test(NAME server_cli_help
        COMMAND sh -c "${SERVER_CLI_EXPECT_OUT}" sh $<TARGET_FILE:alpacahttp_server> 0 "Usage: alpacabridge.*ALPACAHTTP_PORT" --help)
    add_test(NAME server_cli_help_short
        COMMAND sh -c "${SERVER_CLI_EXPECT_OUT}" sh $<TARGET_FILE:alpacahttp_server> 0 "Usage: alpacabridge" -h)
    add_test(NAME server_cli_version
        COMMAND sh -c "${SERVER_CLI_EXPECT_OUT}" sh $<TARGET_FILE:alpacahttp_server> 0 "^[0-9]*[.][0-9]*[.][0-9]*" --version)
    set_tests_properties(server_cli_help server_cli_help_short server_cli_version PROPERTIES TIMEOUT 20)
    add_test(NAME server_cli_unknown_option
        COMMAND sh -c "${SERVER_CLI_EXPECT}" sh $<TARGET_FILE:alpacahttp_server> 2 "unknown option '--bogus'" --bogus)
    add_test(NAME server_cli_extra_argument
        COMMAND sh -c "${SERVER_CLI_EXPECT}" sh $<TARGET_FILE:alpacahttp_server> 2 "at most one" a b)
    add_test(NAME server_cli_missing_config
        COMMAND sh -c "${SERVER_CLI_EXPECT}" sh $<TARGET_FILE:alpacahttp_server> 1 "cannot read config file"
            ${CMAKE_CURRENT_BINARY_DIR}/no_such_config.yaml)
    set_tests_properties(server_cli_unknown_option server_cli_extra_argument server_cli_missing_config PROPERTIES TIMEOUT 20)
endif()
