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
    test_async_connectable.cpp
    test_device_registry.cpp
    test_error_handling.cpp
    test_poll_settle.cpp
    test_serial_by_id_scan.cpp
    test_units.cpp
    test_version_format.cpp
)

if(TARGET alpacacore_zwo)
    list(APPEND TEST_SOURCES test_zwo_camera.cpp)
    list(APPEND TEST_SOURCES test_zwo_focuser.cpp)
    list(APPEND TEST_SOURCES test_zwo_filterwheel.cpp)
    list(APPEND TEST_SOURCES test_zwo_concurrency_stress.cpp)
    list(APPEND TEST_SOURCES test_zwo_rotator.cpp)
    list(APPEND TEST_SOURCES test_zwo_switch.cpp)
    list(APPEND TEST_SOURCES test_zwo_asiair_switch.cpp)
    list(APPEND TEST_SOURCES test_zwo_asiair_plus_switch.cpp)
    list(APPEND TEST_SOURCES test_zwo_telescope.cpp)
endif()

if(TARGET alpacacore_qhy)
    list(APPEND TEST_SOURCES test_qhy_camera.cpp)
    list(APPEND TEST_SOURCES test_qhy_filterwheel.cpp)
endif()
if(TARGET alpacacore_synscan)
    list(APPEND TEST_SOURCES test_synscan_telescope.cpp)
    # Telescope lifecycle storm over the FakeMountServer seam (audit 3.0.1
    # follow-up): async slew / pulse-guide threads racing disconnect and
    # destruction; run under the sanitizers-tsan CI job (issue #101).
    list(APPEND TEST_SOURCES test_synscan_concurrency_stress.cpp)
endif()
if(TARGET alpacacore_skywatcher)
    list(APPEND TEST_SOURCES test_skywatcher_telescope.cpp
        test_skywatcher_async.cpp)
endif()
if(TARGET alpacacore_ioptron)
    list(APPEND TEST_SOURCES test_ioptron_telescope.cpp test_ioptron_ieaf_focuser.cpp
        test_ioptron_filterwheel.cpp)
    # Same telescope [stress] coverage as SynScan (audit 3.0.1 follow-up).
    list(APPEND TEST_SOURCES test_ioptron_concurrency_stress.cpp)
    # The PowerBox Switch test only builds when its driver was compiled in
    # (i.e. libgpiod >= 2.0 was available).
    if(ALPACACORE_IOPTRON_POWERBOX)
        list(APPEND TEST_SOURCES test_ioptron_switch.cpp)
    endif()
endif()
if(TARGET alpacacore_onstep)
    list(APPEND TEST_SOURCES test_onstep_telescope.cpp)
    # Same telescope [stress] coverage as SynScan/iOptron (audit 3.0.1 follow-up).
    list(APPEND TEST_SOURCES test_onstep_concurrency_stress.cpp)
endif()
if(TARGET alpacacore_weewx)
    list(APPEND TEST_SOURCES test_weewx_observingconditions.cpp)
endif()
if(TARGET alpacacore_gemini)
    list(APPEND TEST_SOURCES test_gemini_focuser.cpp test_gemini_flatpanel.cpp test_gemini_flatpanel_v2.cpp)
endif()
if(TARGET alpacacore_wandererastro)
    list(APPEND TEST_SOURCES test_wandererastro_covercalibrator.cpp test_wandererastro_rotator.cpp
         test_wandererastro_filterwheel.cpp test_wandererastro_box_switch.cpp)
endif()
if(TARGET alpacacore_astroasis)
    list(APPEND TEST_SOURCES test_astroasis_focuser.cpp)
endif()
if(TARGET alpacacore_svbony)
    list(APPEND TEST_SOURCES test_svbony_camera.cpp)
    # Camera lifecycle storm for the issue-#116 close-order/held-mutex fixes;
    # run under the sanitizers-tsan CI job (issue #101).
    list(APPEND TEST_SOURCES test_svbony_concurrency_stress.cpp)
endif()
if(TARGET alpacacore_celestron)
    list(APPEND TEST_SOURCES test_celestron_telescope.cpp)
    # Same telescope [stress] coverage as SynScan (audit 3.0.1 follow-up).
    list(APPEND TEST_SOURCES test_celestron_concurrency_stress.cpp)
endif()
if(TARGET alpacacore_bisque)
    list(APPEND TEST_SOURCES test_bisque_telescope.cpp)
    # Covers the mutex-guarded get_connected() driver shape in the [stress]
    # pool (PR #115 round-2 ABBA regression coverage).
    list(APPEND TEST_SOURCES test_bisque_concurrency_stress.cpp)
endif()
if(TARGET alpacacore_touptek)
    list(APPEND TEST_SOURCES test_touptek_camera.cpp)
    list(APPEND TEST_SOURCES test_touptek_focuser.cpp)
    list(APPEND TEST_SOURCES test_touptek_filterwheel.cpp)
    # The thermal switch (camera dew heater + fan) uses the camera SDK only, so
    # it always builds with the ToupTek target.
    list(APPEND TEST_SOURCES test_touptek_thermal_switch.cpp)
    list(APPEND TEST_SOURCES test_touptek_fake_sdk.cpp)
    # Concurrency stress over the fake SDK seam (issue #101); run under the
    # sanitizers-tsan CI job to turn latent data races into hard failures.
    list(APPEND TEST_SOURCES test_touptek_concurrency_stress.cpp)
    # The StellaVita Switch test only builds when its driver was compiled in
    # (i.e. libgpiod >= 2.0 was available).
    if(ALPACACORE_TOUPTEK_STELLAVITA)
        list(APPEND TEST_SOURCES test_touptek_switch.cpp)
    endif()
endif()
if(TARGET alpacacore_playerone)
    list(APPEND TEST_SOURCES test_playerone_camera.cpp)
    list(APPEND TEST_SOURCES test_playerone_filterwheel.cpp)
    list(APPEND TEST_SOURCES test_playerone_switch.cpp)
    list(APPEND TEST_SOURCES test_playerone_concurrency_stress.cpp)
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
if(TARGET alpacacore_skywatcher)
    target_link_libraries(alpacacore_tests PRIVATE alpacacore_skywatcher)
endif()
if(TARGET alpacacore_ioptron)
    target_link_libraries(alpacacore_tests PRIVATE alpacacore_ioptron)
endif()
if(TARGET alpacacore_onstep)
    target_link_libraries(alpacacore_tests PRIVATE alpacacore_onstep)
endif()
if(TARGET alpacacore_weewx)
    target_link_libraries(alpacacore_tests PRIVATE alpacacore_weewx)
endif()
if(TARGET alpacacore_gemini)
    target_link_libraries(alpacacore_tests PRIVATE alpacacore_gemini)
endif()
if(TARGET alpacacore_wandererastro)
    target_link_libraries(alpacacore_tests PRIVATE alpacacore_wandererastro)
endif()
if(TARGET alpacacore_svbony)
    target_link_libraries(alpacacore_tests PRIVATE alpacacore_svbony)
endif()
if(TARGET alpacacore_celestron)
    target_link_libraries(alpacacore_tests PRIVATE alpacacore_celestron)
endif()
if(TARGET alpacacore_bisque)
    target_link_libraries(alpacacore_tests PRIVATE alpacacore_bisque)
endif()
if(TARGET alpacacore_touptek)
    target_link_libraries(alpacacore_tests PRIVATE alpacacore_touptek)
endif()
if(TARGET alpacacore_playerone)
    target_link_libraries(alpacacore_tests PRIVATE alpacacore_playerone)
endif()
if(TARGET alpacacore_astroasis)
    target_link_libraries(alpacacore_tests PRIVATE alpacacore_astroasis)
endif()

# Both QHY and ZWO SDKs define isLeapYear() at file scope.
if(UNIX AND TARGET alpacacore_qhy AND TARGET alpacacore_zwo)
    target_link_options(alpacacore_tests PRIVATE "LINKER:--allow-multiple-definition")
endif()
include(CTest)
# Include Catch2's test discovery module
include(${Catch2_DIR}/Catch.cmake)
catch_discover_tests(alpacacore_tests)
