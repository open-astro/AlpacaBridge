# gphoto vendor support
# libgphoto2 is BUNDLED: scripts/build_bundled_gphoto.sh builds the vendored
# tarball (AlpacaCore/external/libgphoto2/) into ALPACACORE_GPHOTO_PREFIX, and
# this target links that copy, never the distro's. The executables carry a
# DT_RPATH to the prefix so the loader finds it without ldconfig. libraw stays a
# system package (apt), like libusb/libudev for the other camera vendors.

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

    set(GPHOTO_BUNDLE_DIR "${ALPACACORE_GPHOTO_STAGE}${ALPACACORE_GPHOTO_PREFIX}")
    foreach(_gp_file include/gphoto2/gphoto2.h lib/libgphoto2.so lib/libgphoto2_port.so)
        if(NOT EXISTS "${GPHOTO_BUNDLE_DIR}/${_gp_file}")
            message(FATAL_ERROR
                "Bundled libgphoto2 not found: ${GPHOTO_BUNDLE_DIR}/${_gp_file} is missing. "
                "Run scripts/build_bundled_gphoto.sh (see docs/development.md), or point "
                "ALPACACORE_GPHOTO_STAGE at a --destdir it populated.")
        endif()
    endforeach()
    foreach(_gp_lib libgphoto2 libgphoto2_port)
        add_library(alpacacore_bundled_${_gp_lib} SHARED IMPORTED)
        set_target_properties(alpacacore_bundled_${_gp_lib} PROPERTIES
            IMPORTED_LOCATION "${GPHOTO_BUNDLE_DIR}/lib/${_gp_lib}.so"
            INTERFACE_INCLUDE_DIRECTORIES "${GPHOTO_BUNDLE_DIR}/include")
    endforeach()
    pkg_check_modules(LIBRAW REQUIRED IMPORTED_TARGET libraw)

    set(GPHOTO_SOURCES
        gphoto_sdk_wrapper.cpp
        gphoto_camera_driver.cpp
        gphoto_catalog.cpp
    )

    add_library(alpacacore_gphoto STATIC ${GPHOTO_SOURCES})

    target_include_directories(alpacacore_gphoto PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )

    # Base drivers, logging and AsyncConnectable live in alpacacore; this vendor
    # links down to it, never the reverse (open-astro#710).
    target_link_libraries(alpacacore_gphoto PRIVATE alpacacore)

    target_link_libraries(alpacacore_gphoto PRIVATE
        alpacacore_bundled_libgphoto2 alpacacore_bundled_libgphoto2_port PkgConfig::LIBRAW)

    # A DT_RPATH (not DT_RUNPATH) naming the run-time prefix: the loader applies
    # it to transitive lookups, so libgphoto2.so.6 finds the bundled
    # libgphoto2_port.so.12 even when the distro ships the same soname. PUBLIC so
    # it reaches the executables through the static-library chain; the
    # -rpath-link entry lets the linker resolve the library's own dependencies in
    # the staged tree.
    target_link_options(alpacacore_gphoto PUBLIC
        "-Wl,-rpath,${ALPACACORE_GPHOTO_PREFIX}/lib"
        "-Wl,--disable-new-dtags"
        "-Wl,-rpath-link,${GPHOTO_BUNDLE_DIR}/lib")

    find_package(Threads REQUIRED)
    target_link_libraries(alpacacore_gphoto PRIVATE Threads::Threads)
endif()
