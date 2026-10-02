#include "Horo/Extensions/ExtensionAbi.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace {
    std::uint32_t loadCount{};
    std::uint32_t destroyCount{};
    std::uint32_t unloadCount{};
    std::uint32_t outputStatus{};

    /** @brief Emit a small valid payload or ignore the host's oversized-output rejection. */
    HoroExtensionStatus ImportAsset(void *, const HoroAssetImportRequest *, HoroAssetImportResponse *response) {
        if (HORO_ABI_HOSTILE_FIXTURE_MODE == 7)
            throw std::runtime_error{"hostile import"};
        static constexpr char type[] = "example.raw";
        response->assetType = {type, sizeof(type) - 1};
        std::uint8_t *bytes{};
        if (HORO_ABI_HOSTILE_FIXTURE_MODE == 4) {
            outputStatus = response->editorPayload.resize(response->editorPayload.context, 1024ULL * 1024ULL * 1024ULL + 1, &bytes);
            return HORO_EXTENSION_SUCCESS;
        }
        const auto status = response->editorPayload.resize(response->editorPayload.context, 1, &bytes);
        if (status == HORO_EXTENSION_SUCCESS)
            *bytes = 42;
        return status;
    }

    /** @brief Count transferred-context cleanup, including deliberate callback failure. */
    void DestroyImporter(void *) {
        ++destroyCount;
        if (HORO_ABI_HOSTILE_FIXTURE_MODE == 5)
            throw std::runtime_error{"hostile importer destroy"};
    }
}  // namespace

/** @brief Inspect lifecycle calls while the test holds a separate native-library lease. */
HORO_EXTENSION_EXPORT std::uint32_t horo_test_load_count() {
    return loadCount;
}

/** @brief Inspect importer cleanup without retaining a host contribution. */
HORO_EXTENSION_EXPORT std::uint32_t horo_test_destroy_count() {
    return destroyCount;
}

/** @brief Inspect exactly-once module unload. */
HORO_EXTENSION_EXPORT std::uint32_t horo_test_unload_count() {
    return unloadCount;
}

/** @brief Inspect the result returned by the bounded host output callback. */
HORO_EXTENSION_EXPORT std::uint32_t horo_test_output_status() {
    return outputStatus;
}

/** @brief Negotiate a current module or throw before activation begins. */
HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_query(HoroExtensionRequirements *requirements) {
    if (HORO_ABI_HOSTILE_FIXTURE_MODE == 1)
        throw std::runtime_error{"hostile query"};
    *requirements = {.structSize = sizeof(HoroExtensionRequirements),
                     .abiMajorVersion = HORO_EXTENSION_ABI_VERSION,
                     .minimumHostMinor = 0,
                     .requiredHostApiSize = offsetof(HoroExtensionHostApi, abiMinorVersion),
                     .requiredFunctions = HORO_EXTENSION_REQUIRES_ASSET_IMPORTER};
    return HORO_EXTENSION_SUCCESS;
}

/** @brief Transfer one real importer before returning valid, throwing, or mismatched activation results. */
HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_load(const HoroExtensionHostApi *host, HoroExtensionModuleApi *module) {
    ++loadCount;
    static constexpr char id[] = "fixture.importer";
    static constexpr char version[] = "1.0.0";
    static constexpr char extension[] = "raw";
    static constexpr char type[] = "example.raw";
    static constexpr char target[] = ".horoasset";
    const HoroExtensionStringView extensions[]{{extension, sizeof(extension) - 1}};
    const HoroExtensionStringView types[]{{type, sizeof(type) - 1}};
    const HoroAssetImporterDescriptor descriptor{.structSize = sizeof(HoroAssetImporterDescriptor),
                                                 .abiVersion = HORO_ASSET_IMPORTER_ABI_VERSION,
                                                 .contributionId = {id, sizeof(id) - 1},
                                                 .contributionVersion = {version, sizeof(version) - 1},
                                                 .fileExtensions = extensions,
                                                 .fileExtensionCount = 1,
                                                 .assetTypes = types,
                                                 .assetTypeCount = 1,
                                                 .targetExtension = {target, sizeof(target) - 1},
                                                 .importerContext = &destroyCount,
                                                 .importAsset = ImportAsset,
                                                 .destroyImporter = DestroyImporter};
    const auto status = host->registerAssetImporter(host->hostContext, &descriptor);
    if (status != HORO_EXTENSION_SUCCESS)
        return status;
    static constexpr char identity[] = "com.example.abi.native";
    static constexpr char mismatch[] = "com.example.wrong";
    *module = {.structSize = sizeof(HoroExtensionModuleApi),
               .moduleContext = &loadCount,
               .moduleId = HORO_ABI_HOSTILE_FIXTURE_MODE == 3 ? HoroExtensionStringView{mismatch, sizeof(mismatch) - 1}
                                                              : HoroExtensionStringView{identity, sizeof(identity) - 1},
               .moduleVersion = {version, sizeof(version) - 1}};
    if (HORO_ABI_HOSTILE_FIXTURE_MODE == 2)
        throw std::runtime_error{"hostile load after registration"};
    return HORO_ABI_HOSTILE_FIXTURE_MODE == 6 ? HORO_EXTENSION_ERROR_INIT_FAILED : HORO_EXTENSION_SUCCESS;
}

/** @brief Count unload attempts and deliberately throw after releasing module state. */
HORO_EXTENSION_EXPORT void horo_extension_unload(HoroExtensionModuleApi *module) {
    ++unloadCount;
    *module = {};
    if (HORO_ABI_HOSTILE_FIXTURE_MODE == 5 || HORO_ABI_HOSTILE_FIXTURE_MODE == 6)
        throw std::runtime_error{"hostile unload"};
}
