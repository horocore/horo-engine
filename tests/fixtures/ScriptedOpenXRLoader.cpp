/** @file ScriptedOpenXRLoader.cpp
 * @brief Linux-only test artifact loaded through the unchanged signed product loader path.
 * @details The executable owns the same existing native script for the entire verified loader/session lifetime.
 * This artifact supplies only the official loader entry point; it owns no parallel session or fake identity registry.
 */
#include <openxr/openxr.h>

extern "C" XRAPI_ATTR XrResult XRAPI_CALL HoroTestOpenXRResolve(XrInstance instance, const char *name, PFN_xrVoidFunction *function);

extern "C" [[gnu::visibility("default")]] XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char *name,
                                                                                               PFN_xrVoidFunction *function) {
    return HoroTestOpenXRResolve(instance, name, function);
}
