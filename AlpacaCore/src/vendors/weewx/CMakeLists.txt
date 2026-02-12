# WeeWX vendor support
# WeeWX data is retrieved via HTTP JSON feed

if(ALPACACORE_ENABLE_WEEWX)
    message(STATUS "Building WeeWX observing conditions driver support")

    find_package(CURL REQUIRED)

    set(WEEWX_SOURCES
        weewx_observingconditions_driver.cpp
    )

    add_library(alpacacore_weewx STATIC ${WEEWX_SOURCES})

    target_include_directories(alpacacore_weewx PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )

    target_link_libraries(alpacacore_weewx PRIVATE CURL::libcurl)

    message(STATUS "WeeWX driver: HTTP JSON feed support enabled")
endif()
