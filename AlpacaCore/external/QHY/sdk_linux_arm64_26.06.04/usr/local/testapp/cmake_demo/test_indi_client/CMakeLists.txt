cmake_minimum_required(VERSION 3.12)
project(test_indi_focus)

LIST(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake_modules/")


set(CMAKE_CXX_STANDARD 14)
find_package(INDI REQUIRED)

include_directories( ${INDI_INCLUDE_DIR})

add_executable(test_indi_focus main.cpp)

target_link_libraries(test_indi_focus indiclient)
