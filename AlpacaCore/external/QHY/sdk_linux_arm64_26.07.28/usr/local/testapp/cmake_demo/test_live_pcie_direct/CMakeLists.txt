cmake_minimum_required(VERSION 3.16)

project(test_live_pcie_direct)

set(CMAKE_CXX_STANDARD 14)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../cmake)
find_package(LibQHYCCD REQUIRED)
find_package(X11 REQUIRED)

message("inc path: " ${LIBQHYCCD_INCLUDE_DIR})
message("lib path: " ${LIBQHYCCD_LIBRARY})
message("lib found: " ${LIBQHYCCD_FOUND})
message("X11 inc: " ${X11_INCLUDE_DIR})
message("X11 lib: " ${X11_LIBRARIES})

add_executable(test_live_pcie_direct LivePcieDirectThumbnail.cpp)

include_directories(${LIBQHYCCD_INCLUDE_DIR} ${X11_INCLUDE_DIR})
target_link_libraries(test_live_pcie_direct ${LIBQHYCCD_LIBRARY} ${X11_LIBRARIES})
