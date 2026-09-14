#include "Horo/Extensions/ExtensionAbi.h"

#include <cstdint>
#include <stdexcept>

namespace {
    std::int32_t expectedDestroyOrder = 2;
    std::int32_t contexts[]{1, 2};

    HoroExtensionStatus ImportAsset(void *, const HoroAssetImportRequest *, HoroAssetImportResponse *) {
        return HORO_EXTENSION_SUCCESS;
    }

    void DestroyImporter(void *context) {
        if (HORO_ABI_CLEANUP_FIXTURE_MODE == 1)
            throw std::runtime_error{"cleanup failure"};
        if (*static_cast<std::int32_t *>(context) != expectedDestroyOrder)
            throw std::logic_error{"registration cleanup order violated"};
        --expectedDestroyOrder;
    }
}  // namespace

/** @brief Register two owned callbacks so the harness must destroy them in reverse order. */
HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_load(const HoroExtensionHostApi *host, HoroExtensionModuleApi *outModule) {
    static constexpr char contributionId[] = "fixture.cleanup";
    static constexpr char contributionVersion[] = "1.0.0";
    for (auto &context : contexts) {
        const HoroAssetImporterDescriptor importer{
            .structSize = sizeof(HoroAssetImporterDescriptor),
            .abiVersion = HORO_ASSET_IMPORTER_ABI_VERSION,
            .contributionId = {contributionId, sizeof(contributionId) - 1},
            .contributionVersion = {contributionVersion, sizeof(contributionVersion) - 1},
            .importerContext = &context,
            .importAsset = ImportAsset,
            .destroyImporter = DestroyImporter,
        };
        if (const auto status = host->registerAssetImporter(host->hostContext, &importer); status != HORO_EXTENSION_SUCCESS)
            return status;
    }
    outModule->structSize = offsetof(HoroExtensionModuleApi, moduleId);
    return HORO_EXTENSION_SUCCESS;
}

/** @brief Complete or deliberately violate the optional unload contract. */
HORO_EXTENSION_EXPORT void horo_extension_unload(HoroExtensionModuleApi *module) {
    *module = {};
    if (HORO_ABI_CLEANUP_FIXTURE_MODE == 2)
        throw std::runtime_error{"unload failure"};
}
