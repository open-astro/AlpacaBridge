cmake_minimum_required(VERSION 3.16)

project(test_base)

set(CMAKE_CXX_STANDARD 14)

if(WIN32)
  set(OpenCV_DIR "D:/opencv3.4/download/opencv/build")
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR})
find_package(LibQHYCCD REQUIRED)
find_package(OpenCV REQUIRED)

message("inc path: " ${LIBQHYCCD_INCLUDE_DIR})
message("lib path: " ${LIBQHYCCD_LIBRARY})
message("lib found: " ${LIBQHYCCD_FOUND})

 

add_executable(test main.cpp)



include_directories(${LIBQHYCCD_INCLUDE_DIR})
target_link_libraries(test ${LIBQHYCCD_LIBRARY} ${OpenCV_LIBS})

