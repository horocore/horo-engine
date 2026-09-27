#include "Horo/Extensions/ExtensionAbi.h"

#include <stddef.h>

_Static_assert(HORO_EXTENSION_ABI_VERSION == 1, "Unexpected extension ABI major");
_Static_assert(HORO_EXTENSION_ABI_MINOR_VERSION == 3, "Unexpected extension ABI minor");
_Static_assert(HORO_EXTENSION_SDK_ABI_MAJOR == HORO_EXTENSION_ABI_VERSION, "SDK metadata and ABI header major versions disagree");
_Static_assert(HORO_EXTENSION_SDK_ABI_MIN_HOST_MINOR == 0, "Unexpected oldest supported host minor");
_Static_assert(HORO_EXTENSION_SDK_ABI_PUBLISHED_MINOR == HORO_EXTENSION_ABI_MINOR_VERSION,
               "SDK metadata and ABI header minor versions disagree");
_Static_assert(HORO_EXTENSION_SDK_ABI_MAX_HOST_MAJOR_EXCLUSIVE == 2, "Unexpected next incompatible host ABI major");

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_query(HoroExtensionRequirements *requirements) {
    if (requirements == NULL || requirements->structSize < sizeof(*requirements))
        return HORO_EXTENSION_ERROR_INVALID_ARGS;

    *requirements = (HoroExtensionRequirements){
        .structSize = sizeof(*requirements),
        .abiMajorVersion = HORO_EXTENSION_ABI_VERSION,
        .minimumHostMinor = HORO_EXTENSION_ABI_MINOR_VERSION,
        .requiredHostApiSize = sizeof(HoroExtensionHostApi),
        .requiredFunctions = HORO_EXTENSION_REQUIRES_ASSET_IMPORTER,
    };
    return HORO_EXTENSION_SUCCESS;
}

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_load(const HoroExtensionHostApi *host, HoroExtensionModuleApi *module) {
    static const char module_id[] = "com.horo.sdk-fixture";
    static const char module_version[] = "1.0.0";
    if (host == NULL || module == NULL || host->structSize < sizeof(HoroExtensionHostApi) || host->abiVersion != HORO_EXTENSION_ABI_VERSION)
        return HORO_EXTENSION_ERROR_VERSION_MISMATCH;

    *module = (HoroExtensionModuleApi){
        .structSize = sizeof(*module),
        .moduleId = {module_id, sizeof(module_id) - 1},
        .moduleVersion = {module_version, sizeof(module_version) - 1},
    };
    return HORO_EXTENSION_SUCCESS;
}

HORO_EXTENSION_EXPORT void horo_extension_unload(HoroExtensionModuleApi *module) {
    if (module != NULL)
        *module = (HoroExtensionModuleApi){0};
}
