# Celestron vendor support
# NexStar uses HC serial/TCP; CGX/CGX-L direct USB and the focus motor's own
# USB port use the AUX serial protocol.
# No SDK required - uses standard platform serial/socket APIs

if(ALPACACORE_ENABLE_CELESTRON)
    message(STATUS "Building Celestron NexStar and CGX/CGX-L mount driver support")

    set(CELESTRON_SOURCES
        celestron_cgx_geometry.cpp
        celestron_cgx_protocol.cpp
        celestron_cgx_protocol_wrapper.cpp
        celestron_cgx_telescope_driver.cpp
        celestron_focuser_driver.cpp
        celestron_protocol_wrapper.cpp
        celestron_telescope_driver.cpp
        celestron_catalog.cpp
    )

    add_library(alpacacore_celestron STATIC ${CELESTRON_SOURCES})

    target_include_directories(alpacacore_celestron PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )

    # Base drivers, logging and AsyncConnectable live in alpacacore; this vendor
    # links down to it, never the reverse (open-astro#710).
    target_link_libraries(alpacacore_celestron PRIVATE alpacacore)

    message(STATUS "Celestron driver: NexStar serial/TCP and CGX direct USB support enabled")
endif()
