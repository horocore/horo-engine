#include "Horo/Extensions/ExtensionServiceAbi.h"
#define TEXT(s) {s, sizeof(s) - 1}
static unsigned unloaded;

HORO_EXTENSION_EXPORT unsigned horo_module_source_unloaded(void) {
    return unloaded;
}

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_query(HoroExtensionRequirements *requirements) {
    if (!requirements || requirements->structSize < sizeof(*requirements))
        return HORO_EXTENSION_ERROR_INVALID_ARGS;
    *requirements = (HoroExtensionRequirements){.structSize = sizeof(*requirements),
                                                .abiMajorVersion = 1,
                                                .minimumHostMinor = 8,
                                                .requiredHostApiSize = sizeof(HoroExtensionHostApi),
                                                .requiredFunctions = HORO_EXTENSION_REQUIRES_SERVICES};
    return HORO_EXTENSION_SUCCESS;
}

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_load(const HoroExtensionHostApi *host, HoroExtensionModuleApi *module) {
    if (!module || !HoroExtensionHasServiceTransport(host))
        return HORO_EXTENSION_ERROR_VERSION_MISMATCH;
    *module = (HoroExtensionModuleApi){.structSize = sizeof(*module),
                                       .moduleContext = &unloaded,
                                       .moduleId = TEXT("com.horo.source.script"),
                                       .moduleVersion = TEXT("1.0.0")};
    return HORO_EXTENSION_SUCCESS;
}

HORO_EXTENSION_EXPORT void horo_extension_unload(HoroExtensionModuleApi *module) {
    ++unloaded;
    *module = (HoroExtensionModuleApi){0};
}
