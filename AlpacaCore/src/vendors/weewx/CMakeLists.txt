# WeeWX vendor support
# WeeWX data is retrieved via HTTP JSON feed

if(ALPACACORE_ENABLE_WEEWX)
    message(STATUS "Building WeeWX observing conditions driver support")

    find_package(CURL REQUIRED)

    set(WEEWX_SOURCES
        weewx_observingconditions_driver.cpp
        weewx_catalog.cpp
    )

    add_library(alpacacore_weewx STATIC ${WEEWX_SOURCES})

    target_include_directories(alpacacore_weewx PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )

    # Base drivers, logging and AsyncConnectable live in alpacacore; this vendor
    # links down to it, never the reverse (open-astro#710).
    target_link_libraries(alpacacore_weewx PRIVATE alpacacore)

    # HTTP only through util::HttpClient (decision 0023): the seam's libcurl
    # implementation is alpacacore_http_client, built when CURL is found.
    if(NOT TARGET alpacacore_http_client)
        message(FATAL_ERROR "WeeWX needs alpacacore_http_client, which is built only when libcurl is found "
                            "(install libcurl4-openssl-dev, or disable ALPACACORE_ENABLE_WEEWX)")
    endif()
    target_link_libraries(alpacacore_weewx PRIVATE alpacacore_http_client)

    message(STATUS "WeeWX driver: HTTP JSON feed support enabled")
endif()
