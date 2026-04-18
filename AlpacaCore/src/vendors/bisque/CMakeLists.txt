# Bisque vendor support
# Bisque/Paramount mounts use TheSkyX TCP scripting protocol
# No SDK required - uses standard platform TCP socket APIs

if(ALPACACORE_ENABLE_BISQUE)
    message(STATUS "Building Bisque/Paramount (TheSkyX) mount driver support")

    set(BISQUE_SOURCES
        bisque_protocol_wrapper.cpp
        bisque_telescope_driver.cpp
    )

    add_library(alpacacore_bisque STATIC ${BISQUE_SOURCES})

    target_include_directories(alpacacore_bisque PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )

    message(STATUS "Bisque driver: TCP connection to TheSkyX enabled")
endif()
