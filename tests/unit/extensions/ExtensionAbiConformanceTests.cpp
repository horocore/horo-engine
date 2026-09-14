#include "Horo/Extensions/ExtensionAbiConformance.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string_view>

namespace Horo::Extensions {
    TEST_CASE("ABI conformance accepts legacy and current compatible fixtures", "[Extensions][ABI][SDK]") {
        const std::array supported{HORO_ABI_FIXTURE_0, HORO_ABI_FIXTURE_1};
        for (std::size_t index = 0; index < supported.size(); ++index) {
            const auto report = RunExtensionAbiConformance(supported[index]);
            CHECK(report.Passed());
            CHECK(report.code == ExtensionAbiConformanceCode::Passed);
            CHECK(report.status == HORO_EXTENSION_SUCCESS);
            CHECK(report.queryPresent == (index == 1));
            CHECK(report.unloadPresent);
            CHECK(report.unloadInvoked);
        }
    }

    TEST_CASE("ABI conformance identifies incompatible negotiation and table shapes", "[Extensions][ABI][SDK]") {
        const auto incompatibleVersion = RunExtensionAbiConformance(HORO_ABI_FIXTURE_2);
        CHECK_FALSE(incompatibleVersion.Passed());
        CHECK(incompatibleVersion.code == ExtensionAbiConformanceCode::NegotiationRejected);
        CHECK(incompatibleVersion.status == HORO_EXTENSION_ERROR_VERSION_MISMATCH);
        CHECK_FALSE(incompatibleVersion.unloadInvoked);

        const auto malformedTable = RunExtensionAbiConformance(HORO_ABI_FIXTURE_3);
        CHECK_FALSE(malformedTable.Passed());
        CHECK(malformedTable.code == ExtensionAbiConformanceCode::ModuleTableRejected);
        CHECK(malformedTable.status == HORO_EXTENSION_SUCCESS);
        CHECK(malformedTable.unloadInvoked);
    }

    TEST_CASE("ABI conformance owns registered callbacks until deterministic cleanup", "[Extensions][ABI][SDK]") {
        const auto report = RunExtensionAbiConformance(HORO_ABI_FIXTURE_4);
        CHECK(report.Passed());
        CHECK(report.registrationCount == 1);
        CHECK(report.unloadInvoked);
    }

    TEST_CASE("ABI conformance identifies callback load and identity failures", "[Extensions][ABI][SDK]") {
        CHECK(RunExtensionAbiConformance(HORO_ABI_FIXTURE_5).code == ExtensionAbiConformanceCode::RegistrationRejected);
        CHECK(RunExtensionAbiConformance(HORO_ABI_FIXTURE_6).code == ExtensionAbiConformanceCode::LoadRejected);
        CHECK(RunExtensionAbiConformance(HORO_ABI_FIXTURE_7).code == ExtensionAbiConformanceCode::ModuleIdentityRejected);
    }

    TEST_CASE("ABI conformance identifies a missing module load entry point", "[Extensions][ABI][SDK]") {
        const auto report = RunExtensionAbiConformance(HORO_ABI_MISSING_LOAD_FIXTURE);
        CHECK(report.code == ExtensionAbiConformanceCode::MissingLoadEntryPoint);
        CHECK_FALSE(report.unloadInvoked);
    }

    TEST_CASE("ABI conformance reports missing libraries without invoking module lifecycle", "[Extensions][ABI][SDK]") {
        const auto report = RunExtensionAbiConformance("does-not-exist-horo-extension-module");
        CHECK_FALSE(report.Passed());
        CHECK(report.code == ExtensionAbiConformanceCode::LibraryLoadFailed);
        CHECK_FALSE(report.queryPresent);
        CHECK_FALSE(report.unloadPresent);
        CHECK_FALSE(report.unloadInvoked);
    }

    TEST_CASE("ABI conformance exposes stable names for every terminal outcome", "[Extensions][ABI][SDK]") {
        using enum ExtensionAbiConformanceCode;
        constexpr std::array codes{Passed,       LibraryLoadFailed,   MissingLoadEntryPoint,  NegotiationRejected, RegistrationRejected,
                                   LoadRejected, ModuleTableRejected, ModuleIdentityRejected, CleanupRejected};
        for (const auto code : codes)
            CHECK(ExtensionAbiConformanceCodeName(code).starts_with("extension.abi."));
        CHECK(ExtensionAbiConformanceCodeName(static_cast<ExtensionAbiConformanceCode>(255)) == "extension.abi.conformance.unknown");
    }
}  // namespace Horo::Extensions
