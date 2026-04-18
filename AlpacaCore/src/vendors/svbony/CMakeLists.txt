# SVBONY vendor support
# Uses vendored SDK in external/SVBONY

if(ALPACACORE_ENABLE_SVBONY)
    message(STATUS "Building SVBONY camera driver support")

    get_filename_component(ALPACACORE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
    set(SVB_SDK_ROOT "${ALPACACORE_ROOT}/external/SVBONY")
    set(SVB_SDK_INCLUDE "${SVB_SDK_ROOT}/include")

    if(NOT EXISTS "${SVB_SDK_INCLUDE}/SVBCameraSDK.h")
        message(FATAL_ERROR "SVBONY SDK header not found at ${SVB_SDK_INCLUDE}/SVBCameraSDK.h")
    endif()

    set(SVB_LIB_PATH "")

    # Linux only: x64 (NUC) and ARM64 (Raspberry Pi 4/5)
    if(UNIX)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
            set(SVB_LIB_PATH "${SVB_SDK_ROOT}/lib/x64/libSVBCameraSDK.a")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
            set(SVB_LIB_PATH "${SVB_SDK_ROOT}/lib/armv8/libSVBCameraSDK.a")
        else()
            message(FATAL_ERROR "Unsupported Linux architecture: ${CMAKE_SYSTEM_PROCESSOR}. Supported: x86_64, aarch64/arm64.")
        endif()
    else()
        message(FATAL_ERROR "SVBONY driver is supported on Linux only.")
    endif()

    if(NOT EXISTS "${SVB_LIB_PATH}")
        message(FATAL_ERROR "SVBONY SDK library not found at ${SVB_LIB_PATH}")
    endif()

    set(SVBONY_SOURCES
        svbony_sdk_wrapper.cpp
        svbony_camera_driver.cpp
    )

    add_library(alpacacore_svbony STATIC ${SVBONY_SOURCES})

    target_include_directories(alpacacore_svbony PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${SVB_SDK_INCLUDE}
    )

    target_link_libraries(alpacacore_svbony PRIVATE ${SVB_LIB_PATH})

    if(UNIX)
        find_package(PkgConfig)
        if(PkgConfig_FOUND)
            pkg_check_modules(LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
            pkg_check_modules(LIBUDEV QUIET IMPORTED_TARGET libudev)
        endif()

        if(LIBUSB_FOUND)
            target_include_directories(alpacacore_svbony PRIVATE ${LIBUSB_INCLUDE_DIRS})
            target_compile_options(alpacacore_svbony PRIVATE ${LIBUSB_CFLAGS_OTHER})
            if(TARGET PkgConfig::LIBUSB)
                target_link_libraries(alpacacore_svbony PRIVATE PkgConfig::LIBUSB)
            else()
                target_link_libraries(alpacacore_svbony PRIVATE ${LIBUSB_LIBRARIES})
            endif()
        else()
            find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0)
            if(LIBUSB_LIBRARY)
                target_link_libraries(alpacacore_svbony PRIVATE ${LIBUSB_LIBRARY})
            else()
                message(FATAL_ERROR "libusb-1.0 not found; install it (e.g. apt install libusb-1.0-0-dev).")
            endif()
        endif()

        if(LIBUDEV_FOUND)
            target_include_directories(alpacacore_svbony PRIVATE ${LIBUDEV_INCLUDE_DIRS})
            target_compile_options(alpacacore_svbony PRIVATE ${LIBUDEV_CFLAGS_OTHER})
            if(TARGET PkgConfig::LIBUDEV)
                target_link_libraries(alpacacore_svbony PRIVATE PkgConfig::LIBUDEV)
            else()
                target_link_libraries(alpacacore_svbony PRIVATE ${LIBUDEV_LIBRARIES})
            endif()
        else()
            find_library(LIBUDEV_LIBRARY NAMES udev libudev)
            if(LIBUDEV_LIBRARY)
                target_link_libraries(alpacacore_svbony PRIVATE ${LIBUDEV_LIBRARY})
            else()
                message(FATAL_ERROR "libudev not found; install libudev-dev.")
            endif()
        endif()

        find_package(Threads REQUIRED)
        target_link_libraries(alpacacore_svbony PRIVATE Threads::Threads rt)
    endif()
endif()
