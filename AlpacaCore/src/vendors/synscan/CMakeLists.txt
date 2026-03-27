# SynScan vendor support
# SynScan mounts use serial RS-232 protocol (and optional TCP bridges)
# No SDK required - uses standard platform serial/socket APIs

if(ALPACACORE_ENABLE_SYNSCAN)
    message(STATUS "Building SynScan mount driver support")

    set(SYNSCAN_SOURCES
        synscan_protocol_wrapper.cpp
        synscan_telescope_driver.cpp
    )

    add_library(alpacacore_synscan STATIC ${SYNSCAN_SOURCES})

    target_include_directories(alpacacore_synscan PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )

    message(STATUS "SynScan driver: Serial and TCP connection support enabled")
endif()
