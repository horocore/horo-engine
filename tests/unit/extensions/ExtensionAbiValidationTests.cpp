#include "ExtensionAbiValidation.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace Horo::Extensions::Tests {
    namespace {
        HoroExtensionStatus RegisterUnused(void *, const HoroAssetImporterDescriptor *) {
            return HORO_EXTENSION_SUCCESS;
        }

        HoroExtensionStatus RegisterProviderUnused(void *, const HoroPlatformServicesProviderDescriptor *) {
            return HORO_EXTENSION_SUCCESS;
        }

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
                                  .registerAssetImporter = RegisterUnused,
                                  .abiMinorVersion = HORO_EXTENSION_ABI_MINOR_VERSION};
        CHECK(NegotiateModuleAbi(nullptr, host) == HORO_EXTENSION_SUCCESS);
        CHECK(NegotiateModuleAbi(QueryLegacy, host) == HORO_EXTENSION_SUCCESS);
        CHECK(NegotiateModuleAbi(QueryV11, host) == HORO_EXTENSION_SUCCESS);
        CHECK(NegotiateModuleAbi(QueryCurrent, host) == HORO_EXTENSION_SUCCESS);
        CHECK(NegotiateModuleAbi(QueryProvider, host) == HORO_EXTENSION_ERROR_INVALID_ARGS);
        host.registerPlatformServicesProvider = RegisterProviderUnused;
        CHECK(NegotiateModuleAbi(QueryProvider, host) == HORO_EXTENSION_SUCCESS);
        host.registerPlatformServicesProvider = nullptr;
        host.registerAssetImporter = nullptr;
        CHECK(NegotiateModuleAbi(QueryCurrent, host) == HORO_EXTENSION_ERROR_INVALID_ARGS);
    }

    TEST_CASE("ABI negotiation rejects malformed requirements before load", "[Extensions][ABI]") {
        const HoroExtensionHostApi host{.structSize = sizeof(HoroExtensionHostApi),
                                        .abiVersion = HORO_EXTENSION_ABI_VERSION,
                                        .registerAssetImporter = RegisterUnused,
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
            r->requiredFunctions = 4;
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
