# Celestron vendor support
# Celestron NexStar mounts use serial RS-232 protocol (and optional TCP/WiFi bridges)
# No SDK required - uses standard platform serial/socket APIs

if(ALPACACORE_ENABLE_CELESTRON)
    message(STATUS "Building Celestron NexStar mount driver support")

    set(CELESTRON_SOURCES
        celestron_protocol_wrapper.cpp
        celestron_telescope_driver.cpp
    )

    add_library(alpacacore_celestron STATIC ${CELESTRON_SOURCES})

    target_include_directories(alpacacore_celestron PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )

    message(STATUS "Celestron driver: Serial and TCP connection support enabled")
endif()
