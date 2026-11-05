cmake_minimum_required(VERSION 3.16)

project(test_qt_single LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../cmake)

# Prefer user-provided/include-local SDK paths on Windows.
if(NOT LIBQHYCCD_INCLUDE_DIR)
    find_path(LIBQHYCCD_INCLUDE_DIR
        NAMES qhyccd.h
        PATHS
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/include
            /usr/local/include
        PATH_SUFFIXES
            libqhyccd
    )
endif()

if(NOT LIBQHYCCD_LIBRARY)
    find_library(LIBQHYCCD_LIBRARY
        NAMES qhyccd libqhyccd
        PATHS
            ${CMAKE_CURRENT_SOURCE_DIR}/lib
            ${CMAKE_CURRENT_SOURCE_DIR}/../lib
            /usr/local/lib
    )
endif()

if(NOT LIBQHYCCD_INCLUDE_DIR OR NOT LIBQHYCCD_LIBRARY)
    message(FATAL_ERROR
        "LibQHYCCD not found. Set LIBQHYCCD_INCLUDE_DIR and LIBQHYCCD_LIBRARY, "
        "or place headers in test_qt/include and library in test_qt/lib.")
endif()

find_package(Qt6 COMPONENTS Widgets QUIET)
if(Qt6_FOUND)
    set(QT_PACKAGE Qt6)
else()
    find_package(Qt5 COMPONENTS Widgets REQUIRED)
    set(QT_PACKAGE Qt5)
endif()

add_executable(test_qt_single
    main.cpp
)

target_include_directories(test_qt_single PRIVATE ${LIBQHYCCD_INCLUDE_DIR})
target_link_libraries(test_qt_single PRIVATE ${LIBQHYCCD_LIBRARY} ${QT_PACKAGE}::Widgets)
