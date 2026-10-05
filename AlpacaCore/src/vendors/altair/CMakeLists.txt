# Altair Astro vendor support
# Uses vendored SDK in external/Altair/altaircamsdk.20260531
#
# Altair cameras are ToupTek OEM hardware and altaircam is the ToupTek SDK
# renamed, so this target holds only the Altair SDK wrapper (the shared
# ToupcamFamilySDK over Altaircam_* calls) and the factory; the camera driver
# itself is the ToupTek one, linked from alpacacore_touptek.

if(ALPACACORE_ENABLE_ALTAIR)
    message(STATUS "Building Altair camera driver support")

    if(NOT TARGET alpacacore_touptek)
        message(FATAL_ERROR
            "ALPACACORE_ENABLE_ALTAIR requires ALPACACORE_ENABLE_TOUPTEK=ON: the Altair "
            "camera runs on the ToupTek camera driver.")
    endif()

    get_filename_component(ALPACACORE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
    set(ALTAIR_SDK_ROOT "${ALPACACORE_ROOT}/external/Altair/altaircamsdk.20260531")
    set(ALTAIR_SDK_INCLUDE "${ALTAIR_SDK_ROOT}/inc")

    if(NOT EXISTS "${ALTAIR_SDK_INCLUDE}/altaircam.h")
        message(FATAL_ERROR "Altair SDK header not found at ${ALTAIR_SDK_INCLUDE}/altaircam.h")
    endif()

    # Linux arm64 only (Raspberry Pi 4/5, Rockchip SBCs, OrangePi, iOptron iMate)
    if(NOT UNIX)
        message(FATAL_ERROR "Altair driver is supported on Linux only.")
    endif()
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        message(FATAL_ERROR "Unsupported architecture for Altair SDK: ${CMAKE_SYSTEM_PROCESSOR}. AlpacaBridge is arm64-only.")
    endif()
    set(ALTAIR_LIB_DIR "${ALTAIR_SDK_ROOT}/linux/arm64/glibc")
    set(ALTAIR_LIB_PATH "${ALTAIR_LIB_DIR}/libaltaircam.so")

    if(NOT EXISTS "${ALTAIR_LIB_PATH}")
        message(FATAL_ERROR "Altair SDK shared library not found at ${ALTAIR_LIB_PATH}")
    endif()

    add_library(alpacacore_altair STATIC
        altair_sdk_wrapper.cpp
        altair_camera_driver.cpp
        altair_catalog.cpp
    )

    target_include_directories(alpacacore_altair PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${ALTAIR_SDK_INCLUDE}
    )

    # Base drivers and logging live in alpacacore, the camera driver in
    # alpacacore_touptek; this vendor links down to both, never the reverse
    # (open-astro#710).
    target_link_libraries(alpacacore_altair PRIVATE alpacacore alpacacore_touptek)

    # Link the shared library; RPATH covers both install paths used by the
    # workspace (/usr/local/lib via install_alpaca_service.sh) and the Debian
    # package (/usr/lib/alpacabridge via debian/rules).
    target_link_libraries(alpacacore_altair PRIVATE ${ALTAIR_LIB_PATH})
    set_target_properties(alpacacore_altair PROPERTIES
        BUILD_RPATH   "${ALTAIR_LIB_DIR}"
        INSTALL_RPATH "/usr/local/lib;/usr/lib/alpacabridge"
    )

    # libaltaircam.so is self-contained (readelf: only libc/libm/libpthread/
    # libdl), so there is no libusb / libudev link dependency here.
    find_package(Threads REQUIRED)
    target_link_libraries(alpacacore_altair PRIVATE Threads::Threads)
endif()
