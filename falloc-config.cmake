# falloc-config
# --------------
#
# This module defines
#
# ::
#
#   falloc_FOUND - Set to true when falloc is found.
#   falloc_INCLUDE_DIRS - the directories of the falloc headers
#   falloc_LIBRARIES - the falloc libraries needed for linking
#   falloc_LIBRARY_DIRS  - the link directories for falloc libraries

set(falloc_FOUND TRUE)
set(falloc_INCLUDE_DIRS ${falloc_DIR})
set(falloc_LIBRARIES falloc)
get_filename_component(
    falloc_LIBRARY_DIRS
    ${falloc_DIR}/bin/${CMAKE_BUILD_TYPE}
    ABSOLUTE)
