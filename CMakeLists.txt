cmake_minimum_required(VERSION 3.15)
project(AlpacaHTTP VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options
option(ALPACAHTTP_BUILD_TESTS "Build test suite" ON)
option(ALPACAHTTP_USE_BOOST_BEAST "Use Boost.Beast for HTTP server" ON)
option(ALPACAHTTP_ENABLE_DISCOVERY "Enable Alpaca Discovery" ON)

# Find dependencies
find_package(nlohmann_json 3.2.0 REQUIRED)

if(ALPACAHTTP_USE_BOOST_BEAST)
    find_package(Boost REQUIRED COMPONENTS system)
    add_definitions(-DALPACAHTTP_USE_BOOST_BEAST)
endif()

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Source files
set(ALPACAHTTP_SOURCES
    src/core/request.cpp
    src/core/response.cpp
    src/core/config.cpp
    src/core/json_utils.cpp
    src/http/router.cpp
    src/http/server.cpp
    src/http/session.cpp
    src/util/threading.cpp
    src/util/logging_adapter.cpp
    src/util/error_mapping.cpp
)

if(ALPACAHTTP_ENABLE_DISCOVERY)
    list(APPEND ALPACAHTTP_SOURCES src/discovery/discovery.cpp)
    add_definitions(-DALPACAHTTP_ENABLE_DISCOVERY)
endif()

# Create library
add_library(alpacahttp STATIC ${ALPACAHTTP_SOURCES})

target_link_libraries(alpacahttp
    PUBLIC
        nlohmann_json::nlohmann_json
)

if(ALPACAHTTP_USE_BOOST_BEAST)
    target_link_libraries(alpacahttp
        PUBLIC
            Boost::system
    )
endif()

# Install
install(TARGETS alpacahttp
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/alpacahttp
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# Example application
add_executable(alpacahttp_server examples/simple_server/main.cpp)
target_link_libraries(alpacahttp_server alpacahttp)

# Tests
if(ALPACAHTTP_BUILD_TESTS)
    enable_testing()
    
    # Test executables
    add_executable(test_routing tests/test_routing.cpp)
    target_link_libraries(test_routing alpacahttp)
    add_test(NAME test_routing COMMAND test_routing)
    
    add_executable(test_json tests/test_json.cpp)
    target_link_libraries(test_json alpacahttp)
    add_test(NAME test_json COMMAND test_json)
    
    if(ALPACAHTTP_ENABLE_DISCOVERY)
        add_executable(test_discovery tests/test_discovery.cpp)
        target_link_libraries(test_discovery alpacahttp)
        add_test(NAME test_discovery COMMAND test_discovery)
    endif()
    
    add_executable(test_config tests/test_config.cpp)
    target_link_libraries(test_config alpacahttp)
    add_test(NAME test_config COMMAND test_config)
endif()

