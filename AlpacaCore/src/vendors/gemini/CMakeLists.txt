cmake_minimum_required(VERSION 3.20)

set(GEMINI_SOURCES
    gemini_protocol_wrapper.cpp
    gemini_focuser_driver.cpp
)

add_library(alpacacore_gemini STATIC ${GEMINI_SOURCES})

target_include_directories(alpacacore_gemini PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# Gemini uses the MyFocuserPro2 serial protocol over USB or TCP.
# No external SDK is required — only standard POSIX/Winsock APIs.
find_package(Threads REQUIRED)
target_link_libraries(alpacacore_gemini PRIVATE Threads::Threads)
