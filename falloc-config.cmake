# falloc-config
# --------------
#
# This module defines
#
# ::
#
#   falloc_FOUND - Set to true when falloc is found.
#   falloc_INCLUDE_DIR - the directories of the falloc headers
#   falloc_LIBRARY - the falloc libraries needed for linking

find_path(falloc_INCLUDE_DIR
    NAME falloc/cache.hpp
    PATHS ${CMAKE_CURRENT_LIST_DIR})

find_library(falloc_LIBRARY
    NAMES falloc libfalloc
    PATHS ${CMAKE_CURRENT_LIST_DIR}
    PATH_SUFFIXES bin/${CMAKE_BUILD_TYPE})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(falloc
    "Could NOT find falloc, try to set the path to the falloc root folder in the variable falloc_DIR"
    falloc_LIBRARY
    falloc_INCLUDE_DIR)
