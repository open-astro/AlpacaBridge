# iOptron vendor support
# iOptron mounts use RS-232 protocol over serial or TCP connection
# No SDK required - uses standard platform serial/socket APIs

if(ALPACACORE_ENABLE_IOPTRON)
    message(STATUS "Building iOptron mount driver support")
    
    # Source files
    set(IOPTRON_SOURCES
        ioptron_protocol_wrapper.cpp
        ioptron_telescope_driver.cpp
        # iMate PowerBox: ASCOM Switch driver for the on-board DC power ports.
        # These are local GPIO outputs (libgpiod), independent of the mount
        # RS-232 protocol above.
        ioptron_powerbox_wrapper.cpp
        ioptron_switch_driver.cpp
    )

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

    # libgpiod (>= 2.0) backs the iMate PowerBox Switch driver. The iMate's DC
    # outputs are driven via /dev/gpiochip0 character-device lines.
    if(UNIX)
        find_package(PkgConfig)
        if(PkgConfig_FOUND)
            pkg_check_modules(LIBGPIOD QUIET IMPORTED_TARGET libgpiod>=2.0)
        endif()

        if(LIBGPIOD_FOUND)
            target_include_directories(alpacacore_ioptron PRIVATE ${LIBGPIOD_INCLUDE_DIRS})
            target_compile_options(alpacacore_ioptron PRIVATE ${LIBGPIOD_CFLAGS_OTHER})
            if(TARGET PkgConfig::LIBGPIOD)
                target_link_libraries(alpacacore_ioptron PRIVATE PkgConfig::LIBGPIOD)
            else()
                target_link_libraries(alpacacore_ioptron PRIVATE ${LIBGPIOD_LIBRARIES})
            endif()
        else()
            find_library(LIBGPIOD_LIBRARY NAMES gpiod libgpiod)
            find_path(LIBGPIOD_INCLUDE_DIR NAMES gpiod.h)
            if(LIBGPIOD_LIBRARY AND LIBGPIOD_INCLUDE_DIR)
                # The pkg-config branch enforces libgpiod >= 2.0. The
                # find_library fallback has no version metadata, so probe for a
                # v2-only symbol (gpiod_chip_request_lines) before accepting it.
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
                        "— e.g. apt install libgpiod-dev on Debian Trixie.")
                endif()
                target_include_directories(alpacacore_ioptron PRIVATE ${LIBGPIOD_INCLUDE_DIR})
                target_link_libraries(alpacacore_ioptron PRIVATE ${LIBGPIOD_LIBRARY})
            else()
                message(FATAL_ERROR "libgpiod (>=2.0) not found; install it (e.g. apt install libgpiod-dev).")
            endif()
        endif()

        find_package(Threads REQUIRED)
        target_link_libraries(alpacacore_ioptron PRIVATE Threads::Threads)
    endif()

    message(STATUS "iOptron driver: Serial/TCP mount + iMate PowerBox (libgpiod) Switch support enabled")
endif()
