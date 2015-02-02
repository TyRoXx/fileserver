# - Try to find libsilicium
# Once done this will define
#  SILICIUM_FOUND
#  SILICIUM_INCLUDE_DIRS

find_path(SILICIUM_INCLUDE_DIR "silicium/version.hpp" HINTS "/usr/local/include" "/usr/include")

set(SILICIUM_INCLUDE_DIRS ${SILICIUM_INCLUDE_DIR})

include(FindPackageHandleStandardArgs)
# handle the QUIETLY and REQUIRED arguments and set LIBSILICIUM_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args(LibSilicium  DEFAULT_MSG
                                  SILICIUM_INCLUDE_DIR)

mark_as_advanced(SILICIUM_INCLUDE_DIR)
