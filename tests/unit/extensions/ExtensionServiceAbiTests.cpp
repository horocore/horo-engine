#include "Horo/Extensions/ExtensionServiceAbi.h"
#include "extensions/host/ExtensionAbiValidation.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Extensions::Tests {
    namespace {
        HoroExtensionStatus Register(void *, const HoroExtensionServiceDescriptor *) {
            FAIL("negotiation invoked provider registration");
            return 1;
        }

        HoroExtensionStatus Bind(void *, HoroExtensionStringView, HoroExtensionServiceClient *) {
            FAIL("negotiation invoked an import");
            return 1;
        }

        HoroExtensionStatus ServiceQuery(HoroExtensionRequirements *requirements) {
            *requirements = {.structSize = sizeof(*requirements),
                             .abiMajorVersion = 1,
                             .minimumHostMinor = 8,
                             .requiredHostApiSize = offsetof(HoroExtensionHostApi, services) + sizeof(HoroExtensionHostApi::services),
                             .requiredFunctions = HORO_EXTENSION_REQUIRES_SERVICES};
            return HORO_EXTENSION_SUCCESS;
        }

        HoroExtensionStatus PreviousQuery(HoroExtensionRequirements *requirements) {
            *requirements = {.structSize = sizeof(*requirements),
                             .abiMajorVersion = 1,
                             .minimumHostMinor = 7,
                             .requiredHostApiSize = offsetof(HoroExtensionHostApi, services)};
            return HORO_EXTENSION_SUCCESS;
        }
    }  // namespace

    TEST_CASE("ABI1.8 service negotiation preserves genuine previous host prefixes without callbacks", "[extensions][sdk][abi]") {
        HoroExtensionServiceHostApi services{sizeof(services), 1, Register, Bind};
        HoroExtensionHostApi host{.structSize = sizeof(host), .abiVersion = 1, .abiMinorVersion = 8, .services = &services};
        CHECK(NegotiateModuleAbi(ServiceQuery, host) == HORO_EXTENSION_SUCCESS);
        CHECK(NegotiateModuleAbi(PreviousQuery, host) == HORO_EXTENSION_SUCCESS);
        host.structSize = offsetof(HoroExtensionHostApi, services);
        host.abiMinorVersion = 7;
        CHECK(NegotiateModuleAbi(PreviousQuery, host) == HORO_EXTENSION_SUCCESS);
        CHECK(NegotiateModuleAbi(ServiceQuery, host) != HORO_EXTENSION_SUCCESS);
        CHECK_FALSE(HoroExtensionHasServiceTransport(&host));
    }

    TEST_CASE("ABI1.8 service negotiation rejects incomplete and unknown service tables before module load", "[extensions][sdk][abi]") {
        HoroExtensionServiceHostApi services{sizeof(services), 1, Register, Bind};
        HoroExtensionHostApi host{.structSize = sizeof(host), .abiVersion = 1, .abiMinorVersion = 8, .services = &services};
        SECTION("missing table") {
            host.services = nullptr;
        }
        SECTION("truncated table") {
            --services.structSize;
        }
        SECTION("unknown table schema") {
            services.schemaVersion = 2;
        }
        SECTION("missing registration") {
            services.registerService = nullptr;
        }
        SECTION("missing import binding") {
            services.bindImport = nullptr;
        }
        CHECK_FALSE(HoroExtensionHasServiceTransport(&host));
        CHECK(NegotiateModuleAbi(ServiceQuery, host) != HORO_EXTENSION_SUCCESS);
    }
}  // namespace Horo::Extensions::Tests
