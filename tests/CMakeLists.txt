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
    test_camera.cpp
    test_telescope.cpp
    test_management.cpp
    test_units.cpp
)

# Create test executable
if(TEST_FRAMEWORK STREQUAL "catch2")
    add_executable(alpacacore_tests ${TEST_SOURCES})
    target_link_libraries(alpacacore_tests PRIVATE alpacacore Catch2::Catch2)
    include(CTest)
    include(Catch)
    catch_discover_tests(alpacacore_tests)
elseif(TEST_FRAMEWORK STREQUAL "doctest")
    add_executable(alpacacore_tests ${TEST_SOURCES})
    target_link_libraries(alpacacore_tests PRIVATE alpacacore doctest::doctest)
    include(CTest)
    add_test(NAME AlpacaCoreTests COMMAND alpacacore_tests)
endif()

