# SkyWatcher vendor support (motor controller protocol: Wave 100i/150i, AZ-GTi class)
# Direct motor-controller command set over USB serial or built-in Wi-Fi (UDP 11880)
# No SDK required - uses standard platform serial/socket APIs

if(ALPACACORE_ENABLE_SKYWATCHER)
    message(STATUS "Building SkyWatcher motor controller mount driver support")

    set(SKYWATCHER_SOURCES
        skywatcher_protocol_wrapper.cpp
        skywatcher_telescope_driver.cpp
        skywatcher_catalog.cpp
    )

    add_library(alpacacore_skywatcher STATIC ${SKYWATCHER_SOURCES})

    target_include_directories(alpacacore_skywatcher PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )

    # Base drivers, logging and AsyncConnectable live in alpacacore; this vendor
    # links down to it, never the reverse (open-astro#710).
    target_link_libraries(alpacacore_skywatcher PRIVATE alpacacore)

    message(STATUS "SkyWatcher driver: Serial and UDP (Wi-Fi) connection support enabled")
endif()
