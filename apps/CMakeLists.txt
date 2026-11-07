# Executable composition is grouped by its owning host. These includes share the
# apps/ directory scope so existing relative sources, generated interfaces and
# source properties keep their established identities.
include("${CMAKE_CURRENT_SOURCE_DIR}/shared/cmake/Composition.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/HoroEditor/cmake/Composition.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/horo-engine/cmake/Host.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/SdkTools.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/HoroEditor/cmake/Host.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/HoroEditor/cmake/ShaderTools.cmake")
