cmake_minimum_required(VERSION 3.20)

set(WANDERERASTRO_SOURCES
    wandererastro_protocol_wrapper.cpp
    wandererastro_covercalibrator_driver.cpp
    wandererastro_rotator_protocol_wrapper.cpp
    wandererastro_rotator_driver.cpp
    wandererastro_filterwheel_protocol_wrapper.cpp
    wandererastro_filterwheel_driver.cpp
)

add_library(alpacacore_wandererastro STATIC ${WANDERERASTRO_SOURCES})

target_include_directories(alpacacore_wandererastro PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# WandererCover uses an ASCII serial protocol over a CH340 USB-serial link.
# No external SDK is required — only standard POSIX/Win32 APIs and a reader thread.
find_package(Threads REQUIRED)
target_link_libraries(alpacacore_wandererastro PRIVATE Threads::Threads)
