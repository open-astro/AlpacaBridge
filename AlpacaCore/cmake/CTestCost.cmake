# Read by ctest, not by CMake: tests/CMakeLists.txt adds it to TEST_INCLUDE_FILES after
# catch_discover_tests. ctest runs a directory's include files in order and
# before that directory's own add_test lines, and COST can only be set on a test
# that already exists; by now the Catch2 cases and AlpacaHTTP's tests (added in
# the parent directories) do.
#
# With CTEST_COST_DATA naming a file that scripts/ctest_cost.py wrote from an
# earlier run (run_all_tests.sh), it sets each test's COST, so a parallel run
# starts the slowest tests first. Unset or missing, it does nothing.
if(DEFINED ENV{CTEST_COST_DATA} AND EXISTS "$ENV{CTEST_COST_DATA}")
  include("$ENV{CTEST_COST_DATA}")
endif()
