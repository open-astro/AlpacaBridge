# OnStep vendor support
# OnStep mounts use a serial LX200-derived protocol
# No SDK required - uses standard platform serial/socket APIs

if(ALPACACORE_ENABLE_ONSTEP)
    message(STATUS "Building OnStep mount driver support")

    set(ONSTEP_SOURCES
        onstep_protocol_wrapper.cpp
        onstep_telescope_driver.cpp
    )

    add_library(alpacacore_onstep STATIC ${ONSTEP_SOURCES})

    target_include_directories(alpacacore_onstep PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )

    message(STATUS "OnStep driver: Serial connection support enabled")
endif()
