# QHY vendor support

if(ALPACACORE_ENABLE_QHY)
    message(STATUS "Building QHY camera driver support")

    get_filename_component(ALPACACORE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

    # Select SDK root based on target architecture
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(QHY_SDK_ROOT "${ALPACACORE_ROOT}/external/QHY/sdk_Arm64_25.09.29")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
        set(QHY_SDK_ROOT "${ALPACACORE_ROOT}/external/QHY/sdk_linux64_25.09.29")
    else()
        message(FATAL_ERROR "Unsupported architecture for QHY SDK: ${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    set(QHY_SDK_INCLUDE "${QHY_SDK_ROOT}/usr/local/include")
    set(QHY_LIB_PATH    "${QHY_SDK_ROOT}/usr/local/lib/libqhyccd.a")

    if(NOT EXISTS "${QHY_SDK_INCLUDE}/qhyccd.h")
        message(FATAL_ERROR "QHY SDK header not found at ${QHY_SDK_INCLUDE}/qhyccd.h")
    endif()

    if(NOT EXISTS "${QHY_LIB_PATH}")
        message(FATAL_ERROR "QHY SDK library not found at ${QHY_LIB_PATH}")
    endif()

    set(QHY_SOURCES
        qhy_sdk_wrapper.cpp
        qhy_camera_driver.cpp
    )

    add_library(alpacacore_qhy STATIC ${QHY_SOURCES})

    target_include_directories(alpacacore_qhy PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${QHY_SDK_INCLUDE}
    )

    target_link_libraries(alpacacore_qhy PRIVATE ${QHY_LIB_PATH})

    if(UNIX)
        find_package(PkgConfig)
        if(PkgConfig_FOUND)
            pkg_check_modules(LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
            pkg_check_modules(LIBUDEV QUIET IMPORTED_TARGET libudev)
        endif()

        if(LIBUSB_FOUND)
            target_include_directories(alpacacore_qhy PRIVATE ${LIBUSB_INCLUDE_DIRS})
            target_compile_options(alpacacore_qhy PRIVATE ${LIBUSB_CFLAGS_OTHER})
            if(TARGET PkgConfig::LIBUSB)
                target_link_libraries(alpacacore_qhy PRIVATE PkgConfig::LIBUSB)
            else()
                target_link_libraries(alpacacore_qhy PRIVATE ${LIBUSB_LIBRARIES})
            endif()
        else()
            find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0)
            if(LIBUSB_LIBRARY)
                target_link_libraries(alpacacore_qhy PRIVATE ${LIBUSB_LIBRARY})
            else()
                message(FATAL_ERROR "libusb-1.0 not found; install libusb-1.0-0-dev.")
            endif()
        endif()

        if(NOT APPLE)
            if(LIBUDEV_FOUND)
                target_include_directories(alpacacore_qhy PRIVATE ${LIBUDEV_INCLUDE_DIRS})
                target_compile_options(alpacacore_qhy PRIVATE ${LIBUDEV_CFLAGS_OTHER})
                if(TARGET PkgConfig::LIBUDEV)
                    target_link_libraries(alpacacore_qhy PRIVATE PkgConfig::LIBUDEV)
                else()
                    target_link_libraries(alpacacore_qhy PRIVATE ${LIBUDEV_LIBRARIES})
                endif()
            else()
                find_library(LIBUDEV_LIBRARY NAMES udev libudev)
                if(LIBUDEV_LIBRARY)
                    target_link_libraries(alpacacore_qhy PRIVATE ${LIBUDEV_LIBRARY})
                else()
                    message(FATAL_ERROR "libudev not found; install libudev-dev.")
                endif()
            endif()

            find_package(Threads REQUIRED)
            target_link_libraries(alpacacore_qhy PRIVATE Threads::Threads rt)
        endif()
    endif()

endif()
