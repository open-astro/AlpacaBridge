# iOptron vendor support
# iOptron mounts use RS-232 protocol over serial or TCP connection
# No SDK required - uses standard platform serial/socket APIs

if(ALPACACORE_ENABLE_IOPTRON)
    message(STATUS "Building iOptron mount driver support")
    
    # Source files
    set(IOPTRON_SOURCES
        ioptron_protocol_wrapper.cpp
        ioptron_telescope_driver.cpp
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
    
    # Platform-specific settings
    if(WIN32)
        # Windows: Link Winsock library for TCP sockets
        target_link_libraries(alpacacore_ioptron PRIVATE ws2_32)
    endif()
    
    # Compiler-specific settings
    if(MSVC)
        # Suppress warnings for platform-specific includes
        target_compile_options(alpacacore_ioptron PRIVATE /wd4996)
    endif()
    
    message(STATUS "iOptron driver: Serial and TCP connection support enabled")
endif()
