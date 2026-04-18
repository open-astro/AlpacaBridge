# ToupTek vendor support
# Uses vendored SDK in external/ToupTek/toupcamsdk.20260128

if(ALPACACORE_ENABLE_TOUPTEK)
    message(STATUS "Building ToupTek camera driver support")

    get_filename_component(ALPACACORE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
    set(TOUPTEK_SDK_ROOT "${ALPACACORE_ROOT}/external/ToupTek/toupcamsdk.20260128")
    set(TOUPTEK_SDK_INCLUDE "${TOUPTEK_SDK_ROOT}/inc")

    if(NOT EXISTS "${TOUPTEK_SDK_INCLUDE}/toupcam.h")
        message(FATAL_ERROR "ToupTek SDK header not found at ${TOUPTEK_SDK_INCLUDE}/toupcam.h")
    endif()

    set(TOUPTEK_LIB_DIR "")

    # Linux only: x64 (NUC) and ARM64 (Raspberry Pi 4/5)
    if(UNIX)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
            set(TOUPTEK_LIB_DIR "${TOUPTEK_SDK_ROOT}/linux/x64")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
            set(TOUPTEK_LIB_DIR "${TOUPTEK_SDK_ROOT}/linux/arm64/glibc")
        else()
            message(FATAL_ERROR "Unsupported Linux architecture for ToupTek SDK: ${CMAKE_SYSTEM_PROCESSOR}. Supported: x86_64, aarch64/arm64.")
        endif()
    else()
        message(FATAL_ERROR "ToupTek driver is supported on Linux only.")
    endif()

    set(TOUPTEK_LIB_PATH "${TOUPTEK_LIB_DIR}/libtoupcam.so")

    if(NOT EXISTS "${TOUPTEK_LIB_PATH}")
        message(FATAL_ERROR "ToupTek SDK shared library not found at ${TOUPTEK_LIB_PATH}")
    endif()

    set(TOUPTEK_SOURCES
        touptek_sdk_wrapper.cpp
        touptek_camera_driver.cpp
    )

    add_library(alpacacore_touptek STATIC ${TOUPTEK_SOURCES})

    target_include_directories(alpacacore_touptek PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${TOUPTEK_SDK_INCLUDE}
    )

    # Link the shared library; RPATH covers both install paths used by the
    # workspace (/usr/local/lib via install_alpaca_service.sh) and the Debian
    # package (/usr/lib/alpacabridge via debian/rules).
    target_link_libraries(alpacacore_touptek PRIVATE ${TOUPTEK_LIB_PATH})
    set_target_properties(alpacacore_touptek PROPERTIES
        BUILD_RPATH   "${TOUPTEK_LIB_DIR}"
        INSTALL_RPATH "/usr/local/lib;/usr/lib/alpacabridge"
    )

    # libtoupcam.so is self-contained (ldd confirms only libc/libm/libpthread/
    # librt/libdl) so there is no libusb / libudev link dependency here.
    find_package(Threads REQUIRED)
    target_link_libraries(alpacacore_touptek PRIVATE Threads::Threads)
endif()
