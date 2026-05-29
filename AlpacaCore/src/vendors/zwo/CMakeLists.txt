# ZWO vendor support
# Uses vendored SDK subset in external/ZWO/ASI_Camera_SDK

if(ALPACACORE_ENABLE_ZWO)
    message(STATUS "Building ZWO camera driver support")

    get_filename_component(ALPACACORE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
    set(ZWO_SDK_ROOT "${ALPACACORE_ROOT}/external/ZWO/ASI_Camera_SDK")
    set(ZWO_SDK_INCLUDE "${ZWO_SDK_ROOT}/include")
    set(ZWO_EAF_ROOT "${ALPACACORE_ROOT}/external/ZWO/EAF")
    set(ZWO_EAF_INCLUDE "${ZWO_EAF_ROOT}/include")
    set(ZWO_EFW_ROOT "${ALPACACORE_ROOT}/external/ZWO/EFW")
    set(ZWO_EFW_INCLUDE "${ZWO_EFW_ROOT}/include")
    set(ZWO_CAA_ROOT "${ALPACACORE_ROOT}/external/ZWO/CAA")
    set(ZWO_CAA_INCLUDE "${ZWO_CAA_ROOT}/include")

    if(NOT EXISTS "${ZWO_SDK_INCLUDE}/ASICamera2.h")
        message(FATAL_ERROR "ZWO SDK header not found at ${ZWO_SDK_INCLUDE}/ASICamera2.h")
    endif()
    if(NOT EXISTS "${ZWO_EAF_INCLUDE}/EAF_focuser.h")
        message(FATAL_ERROR "ZWO EAF SDK header not found at ${ZWO_EAF_INCLUDE}/EAF_focuser.h")
    endif()
    if(NOT EXISTS "${ZWO_EFW_INCLUDE}/EFW_filter.h")
        message(FATAL_ERROR "ZWO EFW SDK header not found at ${ZWO_EFW_INCLUDE}/EFW_filter.h")
    endif()
    if(NOT EXISTS "${ZWO_CAA_INCLUDE}/CAA_API.h")
        message(FATAL_ERROR "ZWO CAA SDK header not found at ${ZWO_CAA_INCLUDE}/CAA_API.h")
    endif()

    # Linux arm64 only (Raspberry Pi 4/5, Rockchip SBCs, OrangePi, iOptron iMate)
    if(NOT UNIX)
        message(FATAL_ERROR "ZWO driver is supported on Linux only.")
    endif()
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        message(FATAL_ERROR "Unsupported architecture: ${CMAKE_SYSTEM_PROCESSOR}. AlpacaBridge is arm64-only.")
    endif()
    set(ZWO_LIB_PATH "${ZWO_SDK_ROOT}/lib/linux/armv8/libASICamera2.a")
    set(ZWO_EAF_LIB_PATH "${ZWO_EAF_ROOT}/lib/armv8/libEAFFocuser.a")
    set(ZWO_EFW_LIB_PATH "${ZWO_EFW_ROOT}/lib/armv8/libEFWFilter.a")
    set(ZWO_CAA_LIB_PATH "${ZWO_CAA_ROOT}/lib/armv8/libCAA.a")

    if(NOT EXISTS "${ZWO_LIB_PATH}")
        message(FATAL_ERROR "ZWO SDK library not found at ${ZWO_LIB_PATH}")
    endif()
    if(NOT EXISTS "${ZWO_EAF_LIB_PATH}")
        message(FATAL_ERROR "ZWO EAF SDK library not found at ${ZWO_EAF_LIB_PATH}")
    endif()
    if(NOT EXISTS "${ZWO_EFW_LIB_PATH}")
        message(FATAL_ERROR "ZWO EFW SDK library not found at ${ZWO_EFW_LIB_PATH}")
    endif()
    if(NOT EXISTS "${ZWO_CAA_LIB_PATH}")
        message(FATAL_ERROR "ZWO CAA SDK library not found at ${ZWO_CAA_LIB_PATH}")
    endif()

    set(ZWO_SOURCES
        zwo_sdk_wrapper.cpp
        zwo_camera_driver.cpp
        zwo_switch_driver.cpp
        zwo_asiair_protocol_wrapper.cpp
        zwo_asiair_switch_driver.cpp
        zwo_asiair_plus_protocol_wrapper.cpp
        zwo_asiair_plus_switch_driver.cpp
        zwo_eaf_wrapper.cpp
        zwo_focuser_driver.cpp
        zwo_efw_wrapper.cpp
        zwo_filterwheel_driver.cpp
        zwo_caa_wrapper.cpp
        zwo_rotator_driver.cpp
        zwo_mount_protocol_wrapper.cpp
        zwo_telescope_driver.cpp
    )

    add_library(alpacacore_zwo STATIC ${ZWO_SOURCES})

    target_include_directories(alpacacore_zwo PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${ZWO_SDK_INCLUDE}
        ${ZWO_EAF_INCLUDE}
        ${ZWO_EFW_INCLUDE}
        ${ZWO_CAA_INCLUDE}
        ${ALPACACORE_ROOT}/external/ZWO/asiair-plus
    )

    target_link_libraries(alpacacore_zwo PRIVATE ${ZWO_LIB_PATH} ${ZWO_EAF_LIB_PATH} ${ZWO_EFW_LIB_PATH} ${ZWO_CAA_LIB_PATH})

    if(UNIX)
        find_package(PkgConfig)
        if(PkgConfig_FOUND)
            pkg_check_modules(LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
            pkg_check_modules(LIBUDEV QUIET IMPORTED_TARGET libudev)
            pkg_check_modules(LIBGPIOD QUIET IMPORTED_TARGET libgpiod>=2.0)
        endif()

        if(LIBGPIOD_FOUND)
            target_include_directories(alpacacore_zwo PRIVATE ${LIBGPIOD_INCLUDE_DIRS})
            target_compile_options(alpacacore_zwo PRIVATE ${LIBGPIOD_CFLAGS_OTHER})
            if(TARGET PkgConfig::LIBGPIOD)
                target_link_libraries(alpacacore_zwo PRIVATE PkgConfig::LIBGPIOD)
            else()
                target_link_libraries(alpacacore_zwo PRIVATE ${LIBGPIOD_LIBRARIES})
            endif()
        else()
            find_library(LIBGPIOD_LIBRARY NAMES gpiod libgpiod)
            find_path(LIBGPIOD_INCLUDE_DIR NAMES gpiod.h)
            if(LIBGPIOD_LIBRARY AND LIBGPIOD_INCLUDE_DIR)
                # The pkg-config branch above enforces libgpiod >= 2.0 via
                # pkg_check_modules. The find_library fallback has no such
                # version metadata, so probe for a v2-only symbol
                # (gpiod_chip_request_lines) before accepting the library.
                # v1 headers don't declare it; linking against v1 would
                # otherwise fail with cryptic compiler/linker errors later
                # in the build.
                include(CheckSymbolExists)
                set(CMAKE_REQUIRED_INCLUDES "${LIBGPIOD_INCLUDE_DIR}")
                set(CMAKE_REQUIRED_LIBRARIES "${LIBGPIOD_LIBRARY}")
                check_symbol_exists(gpiod_chip_request_lines "gpiod.h" LIBGPIOD_V2_OK)
                unset(CMAKE_REQUIRED_INCLUDES)
                unset(CMAKE_REQUIRED_LIBRARIES)
                if(NOT LIBGPIOD_V2_OK)
                    message(FATAL_ERROR
                        "libgpiod found at ${LIBGPIOD_LIBRARY} but appears to be < 2.0 "
                        "(missing gpiod_chip_request_lines). Install libgpiod-dev (>= 2.0) "
                        "— e.g. apt install libgpiod-dev on Debian Trixie. On older "
                        "Bullseye/Bookworm hosts you may need backports.")
                endif()
                target_include_directories(alpacacore_zwo PRIVATE ${LIBGPIOD_INCLUDE_DIR})
                target_link_libraries(alpacacore_zwo PRIVATE ${LIBGPIOD_LIBRARY})
            else()
                message(FATAL_ERROR "libgpiod (>=2.0) not found; install it (e.g. apt install libgpiod-dev).")
            endif()
        endif()

        if(LIBUSB_FOUND)
            target_include_directories(alpacacore_zwo PRIVATE ${LIBUSB_INCLUDE_DIRS})
            target_compile_options(alpacacore_zwo PRIVATE ${LIBUSB_CFLAGS_OTHER})
            if(TARGET PkgConfig::LIBUSB)
                target_link_libraries(alpacacore_zwo PRIVATE PkgConfig::LIBUSB)
            else()
                target_link_libraries(alpacacore_zwo PRIVATE ${LIBUSB_LIBRARIES})
            endif()
        else()
            find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0)
            if(LIBUSB_LIBRARY)
                target_link_libraries(alpacacore_zwo PRIVATE ${LIBUSB_LIBRARY})
            else()
                message(FATAL_ERROR "libusb-1.0 not found; install it (e.g. apt install libusb-1.0-0-dev).")
            endif()
        endif()

        if(LIBUDEV_FOUND)
            target_include_directories(alpacacore_zwo PRIVATE ${LIBUDEV_INCLUDE_DIRS})
            target_compile_options(alpacacore_zwo PRIVATE ${LIBUDEV_CFLAGS_OTHER})
            if(TARGET PkgConfig::LIBUDEV)
                target_link_libraries(alpacacore_zwo PRIVATE PkgConfig::LIBUDEV)
            else()
                target_link_libraries(alpacacore_zwo PRIVATE ${LIBUDEV_LIBRARIES})
            endif()
        else()
            find_library(LIBUDEV_LIBRARY NAMES udev libudev)
            if(LIBUDEV_LIBRARY)
                target_link_libraries(alpacacore_zwo PRIVATE ${LIBUDEV_LIBRARY})
            else()
                message(FATAL_ERROR "libudev not found; install libudev-dev.")
            endif()
        endif()

        find_package(Threads REQUIRED)
        target_link_libraries(alpacacore_zwo PRIVATE Threads::Threads rt)
    endif()
endif()
