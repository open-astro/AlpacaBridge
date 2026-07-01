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

    # Linux arm64 only (Raspberry Pi 4/5, Rockchip SBCs, OrangePi, iOptron iMate)
    if(NOT UNIX)
        message(FATAL_ERROR "ToupTek driver is supported on Linux only.")
    endif()
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        message(FATAL_ERROR "Unsupported architecture for ToupTek SDK: ${CMAKE_SYSTEM_PROCESSOR}. AlpacaBridge is arm64-only.")
    endif()
    set(TOUPTEK_LIB_DIR "${TOUPTEK_SDK_ROOT}/linux/arm64/glibc")

    set(TOUPTEK_LIB_PATH "${TOUPTEK_LIB_DIR}/libtoupcam.so")

    if(NOT EXISTS "${TOUPTEK_LIB_PATH}")
        message(FATAL_ERROR "ToupTek SDK shared library not found at ${TOUPTEK_LIB_PATH}")
    endif()

    set(TOUPTEK_SOURCES
        touptek_sdk_wrapper.cpp
        touptek_camera_driver.cpp
        touptek_focuser_driver.cpp
        touptek_filterwheel_driver.cpp
        touptek_thermal_switch_driver.cpp
    )

    # StellaVita power-box Switch backend. The StellaVita is a CM4-based
    # controller whose on-board 12V DC ports are local GPIO outputs (libgpiod),
    # independent of the camera SDK above. Probe for libgpiod >= 2.0 (pkg-config
    # first; then a find_library fallback that confirms a v2-only symbol) and
    # compile the Switch sources in only when it is available, so a camera-only
    # build still works on hosts without libgpiod.
    set(_touptek_stellavita OFF)
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBGPIOD QUIET IMPORTED_TARGET libgpiod>=2.0)
    endif()
    if(LIBGPIOD_FOUND)
        set(_touptek_stellavita ON)
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
                set(_touptek_stellavita ON)
            endif()
        endif()
    endif()

    if(_touptek_stellavita)
        # StellaVita: ASCOM Switch driver for the on-board 12V DC power ports
        # (local GPIO via libgpiod), independent of the camera/focuser SDK.
        list(APPEND TOUPTEK_SOURCES
            touptek_powerbox_wrapper.cpp
            touptek_switch_driver.cpp
        )
    else()
        message(STATUS
            "ToupTek StellaVita Switch disabled: libgpiod (>= 2.0) not found. "
            "The camera/focuser drivers are still built. Install libgpiod-dev "
            "(>= 2.0) — e.g. apt install libgpiod-dev on Debian Trixie — to enable it.")
    endif()

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

    if(_touptek_stellavita)
        # libgpiod (>= 2.0) backs the StellaVita Switch driver. The DC outputs
        # are driven via /dev/gpiochip0 (BCM2711) character-device lines.
        if(LIBGPIOD_FOUND)
            target_include_directories(alpacacore_touptek PRIVATE ${LIBGPIOD_INCLUDE_DIRS})
            target_compile_options(alpacacore_touptek PRIVATE ${LIBGPIOD_CFLAGS_OTHER})
            if(TARGET PkgConfig::LIBGPIOD)
                target_link_libraries(alpacacore_touptek PRIVATE PkgConfig::LIBGPIOD)
            else()
                target_link_libraries(alpacacore_touptek PRIVATE ${LIBGPIOD_LIBRARIES})
            endif()
        else()
            target_include_directories(alpacacore_touptek PRIVATE ${LIBGPIOD_INCLUDE_DIR})
            target_link_libraries(alpacacore_touptek PRIVATE ${LIBGPIOD_LIBRARY})
        endif()
        message(STATUS "ToupTek driver: camera + focuser + StellaVita (libgpiod) Switch support enabled")
    else()
        message(STATUS "ToupTek driver: camera + focuser support enabled (StellaVita Switch disabled)")
    endif()

    # Tell dependents (the AlpacaHTTP router, the AlpacaCore tests) whether the
    # StellaVita Switch was compiled in. A cache variable is used because
    # AlpacaCore links this vendor library PRIVATEly, so a PUBLIC compile
    # definition would not reach AlpacaHTTP's translation units.
    set(ALPACACORE_TOUPTEK_STELLAVITA ${_touptek_stellavita} CACHE INTERNAL
        "ToupTek StellaVita Switch driver compiled in")
endif()
