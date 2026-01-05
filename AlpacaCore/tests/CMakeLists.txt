cmake_minimum_required(VERSION 3.20)

# Find testing framework (Catch2 or doctest)
find_package(Catch2 QUIET)
if(NOT Catch2_FOUND)
    # Try doctest as fallback
    find_package(doctest QUIET)
    if(doctest_FOUND)
        set(TEST_FRAMEWORK "doctest")
    else()
        message(WARNING "No test framework found. Install Catch2 or doctest to build tests.")
        return()
    endif()
else()
    set(TEST_FRAMEWORK "catch2")
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
endif()

# Create test executable
if(TEST_FRAMEWORK STREQUAL "catch2")
    add_executable(alpacacore_tests ${TEST_SOURCES})
    target_link_libraries(alpacacore_tests PRIVATE alpacacore Catch2::Catch2WithMain)
    include(CTest)
    # Include Catch2's test discovery module
    include(${Catch2_DIR}/Catch.cmake)
    catch_discover_tests(alpacacore_tests)
elseif(TEST_FRAMEWORK STREQUAL "doctest")
    add_executable(alpacacore_tests ${TEST_SOURCES})
    target_link_libraries(alpacacore_tests PRIVATE alpacacore doctest::doctest)
    include(CTest)
    add_test(NAME AlpacaCoreTests COMMAND alpacacore_tests)
endif()
