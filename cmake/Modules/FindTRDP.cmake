# Find the TCNopen TRDP stack installation
#
#  TRDP_INCLUDE_DIRS - header directories
#  TRDP_LIBRARIES    - libraries to link against
#  TRDP_FOUND        - TRUE if found

find_path(TRDP_INCLUDE_DIR trdp_if.h
    HINTS $ENV{TRDP_ROOT}/include ${TRDP_ROOT}/include
    PATH_SUFFIXES api include
)

find_library(TRDP_LIBRARY NAMES trdp
    HINTS $ENV{TRDP_ROOT}/lib ${TRDP_ROOT}/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(TRDP REQUIRED_VARS TRDP_INCLUDE_DIR TRDP_LIBRARY)

if (TRDP_FOUND)
    set(TRDP_INCLUDE_DIRS ${TRDP_INCLUDE_DIR})
    set(TRDP_LIBRARIES ${TRDP_LIBRARY})
endif()
