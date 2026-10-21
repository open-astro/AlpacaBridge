# 查找 LibQHYCCD 库



if(WIN32)
  find_path(LIBQHYCCD_INCLUDE_DIR
    NAMES qhyccd.h
    PATHS ${CMAKE_SOURCE_DIR}/include
    PATH_SUFFIXES .
  )
else()
  find_path(LIBQHYCCD_INCLUDE_DIR
    NAMES qhyccd.h
    PATHS /usr/local/include
    PATH_SUFFIXES libqhyccd
  )
endif()


if(WIN32)
  find_library(LIBQHYCCD_LIBRARY
    NAMES qhyccd
    PATHS ${CMAKE_SOURCE_DIR}  ${CMAKE_SOURCE_DIR}/lib
    PATH_SUFFIXES .
  )
else()
  find_library(LIBQHYCCD_LIBRARY
    NAMES qhyccd
    PATHS /usr/local/lib
  )
endif()


include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibQHYCCD DEFAULT_MSG
  LIBQHYCCD_LIBRARY LIBQHYCCD_INCLUDE_DIR)


if(LIBQHYCCD_FOUND)
  set(LIBQHYCCD_LIBRARIES ${LIBQHYCCD_LIBRARY})
  set(LIBQHYCCD_INCLUDE_DIRS ${LIBQHYCCD_INCLUDE_DIR})
endif()

mark_as_advanced(LIBQHYCCD_INCLUDE_DIR LIBQHYCCD_LIBRARY)