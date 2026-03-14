cmake_minimum_required(VERSION 3.20)

# Tests use Catch2 macros/includes; require Catch2 (v2 or v3).
find_package(Catch2 QUIET)
if(NOT Catch2_FOUND)
    message(WARNING "Catch2 not found. Install Catch2 (v2 or v3) to build AlpacaCore tests.")
    return()
endif()

set(CATCH2_MAIN_TARGET "")
if(TARGET Catch2::Catch2WithMain)
    set(CATCH2_MAIN_TARGET Catch2::Catch2WithMain)
elseif(TARGET Catch2::Catch2Main)
    set(CATCH2_MAIN_TARGET Catch2::Catch2Main)
endif()

if(CATCH2_MAIN_TARGET STREQUAL "")
    message(WARNING "Catch2 found, but no main target was exported. Skipping AlpacaCore tests.")
    return()
endif()

# Test sources
set(TEST_SOURCES
    test_alpaca_defs.cpp
    test_device_registry.cpp
    test_error_handling.cpp
    test_units.cpp
)

if(TARGET alpacacore_zwo)
    list(APPEND TEST_SOURCES test_zwo_camera.cpp)
    list(APPEND TEST_SOURCES test_zwo_focuser.cpp)
    list(APPEND TEST_SOURCES test_zwo_filterwheel.cpp)
    list(APPEND TEST_SOURCES test_zwo_rotator.cpp)
    list(APPEND TEST_SOURCES test_zwo_switch.cpp)
    list(APPEND TEST_SOURCES test_zwo_telescope.cpp)
endif()

if(TARGET alpacacore_qhy)
    list(APPEND TEST_SOURCES test_qhy_camera.cpp)
endif()
if(TARGET alpacacore_synscan)
    list(APPEND TEST_SOURCES test_synscan_telescope.cpp)
endif()
if(TARGET alpacacore_ioptron)
    list(APPEND TEST_SOURCES test_ioptron_telescope.cpp)
endif()
if(TARGET alpacacore_weewx)
    list(APPEND TEST_SOURCES test_weewx_observingconditions.cpp)
endif()

# Create test executable
add_executable(alpacacore_tests ${TEST_SOURCES})
target_link_libraries(alpacacore_tests PRIVATE alpacacore ${CATCH2_MAIN_TARGET})
if(TARGET alpacacore_zwo)
    target_link_libraries(alpacacore_tests PRIVATE alpacacore_zwo)
endif()
if(TARGET alpacacore_qhy)
    target_link_libraries(alpacacore_tests PRIVATE alpacacore_qhy)
endif()
if(TARGET alpacacore_synscan)
    target_link_libraries(alpacacore_tests PRIVATE alpacacore_synscan)
endif()
if(TARGET alpacacore_ioptron)
    target_link_libraries(alpacacore_tests PRIVATE alpacacore_ioptron)
endif()
if(TARGET alpacacore_weewx)
    target_link_libraries(alpacacore_tests PRIVATE alpacacore_weewx)
endif()

# Both QHY and ZWO SDKs define isLeapYear() at file scope.
if(UNIX AND TARGET alpacacore_qhy AND TARGET alpacacore_zwo)
    target_link_options(alpacacore_tests PRIVATE "LINKER:--allow-multiple-definition")
endif()
include(CTest)
# Include Catch2's test discovery module
include(${Catch2_DIR}/Catch.cmake)
catch_discover_tests(alpacacore_tests)
