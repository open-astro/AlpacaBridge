# remotealpaca vendor: a client for one device of an upstream Alpaca server
# (decision 0023, issue #1753). Slice 2 builds the client library only: no
# driver, no catalog registration, no device type yet.

if(ALPACACORE_ENABLE_REMOTEALPACA)
    if(NOT TARGET alpacacore_http_client)
        message(FATAL_ERROR "ALPACACORE_ENABLE_REMOTEALPACA needs libcurl (util::HttpClient, decision 0023)")
    endif()
    message(STATUS "Building remotealpaca upstream Alpaca client")

    add_library(alpacacore_remotealpaca STATIC remote_alpaca_client.cpp)

    target_include_directories(alpacacore_remotealpaca PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )

    # Links down to alpacacore, never the reverse (open-astro#710). HTTP only
    # through util::HttpClient (decision 0023 rule 2).
    target_link_libraries(alpacacore_remotealpaca PRIVATE alpacacore)
    target_link_libraries(alpacacore_remotealpaca PUBLIC alpacacore_http_client)
endif()
