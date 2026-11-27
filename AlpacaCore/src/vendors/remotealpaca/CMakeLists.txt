# remotealpaca vendor: devices of an upstream Alpaca server served as proxied
# devices (decision 0023, issue #1753): the upstream client, the self-loop check
# and the ObservingConditions, SafetyMonitor, Rotator and FilterWheel drivers over the
# shared UpstreamProxy (upstream_proxy.h), with their catalog factories.

if(ALPACACORE_ENABLE_REMOTEALPACA)
    if(NOT TARGET alpacacore_http_client)
        message(FATAL_ERROR "ALPACACORE_ENABLE_REMOTEALPACA needs libcurl (util::HttpClient, decision 0023)")
    endif()
    message(STATUS "Building remotealpaca proxied devices (upstream Alpaca client)")

    add_library(alpacacore_remotealpaca STATIC
        remote_alpaca_client.cpp
        self_loop.cpp
        remotealpaca_observingconditions_driver.cpp
        remotealpaca_safetymonitor_driver.cpp
        remotealpaca_rotator_driver.cpp
        remotealpaca_filterwheel_driver.cpp
        remotealpaca_catalog.cpp
    )

    target_include_directories(alpacacore_remotealpaca PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )

    # Links down to alpacacore, never the reverse (open-astro#710). HTTP only
    # through util::HttpClient (decision 0023 rule 2).
    target_link_libraries(alpacacore_remotealpaca PRIVATE alpacacore)
    target_link_libraries(alpacacore_remotealpaca PUBLIC alpacacore_http_client)
endif()
