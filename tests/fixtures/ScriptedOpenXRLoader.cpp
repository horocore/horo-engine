/** @file ScriptedOpenXRLoader.cpp
 * @brief Linux-only test artifact loaded through the unchanged signed product loader path.
 * @details The executable owns the same existing native script for the entire verified loader/session lifetime.
 * This artifact forwards the official dispatch and bootstrap retirement exports; it owns no parallel session or identity registry.
 */
#include <openxr/openxr.h>

extern "C" XRAPI_ATTR XrResult XRAPI_CALL HoroTestOpenXRResolve(XrInstance instance, const char *name, PFN_xrVoidFunction *function);

extern "C" [[gnu::visibility("default")]] XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char *name,
                                                                                               PFN_xrVoidFunction *function) {
    return HoroTestOpenXRResolve(instance, name, function);
}

extern "C" [[gnu::visibility("default")]] XRAPI_ATTR XrResult XRAPI_CALL xrDestroyInstance(XrInstance instance) {
    PFN_xrVoidFunction function{};
    const auto resolved = HoroTestOpenXRResolve(XR_NULL_HANDLE, "xrDestroyInstance", &function);
    if (XR_FAILED(resolved))
        return resolved;
    if (!function)
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    return reinterpret_cast<PFN_xrDestroyInstance>(function)(instance);
}
