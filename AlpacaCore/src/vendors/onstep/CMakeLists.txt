# OnStep vendor support
# OnStep controllers (the mount, its focusers, its rotator and its auxiliary features) use a serial or TCP LX200-derived protocol
# No SDK required - uses standard platform serial/socket APIs

if(ALPACACORE_ENABLE_ONSTEP)
    message(STATUS "Building OnStep mount driver support")

    set(ONSTEP_SOURCES
        onstep_protocol_wrapper.cpp
        onstep_telescope_driver.cpp
        onstep_focuser_driver.cpp
        onstep_rotator_driver.cpp
        onstep_switch_driver.cpp
        onstep_catalog.cpp
    )

    add_library(alpacacore_onstep STATIC ${ONSTEP_SOURCES})

    target_include_directories(alpacacore_onstep PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )

    # Base drivers, logging and AsyncConnectable live in alpacacore; this vendor
    # links down to it, never the reverse (open-astro#710).
    target_link_libraries(alpacacore_onstep PRIVATE alpacacore)

    message(STATUS "OnStep driver: Serial connection support enabled")
endif()
