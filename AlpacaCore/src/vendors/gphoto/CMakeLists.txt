# gphoto vendor support
# Uses the system libgphoto2/libraw packages (no vendored SDK -- unlike the
# proprietary camera SDKs, libgphoto2 and libraw are open-source libraries
# installed via apt, same as libusb/libudev are for the other camera vendors).

if(ALPACACORE_ENABLE_GPHOTO)
    message(STATUS "Building libgphoto2 DSLR/mirrorless camera driver support")

    # Linux arm64 only (Raspberry Pi 4/5, Rockchip SBCs, OrangePi, iOptron iMate)
    if(NOT UNIX)
        message(FATAL_ERROR "gphoto driver is supported on Linux only.")
    endif()
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        message(FATAL_ERROR "Unsupported architecture: ${CMAKE_SYSTEM_PROCESSOR}. AlpacaBridge is arm64-only.")
    endif()

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBGPHOTO2 REQUIRED IMPORTED_TARGET libgphoto2)
    pkg_check_modules(LIBRAW REQUIRED IMPORTED_TARGET libraw)

    set(GPHOTO_SOURCES
        gphoto_sdk_wrapper.cpp
        gphoto_camera_driver.cpp
    )

    add_library(alpacacore_gphoto STATIC ${GPHOTO_SOURCES})

    target_include_directories(alpacacore_gphoto PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )

    target_link_libraries(alpacacore_gphoto PRIVATE PkgConfig::LIBGPHOTO2 PkgConfig::LIBRAW)

    find_package(Threads REQUIRED)
    target_link_libraries(alpacacore_gphoto PRIVATE Threads::Threads)
endif()
