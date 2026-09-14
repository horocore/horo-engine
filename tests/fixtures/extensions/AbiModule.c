#include "Horo/Extensions/ExtensionAbi.h"

#include <stddef.h>

static uint32_t loadCount;

#if HORO_ABI_FIXTURE_MODE == 4 || HORO_ABI_FIXTURE_MODE == 5 || HORO_ABI_FIXTURE_MODE == 8
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

static HoroExtensionStatus RegisterFixtureImporters(const HoroExtensionHostApi *host) {
    static const char contributionId[] = "fixture.importer";
    static const char contributionVersion[] = "1.0.0";
    static uint32_t destroyCount;
    const HoroAssetImporterDescriptor importer = {
        .structSize = sizeof(HoroAssetImporterDescriptor),
        .abiVersion = HORO_ASSET_IMPORTER_ABI_VERSION,
        .contributionId = {contributionId, sizeof(contributionId) - 1},
        .contributionVersion = {contributionVersion, sizeof(contributionVersion) - 1},
        .importerContext = HORO_ABI_FIXTURE_MODE == 8 ? NULL : &destroyCount,
        .importAsset = HORO_ABI_FIXTURE_MODE == 5 ? NULL : ImportAsset,
        .destroyImporter = HORO_ABI_FIXTURE_MODE == 8 ? NULL : DestroyImporter,
    };
    const uint32_t registrationAttempts = HORO_ABI_FIXTURE_MODE == 8 ? 257 : 1;
    for (uint32_t index = 0; index < registrationAttempts; ++index) {
        const HoroExtensionStatus status = host->registerAssetImporter(host->hostContext, &importer);
        if (status != HORO_EXTENSION_SUCCESS)
            return status;
    }
    return HORO_EXTENSION_SUCCESS;
}
#endif

/** @brief Expose whether negotiation prevented the load callback. */
HORO_EXTENSION_EXPORT uint32_t horo_test_load_count(void) {
    return loadCount;
}

#if HORO_ABI_FIXTURE_MODE != 0
/** @brief Return a valid or deliberately incompatible inert requirements table. */
HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_query(HoroExtensionRequirements *requirements) {
    *requirements = (HoroExtensionRequirements){.structSize = sizeof(HoroExtensionRequirements),
                                                .abiMajorVersion = HORO_EXTENSION_ABI_VERSION,
                                                .minimumHostMinor = HORO_ABI_FIXTURE_MODE == 2 ? 99 : 0,
                                                .requiredHostApiSize = offsetof(HoroExtensionHostApi, abiMinorVersion),
                                                .requiredFunctions =
                                                    HORO_ABI_FIXTURE_MODE == 4 || HORO_ABI_FIXTURE_MODE == 5 || HORO_ABI_FIXTURE_MODE == 8
                                                        ? HORO_EXTENSION_REQUIRES_ASSET_IMPORTER
                                                        : 0};
    return HORO_EXTENSION_SUCCESS;
}
#endif

/** @brief Return a legacy module prefix or a deliberately truncated result. */
HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_load(const HoroExtensionHostApi *host, HoroExtensionModuleApi *outModule) {
    ++loadCount;
#if HORO_ABI_FIXTURE_MODE == 4 || HORO_ABI_FIXTURE_MODE == 5 || HORO_ABI_FIXTURE_MODE == 8
    const HoroExtensionStatus registrationStatus = RegisterFixtureImporters(host);
    if (registrationStatus != HORO_EXTENSION_SUCCESS)
        return registrationStatus;
#else
    (void)host;
#endif
    if (HORO_ABI_FIXTURE_MODE == 6)
        return HORO_EXTENSION_ERROR_INIT_FAILED;
    if (HORO_ABI_FIXTURE_MODE == 7) {
        static const char invalidIdentity[] = "invalid";
        *outModule = (HoroExtensionModuleApi){.structSize = sizeof(HoroExtensionModuleApi),
                                              .moduleId = {invalidIdentity, 257},
                                              .moduleVersion = {invalidIdentity, sizeof(invalidIdentity) - 1}};
        return HORO_EXTENSION_SUCCESS;
    }
    outModule->structSize = HORO_ABI_FIXTURE_MODE == 3 ? 1 : offsetof(HoroExtensionModuleApi, moduleId);
    return HORO_EXTENSION_SUCCESS;
}

/** @brief Accept one deterministic host shutdown callback for conformance coverage. */
HORO_EXTENSION_EXPORT void horo_extension_unload(HoroExtensionModuleApi *module) {
    *module = (HoroExtensionModuleApi){0};
}
