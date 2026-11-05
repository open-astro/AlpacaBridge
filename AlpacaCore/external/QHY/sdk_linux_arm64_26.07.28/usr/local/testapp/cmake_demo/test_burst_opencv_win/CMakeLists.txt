cmake_minimum_required(VERSION 3.16)

project(test_base_burst)

set(CMAKE_CXX_STANDARD 14)

add_executable(test_burst main.cpp)
add_executable(test_single main_single.cpp)

# No find_package(), no external CMake modules.
# Use SDKs bundled in this repository:
# - QHYCCD SDK is under ./qhyccd
# - OpenCV must be bundled under ./opencv/build (self-contained; no fallback to external paths)
set(QHYCCD_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/qhyccd/include")
set(QHYCCD_LIB "${CMAKE_CURRENT_SOURCE_DIR}/qhyccd/lib/qhyccd.lib")

# OpenCV:
# NOTE: OpenCV must provide BOTH:
# - headers:  <root>/include/opencv2/core.hpp (and imgproc/imgcodecs as used by this project)
# - libs/dlls: <root>/x64/vc15/(lib|bin)
set(OPENCV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/opencv/build")
if(NOT EXISTS "${OPENCV_ROOT}/include/opencv2/core.hpp")
  message(FATAL_ERROR
    "Bundled OpenCV not found. Expected: ${OPENCV_ROOT}/include/opencv2/core.hpp\n"
    "Please bundle OpenCV into ./opencv/build (include + x64/vc15).")
endif()
set(OPENCV_INCLUDE_DIR "${OPENCV_ROOT}/include")

# Require VS2019 toolset (vc15).
set(OPENCV_LIB_DIR "")
set(OPENCV_BIN_DIR "")
if(EXISTS "${OPENCV_ROOT}/x64/vc15/lib/opencv_world3416.lib")
  set(OPENCV_LIB_DIR "${OPENCV_ROOT}/x64/vc15/lib")
  set(OPENCV_BIN_DIR "${OPENCV_ROOT}/x64/vc15/bin")
else()
  message(FATAL_ERROR "OpenCV prebuilt vc15 libs not found under ${OPENCV_ROOT}/x64/vc15/lib")
endif()

set(OPENCV_WORLD_LIB "${OPENCV_LIB_DIR}/opencv_world3416.lib")

target_include_directories(test_burst PRIVATE
  "${QHYCCD_INCLUDE_DIR}"
  "${OPENCV_INCLUDE_DIR}"
)
target_link_libraries(test_burst PRIVATE
  "${QHYCCD_LIB}"
  "${OPENCV_WORLD_LIB}"
)

target_include_directories(test_single PRIVATE
  "${QHYCCD_INCLUDE_DIR}"
  "${OPENCV_INCLUDE_DIR}"
)
target_link_libraries(test_single PRIVATE
  "${QHYCCD_LIB}"
  "${OPENCV_WORLD_LIB}"
)

if(WIN32)
  # Make the built exe runnable without manually tweaking PATH.
  add_custom_command(TARGET test_burst POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/qhyccd/qhyccd.dll"
      "$<TARGET_FILE_DIR:test_burst>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${OPENCV_BIN_DIR}/opencv_world3416.dll"
      "$<TARGET_FILE_DIR:test_burst>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${OPENCV_BIN_DIR}/opencv_ffmpeg3416_64.dll"
      "$<TARGET_FILE_DIR:test_burst>"
  )

  add_custom_command(TARGET test_single POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/qhyccd/qhyccd.dll"
      "$<TARGET_FILE_DIR:test_single>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${OPENCV_BIN_DIR}/opencv_world3416.dll"
      "$<TARGET_FILE_DIR:test_single>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${OPENCV_BIN_DIR}/opencv_ffmpeg3416_64.dll"
      "$<TARGET_FILE_DIR:test_single>"
  )
endif()