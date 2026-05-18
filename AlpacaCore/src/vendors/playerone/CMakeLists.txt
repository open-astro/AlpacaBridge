# Player One vendor support
# Uses vendored SDK in external/PlayerOne/PlayerOne_Camera_SDK_Linux_V3.10.0

if(ALPACACORE_ENABLE_PLAYERONE)
    message(STATUS "Building Player One camera driver support")

    get_filename_component(ALPACACORE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
    set(PLAYERONE_SDK_ROOT "${ALPACACORE_ROOT}/external/PlayerOne/PlayerOne_Camera_SDK_Linux_V3.10.0")
    set(PLAYERONE_SDK_INCLUDE "${PLAYERONE_SDK_ROOT}/include")

    if(NOT EXISTS "${PLAYERONE_SDK_INCLUDE}/PlayerOneCamera.h")
        message(FATAL_ERROR "Player One SDK header not found at ${PLAYERONE_SDK_INCLUDE}/PlayerOneCamera.h")
    endif()

    # Linux arm64 only (Raspberry Pi 4/5, Rockchip SBCs, OrangePi, iOptron iMate)
    if(NOT UNIX)
        message(FATAL_ERROR "Player One driver is supported on Linux only.")
    endif()
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        message(FATAL_ERROR "Unsupported architecture for Player One SDK: ${CMAKE_SYSTEM_PROCESSOR}. AlpacaBridge is arm64-only.")
    endif()
    set(PLAYERONE_LIB_DIR "${PLAYERONE_SDK_ROOT}/lib/arm64")

    set(PLAYERONE_LIB_PATH "${PLAYERONE_LIB_DIR}/libPlayerOneCamera.so")

    if(NOT EXISTS "${PLAYERONE_LIB_PATH}")
        message(FATAL_ERROR "Player One SDK shared library not found at ${PLAYERONE_LIB_PATH}")
    endif()

    set(PLAYERONE_SOURCES
        playerone_sdk_wrapper.cpp
        playerone_camera_driver.cpp
    )

    add_library(alpacacore_playerone STATIC ${PLAYERONE_SOURCES})

    target_include_directories(alpacacore_playerone PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${PLAYERONE_SDK_INCLUDE}
    )

    # Link the shared library; RPATH covers both install paths used by the
    # workspace (/usr/local/lib via install_alpaca_service.sh) and the Debian
    # package (/usr/lib/alpacabridge via debian/rules).
    target_link_libraries(alpacacore_playerone PRIVATE ${PLAYERONE_LIB_PATH})
    set_target_properties(alpacacore_playerone PROPERTIES
        BUILD_RPATH   "${PLAYERONE_LIB_DIR}"
        INSTALL_RPATH "/usr/local/lib;/usr/lib/alpacabridge"
    )

    find_package(Threads REQUIRED)
    target_link_libraries(alpacacore_playerone PRIVATE Threads::Threads)
endif()
