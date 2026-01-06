# ZWO vendor support
# Uses vendored SDK subset in external/ASI_Camera_SDK

if(ALPACACORE_ENABLE_ZWO)
    message(STATUS "Building ZWO camera driver support")

    get_filename_component(ALPACACORE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
    set(ZWO_SDK_ROOT "${ALPACACORE_ROOT}/external/ASI_Camera_SDK")
    set(ZWO_SDK_INCLUDE "${ZWO_SDK_ROOT}/include")

    if(NOT EXISTS "${ZWO_SDK_INCLUDE}/ASICamera2.h")
        message(FATAL_ERROR "ZWO SDK header not found at ${ZWO_SDK_INCLUDE}/ASICamera2.h")
    endif()

    set(ZWO_LIB_PATH "")
    set(ZWO_RUNTIME_DLL "")

    if(WIN32)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(ZWO_LIB_DIR "${ZWO_SDK_ROOT}/lib/windows/x64")
        else()
            set(ZWO_LIB_DIR "${ZWO_SDK_ROOT}/lib/windows/x86")
        endif()
        set(ZWO_LIB_PATH "${ZWO_LIB_DIR}/ASICamera2.lib")
        set(ZWO_RUNTIME_DLL "${ZWO_LIB_DIR}/ASICamera2.dll")
    elseif(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            set(ZWO_LIB_PATH "${ZWO_SDK_ROOT}/lib/mac/arm64/libASICamera2.a")
        else()
            set(ZWO_LIB_PATH "${ZWO_SDK_ROOT}/lib/mac/x64/libASICamera2.a")
        endif()
    elseif(UNIX)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
            set(ZWO_LIB_PATH "${ZWO_SDK_ROOT}/lib/linux/x64/libASICamera2.a")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "i386|i686|x86")
            set(ZWO_LIB_PATH "${ZWO_SDK_ROOT}/lib/linux/x86/libASICamera2.a")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
            set(ZWO_LIB_PATH "${ZWO_SDK_ROOT}/lib/linux/armv8/libASICamera2.a")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "armv7|armv7l")
            set(ZWO_LIB_PATH "${ZWO_SDK_ROOT}/lib/linux/armv7/libASICamera2.a")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "armv6|armv6l")
            set(ZWO_LIB_PATH "${ZWO_SDK_ROOT}/lib/linux/armv6/libASICamera2.a")
        else()
            message(FATAL_ERROR "Unsupported Linux architecture: ${CMAKE_SYSTEM_PROCESSOR}")
        endif()
    endif()

    if(NOT EXISTS "${ZWO_LIB_PATH}")
        message(FATAL_ERROR "ZWO SDK library not found at ${ZWO_LIB_PATH}")
    endif()

    set(ZWO_SOURCES
        zwo_sdk_wrapper.cpp
        zwo_camera_driver.cpp
        zwo_switch_driver.cpp
    )

    add_library(alpacacore_zwo STATIC ${ZWO_SOURCES})

    target_include_directories(alpacacore_zwo PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${ZWO_SDK_INCLUDE}
    )

    target_link_libraries(alpacacore_zwo PRIVATE ${ZWO_LIB_PATH})

    if(UNIX)
        find_package(PkgConfig)
        if(PkgConfig_FOUND)
            pkg_check_modules(LIBUSB libusb-1.0)
        endif()

        if(LIBUSB_FOUND)
            target_include_directories(alpacacore_zwo PRIVATE ${LIBUSB_INCLUDE_DIRS})
            target_compile_options(alpacacore_zwo PRIVATE ${LIBUSB_CFLAGS_OTHER})
            target_link_libraries(alpacacore_zwo PRIVATE ${LIBUSB_LIBRARIES})
        else()
            find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0)
            if(LIBUSB_LIBRARY)
                target_link_libraries(alpacacore_zwo PRIVATE ${LIBUSB_LIBRARY})
            else()
                message(FATAL_ERROR "libusb-1.0 not found; install it and ensure pkg-config can locate it (e.g. macOS: `brew install libusb`, Linux: `libusb-1.0-0-dev`).")
            endif()
        endif()

        if(NOT APPLE)
            find_package(Threads REQUIRED)
            target_link_libraries(alpacacore_zwo PRIVATE Threads::Threads rt)
        endif()
    endif()

    if(WIN32 AND EXISTS "${ZWO_RUNTIME_DLL}")
        set(ZWO_RUNTIME_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
        if(NOT ZWO_RUNTIME_OUTPUT_DIR)
            set(ZWO_RUNTIME_OUTPUT_DIR "${CMAKE_BINARY_DIR}")
        endif()

        add_custom_command(TARGET alpacacore_zwo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ZWO_RUNTIME_DLL}"
                "${ZWO_RUNTIME_OUTPUT_DIR}"
        )

        install(FILES "${ZWO_RUNTIME_DLL}" DESTINATION bin)
    endif()
endif()
