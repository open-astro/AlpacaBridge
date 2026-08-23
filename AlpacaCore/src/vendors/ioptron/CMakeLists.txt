# iOptron vendor support
# iOptron mounts use RS-232 protocol over serial or TCP connection
# No SDK required - uses standard platform serial/socket APIs

if(ALPACACORE_ENABLE_IOPTRON)
    message(STATUS "Building iOptron mount driver support")

    # Mount (serial/TCP) driver — pure platform serial/socket APIs, builds on
    # any platform. The iMate PowerBox Switch sources are added below only when
    # libgpiod (>= 2.0) is available, so an iOptron *telescope*-only build still
    # works on macOS or a Linux box without GPIO/libgpiod.
    set(IOPTRON_SOURCES
        ioptron_protocol_wrapper.cpp
        ioptron_telescope_driver.cpp
        ioptron_ieaf_protocol_wrapper.cpp
        ioptron_ieaf_focuser_driver.cpp
    )

    # Probe for libgpiod >= 2.0 (the iMate PowerBox Switch backend). pkg-config
    # first; then a find_library fallback that confirms a v2-only symbol.
    set(_ioptron_powerbox OFF)
    if(UNIX)
        find_package(PkgConfig)
        if(PkgConfig_FOUND)
            pkg_check_modules(LIBGPIOD QUIET IMPORTED_TARGET libgpiod>=2.0)
        endif()

        if(LIBGPIOD_FOUND)
            set(_ioptron_powerbox ON)
        else()
            find_library(LIBGPIOD_LIBRARY NAMES gpiod libgpiod)
            find_path(LIBGPIOD_INCLUDE_DIR NAMES gpiod.h)
            if(LIBGPIOD_LIBRARY AND LIBGPIOD_INCLUDE_DIR)
                # No version metadata via find_library, so probe for a v2-only
                # symbol (gpiod_chip_request_lines) before accepting it.
                include(CheckSymbolExists)
                set(CMAKE_REQUIRED_INCLUDES "${LIBGPIOD_INCLUDE_DIR}")
                set(CMAKE_REQUIRED_LIBRARIES "${LIBGPIOD_LIBRARY}")
                check_symbol_exists(gpiod_chip_request_lines "gpiod.h" LIBGPIOD_V2_OK)
                unset(CMAKE_REQUIRED_INCLUDES)
                unset(CMAKE_REQUIRED_LIBRARIES)
                if(LIBGPIOD_V2_OK)
                    set(_ioptron_powerbox ON)
                endif()
            endif()
        endif()
    endif()

    if(_ioptron_powerbox)
        # iMate PowerBox: ASCOM Switch driver for the on-board DC power ports.
        # These are local GPIO outputs (libgpiod), independent of the mount
        # RS-232 protocol above.
        list(APPEND IOPTRON_SOURCES
            ioptron_powerbox_wrapper.cpp
            ioptron_switch_driver.cpp
        )
    else()
        message(STATUS
            "iOptron iMate PowerBox Switch disabled: libgpiod (>= 2.0) not found. "
            "The mount (serial/TCP) driver is still built. Install libgpiod-dev "
            "(>= 2.0) — e.g. apt install libgpiod-dev on Debian Trixie — to enable it.")
    endif()

    # Create vendor library
    add_library(alpacacore_ioptron STATIC ${IOPTRON_SOURCES})

    # Include directories
    # The vendor library only needs headers from alpacacore, not the library itself
    # The linking is one-way: alpacacore -> alpacacore_ioptron (in main CMakeLists.txt)
    target_include_directories(alpacacore_ioptron PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )

    # Do NOT link to alpacacore here - that creates a circular dependency
    # The vendor library only uses headers, and alpacacore will link to this library

    if(_ioptron_powerbox)
        # libgpiod (>= 2.0) backs the iMate PowerBox Switch driver. The iMate's
        # DC outputs are driven via /dev/gpiochip1 character-device lines.
        if(LIBGPIOD_FOUND)
            target_include_directories(alpacacore_ioptron PRIVATE ${LIBGPIOD_INCLUDE_DIRS})
            target_compile_options(alpacacore_ioptron PRIVATE ${LIBGPIOD_CFLAGS_OTHER})
            if(TARGET PkgConfig::LIBGPIOD)
                target_link_libraries(alpacacore_ioptron PRIVATE PkgConfig::LIBGPIOD)
            else()
                target_link_libraries(alpacacore_ioptron PRIVATE ${LIBGPIOD_LIBRARIES})
            endif()
        else()
            target_include_directories(alpacacore_ioptron PRIVATE ${LIBGPIOD_INCLUDE_DIR})
            target_link_libraries(alpacacore_ioptron PRIVATE ${LIBGPIOD_LIBRARY})
        endif()

        find_package(Threads REQUIRED)
        target_link_libraries(alpacacore_ioptron PRIVATE Threads::Threads)

        message(STATUS "iOptron driver: Serial/TCP mount + iMate PowerBox (libgpiod) Switch support enabled")
    else()
        message(STATUS "iOptron driver: Serial/TCP mount support enabled (iMate PowerBox Switch disabled)")
    endif()

    # Tell dependents (the AlpacaHTTP router, the AlpacaCore tests) whether the
    # PowerBox Switch was compiled in. A cache variable is used because
    # AlpacaCore links this vendor library PRIVATEly, so a PUBLIC compile
    # definition would not reach AlpacaHTTP's translation units.
    set(ALPACACORE_IOPTRON_POWERBOX ${_ioptron_powerbox} CACHE INTERNAL
        "iOptron iMate PowerBox Switch driver compiled in")
endif()
