#include "Horo/Extensions/ExtensionAbi.h"

#include <stddef.h>

static uint32_t loadCount;

#if HORO_ABI_FIXTURE_MODE == 4
static HoroExtensionStatus ImportAsset(void *context, const HoroAssetImportRequest *request, HoroAssetImportResponse *response) {
    (void)context;
    (void)request;
    (void)response;
    return HORO_EXTENSION_SUCCESS;
}

static void DestroyImporter(void *context) {
    uint32_t *destroyCount = (uint32_t *)context;
    ++*destroyCount;
}
#endif

/** @brief Expose whether negotiation prevented the load callback. */
HORO_EXTENSION_EXPORT uint32_t horo_test_load_count(void) {
    return loadCount;
}

#if HORO_ABI_FIXTURE_MODE != 0
/** @brief Return a valid or deliberately incompatible inert requirements table. */
HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_query(HoroExtensionRequirements *requirements) {
    *requirements =
        (HoroExtensionRequirements){.structSize = sizeof(HoroExtensionRequirements),
                                    .abiMajorVersion = HORO_EXTENSION_ABI_VERSION,
                                    .minimumHostMinor = HORO_ABI_FIXTURE_MODE == 2 ? 99 : 0,
                                    .requiredHostApiSize = offsetof(HoroExtensionHostApi, abiMinorVersion),
                                    .requiredFunctions = HORO_ABI_FIXTURE_MODE == 4 ? HORO_EXTENSION_REQUIRES_ASSET_IMPORTER : 0};
    return HORO_EXTENSION_SUCCESS;
}
#endif

/** @brief Return a legacy module prefix or a deliberately truncated result. */
HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_load(const HoroExtensionHostApi *host, HoroExtensionModuleApi *outModule) {
    ++loadCount;
#if HORO_ABI_FIXTURE_MODE == 4
    static const char contributionId[] = "fixture.importer";
    static const char contributionVersion[] = "1.0.0";
    static uint32_t destroyCount;
    const HoroAssetImporterDescriptor importer = {
        .structSize = sizeof(HoroAssetImporterDescriptor),
        .abiVersion = HORO_ASSET_IMPORTER_ABI_VERSION,
        .contributionId = {contributionId, sizeof(contributionId) - 1},
        .contributionVersion = {contributionVersion, sizeof(contributionVersion) - 1},
        .importerContext = &destroyCount,
        .importAsset = ImportAsset,
        .destroyImporter = DestroyImporter,
    };
    const HoroExtensionStatus registrationStatus = host->registerAssetImporter(host->hostContext, &importer);
    if (registrationStatus != HORO_EXTENSION_SUCCESS)
        return registrationStatus;
#else
    (void)host;
#endif
    outModule->structSize = HORO_ABI_FIXTURE_MODE == 3 ? 1 : offsetof(HoroExtensionModuleApi, moduleId);
    return HORO_EXTENSION_SUCCESS;
}

/** @brief Accept one deterministic host shutdown callback for conformance coverage. */
HORO_EXTENSION_EXPORT void horo_extension_unload(HoroExtensionModuleApi *module) {
    *module = (HoroExtensionModuleApi){0};
}
