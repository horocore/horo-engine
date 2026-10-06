#include "ExtensionAbiValidation.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>

// Real C ABI test endpoints; these are not exported SDK entry points.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(hidden)
#endif
extern "C" {
HoroExtensionStatus HoroTestRegisterUnused(void *, const HoroAssetImporterDescriptor *) noexcept {
    return HORO_EXTENSION_SUCCESS;
}

HoroExtensionStatus HoroTestRegisterProviderUnused(void *, const HoroPlatformServicesProviderDescriptor *) noexcept {
    return HORO_EXTENSION_SUCCESS;
}

HoroExtensionStatus HoroTestRegisterActivityUnused(void *, const HoroEditorActivityDescriptor *, HoroEditorActivitySessionApi *) noexcept {
    return HORO_EXTENSION_SUCCESS;
}
}
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

namespace Horo::Extensions::Tests {
    namespace {
        static_assert(std::is_convertible_v<decltype(&HoroTestRegisterUnused), decltype(HoroExtensionHostApi::registerAssetImporter)>);
        static_assert(std::is_convertible_v<decltype(&HoroTestRegisterProviderUnused),
                                            decltype(HoroExtensionHostApi::registerPlatformServicesProvider)>);
        static_assert(
            std::is_convertible_v<decltype(&HoroTestRegisterActivityUnused), decltype(HoroExtensionHostApi::registerEditorActivity)>);

        HoroExtensionStatus QueryLegacy(HoroExtensionRequirements *requirements) {
            *requirements = {.structSize = sizeof(HoroExtensionRequirements),
                             .abiMajorVersion = HORO_EXTENSION_ABI_VERSION,
                             .minimumHostMinor = 0,
                             .requiredHostApiSize = offsetof(HoroExtensionHostApi, abiMinorVersion),
                             .requiredFunctions = HORO_EXTENSION_REQUIRES_ASSET_IMPORTER};
            return HORO_EXTENSION_SUCCESS;
        }

        HoroExtensionStatus QueryCurrent(HoroExtensionRequirements *requirements) {
            QueryLegacy(requirements);
            requirements->minimumHostMinor = HORO_EXTENSION_ABI_MINOR_VERSION;
            requirements->requiredHostApiSize = sizeof(HoroExtensionHostApi);
            return HORO_EXTENSION_SUCCESS;
        }

        HoroExtensionStatus QueryV11(HoroExtensionRequirements *requirements) {
            QueryLegacy(requirements);
            requirements->minimumHostMinor = 1;
            requirements->requiredHostApiSize = offsetof(HoroExtensionHostApi, registerPlatformServicesProvider);
            return HORO_EXTENSION_SUCCESS;
        }

        HoroExtensionStatus QueryProvider(HoroExtensionRequirements *requirements) {
            QueryCurrent(requirements);
            requirements->requiredFunctions = HORO_EXTENSION_REQUIRES_PLATFORM_PROVIDER;
            return HORO_EXTENSION_SUCCESS;
        }
    }  // namespace

    TEST_CASE("ABI negotiation accepts compatible minors and requires requested functions", "[Extensions][ABI]") {
        HoroExtensionHostApi host{.structSize = sizeof(HoroExtensionHostApi),
                                  .abiVersion = HORO_EXTENSION_ABI_VERSION,
                                  .registerAssetImporter = HoroTestRegisterUnused,
                                  .abiMinorVersion = HORO_EXTENSION_ABI_MINOR_VERSION};
        CHECK(NegotiateModuleAbi(nullptr, host) == HORO_EXTENSION_SUCCESS);
        CHECK(NegotiateModuleAbi(QueryLegacy, host) == HORO_EXTENSION_SUCCESS);
        CHECK(NegotiateModuleAbi(QueryV11, host) == HORO_EXTENSION_SUCCESS);
        CHECK(NegotiateModuleAbi(QueryCurrent, host) == HORO_EXTENSION_SUCCESS);
        CHECK(NegotiateModuleAbi(QueryProvider, host) == HORO_EXTENSION_ERROR_INVALID_ARGS);
        host.registerPlatformServicesProvider = HoroTestRegisterProviderUnused;
        CHECK(NegotiateModuleAbi(QueryProvider, host) == HORO_EXTENSION_SUCCESS);
        host.registerPlatformServicesProvider = nullptr;
        host.registerAssetImporter = nullptr;
        CHECK(NegotiateModuleAbi(QueryCurrent, host) == HORO_EXTENSION_ERROR_INVALID_ARGS);
    }

    TEST_CASE("ABI negotiation rejects malformed requirements before load", "[Extensions][ABI]") {
        const HoroExtensionHostApi host{.structSize = sizeof(HoroExtensionHostApi),
                                        .abiVersion = HORO_EXTENSION_ABI_VERSION,
                                        .registerAssetImporter = HoroTestRegisterUnused,
                                        .abiMinorVersion = HORO_EXTENSION_ABI_MINOR_VERSION};
        const std::array<HoroExtensionQueryFunc, 6> invalid{[](HoroExtensionRequirements *r) -> HoroExtensionStatus {
            QueryLegacy(r);
            --r->structSize;
            return HORO_EXTENSION_SUCCESS;
        }, [](HoroExtensionRequirements *r) -> HoroExtensionStatus {
            QueryLegacy(r);
            ++r->abiMajorVersion;
            return HORO_EXTENSION_SUCCESS;
        }, [](HoroExtensionRequirements *r) -> HoroExtensionStatus {
            QueryCurrent(r);
            ++r->minimumHostMinor;
            return HORO_EXTENSION_SUCCESS;
        }, [](HoroExtensionRequirements *r) -> HoroExtensionStatus {
            QueryCurrent(r);
            ++r->requiredHostApiSize;
            return HORO_EXTENSION_SUCCESS;
        }, [](HoroExtensionRequirements *r) -> HoroExtensionStatus {
            QueryLegacy(r);
            r->reserved = 1;
            return HORO_EXTENSION_SUCCESS;
        }, [](HoroExtensionRequirements *r) -> HoroExtensionStatus {
            QueryLegacy(r);
            r->requiredFunctions = 8;
            return HORO_EXTENSION_SUCCESS;
        }};
        for (const auto query : invalid)
            CHECK(NegotiateModuleAbi(query, host) == HORO_EXTENSION_ERROR_VERSION_MISMATCH);
        CHECK(NegotiateModuleAbi([](HoroExtensionRequirements *) -> HoroExtensionStatus {
            return HORO_EXTENSION_ERROR_CANCELLED;
        }, host) == HORO_EXTENSION_ERROR_CANCELLED);
        CHECK(NegotiateModuleAbi([](HoroExtensionRequirements *) -> HoroExtensionStatus {
            throw std::runtime_error("invalid native callback");
        }, host) == HORO_EXTENSION_ERROR_INIT_FAILED);
    }

    TEST_CASE("ABI 1.4 keeps the 1.3 prefix valid and rejects editor transport without exact negotiation", "[Extensions][ABI][Activity]") {
        HoroExtensionHostApi oldHost{.structSize = offsetof(HoroExtensionHostApi, registerEditorActivity),
                                     .abiVersion = 1,
                                     .registerAssetImporter = HoroTestRegisterUnused,
                                     .abiMinorVersion = 3,
                                     .registerPlatformServicesProvider = HoroTestRegisterProviderUnused};
        const HoroExtensionQueryFunc oldQuery = [](HoroExtensionRequirements *requirements) -> HoroExtensionStatus {
            QueryProvider(requirements);
            requirements->minimumHostMinor = 3;
            requirements->requiredHostApiSize = offsetof(HoroExtensionHostApi, registerEditorActivity);
            return HORO_EXTENSION_SUCCESS;
        };
        const HoroExtensionQueryFunc editorQuery = [](HoroExtensionRequirements *requirements) -> HoroExtensionStatus {
            QueryCurrent(requirements);
            requirements->requiredFunctions = HORO_EXTENSION_REQUIRES_EDITOR_ACTIVITY;
            return HORO_EXTENSION_SUCCESS;
        };
        CHECK(NegotiateModuleAbi(oldQuery, oldHost) == HORO_EXTENSION_SUCCESS);
        CHECK(NegotiateModuleAbi(editorQuery, oldHost) == HORO_EXTENSION_ERROR_VERSION_MISMATCH);
        auto current = oldHost;
        current.structSize = sizeof(HoroExtensionHostApi);
        current.abiMinorVersion = 4;
        CHECK(NegotiateModuleAbi(editorQuery, current) == HORO_EXTENSION_ERROR_INVALID_ARGS);
        current.registerEditorActivity = HoroTestRegisterActivityUnused;
        CHECK(NegotiateModuleAbi(editorQuery, current) == HORO_EXTENSION_SUCCESS);
        CHECK(NegotiateModuleAbi(oldQuery, current) == HORO_EXTENSION_SUCCESS);
    }

    TEST_CASE("Module result sizes accept complete legacy prefixes only", "[Extensions][ABI]") {
        for (std::uint32_t size = 0; size <= sizeof(HoroExtensionModuleApi) + 1; ++size) {
            HoroExtensionModuleApi moduleApi{.structSize = size};
            const bool expected = size == offsetof(HoroExtensionModuleApi, moduleId) ||
                                  size == offsetof(HoroExtensionModuleApi, moduleVersion) || size == sizeof(HoroExtensionModuleApi);
            CHECK(NormalizeModuleApi(moduleApi) == expected);
        }
    }

    TEST_CASE("Absent legacy identity fields never participate in validation", "[Extensions][ABI]") {
        int context = 0;
        HoroExtensionModuleApi moduleApi{.structSize = offsetof(HoroExtensionModuleApi, moduleId),
                                         .moduleContext = &context,
                                         .moduleId = {"ignored", 7},
                                         .moduleVersion = {"ignored", 7}};
        REQUIRE(NormalizeModuleApi(moduleApi));
        CHECK(moduleApi.moduleContext == &context);
        CHECK(moduleApi.moduleId.data == nullptr);
        CHECK(moduleApi.moduleId.length == 0);
        CHECK(moduleApi.moduleVersion.data == nullptr);
        CHECK(moduleApi.moduleVersion.length == 0);

        moduleApi = {.structSize = offsetof(HoroExtensionModuleApi, moduleVersion),
                     .moduleId = {"retained", 8},
                     .moduleVersion = {"ignored", 7}};
        REQUIRE(NormalizeModuleApi(moduleApi));
        CHECK(moduleApi.moduleId.length == 8);
        CHECK(moduleApi.moduleVersion.data == nullptr);
        CHECK(moduleApi.moduleVersion.length == 0);
    }
}  // namespace Horo::Extensions::Tests
