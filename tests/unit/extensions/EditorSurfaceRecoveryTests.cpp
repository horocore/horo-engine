#include "EditorSurfaceRegistryTestSupport.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Extensions::Tests {
    using namespace RegistrySupport;

    TEST_CASE("Unavailable workspace intent survives repeated restores without activation authority",
              "[Extensions][EditorSurface][Recovery]") {
        EditorSurfaceRegistry registry;
        const EditorSurfaceWorkspaceState saved{.surfaces = {{.surfaceId = "com.example.tools.inspector",
                                                              .provider = Provider(),
                                                              .open = true,
                                                              .focused = true,
                                                              .opaqueState = {0, 255, 7}}}};
        for (int iteration = 0; iteration < 4; ++iteration) {
            REQUIRE(registry.Restore(saved).HasValue());
            const auto recovery = registry.RecoverySnapshot();
            REQUIRE(recovery.size() == 1);
            CHECK(recovery.front().intent.surfaceId == saved.surfaces.front().surfaceId);
            CHECK(recovery.front().intent.provider == Provider());
            CHECK(recovery.front().intent.opaqueState == saved.surfaces.front().opaqueState);
            CHECK(recovery.front().status == EditorSurfaceProviderStatus::Missing);
            CHECK(registry.Snapshot().empty());
            CHECK(registry.Open(saved.surfaces.front().surfaceId).HasError());
            CHECK(registry.Save().surfaces.front().opaqueState == saved.surfaces.front().opaqueState);
        }
    }

    TEST_CASE("Each host availability reason remains identifiable and cannot open an absent surface",
              "[Extensions][EditorSurface][Recovery]") {
        EditorSurfaceRegistry registry;
        const EditorSurfaceWorkspaceState saved{
            .surfaces = {{.surfaceId = "com.example.tools.inspector", .provider = Provider(), .open = true}}};
        REQUIRE(registry.Restore(saved).HasValue());
        for (const auto status : {EditorSurfaceProviderStatus::Disabled, EditorSurfaceProviderStatus::Incompatible,
                                  EditorSurfaceProviderStatus::Failed, EditorSurfaceProviderStatus::Missing}) {
            REQUIRE(registry.SetProviderStatus(Provider(), status).HasValue());
            REQUIRE(registry.RecoverySnapshot().size() == 1);
            CHECK(registry.RecoverySnapshot().front().status == status);
            CHECK(registry.Open(saved.surfaces.front().surfaceId).HasError());
            CHECK(registry.Save().surfaces.front().open);
        }
        REQUIRE(registry.SetExtensionProviderStatus(Provider().extensionId, EditorSurfaceProviderStatus::Active).HasValue());
        CHECK(registry.RecoverySnapshot().front().status == EditorSurfaceProviderStatus::Missing);
    }

    TEST_CASE("Real provider reinstallation recovers intent only through a new valid registration",
              "[Extensions][EditorSurface][Recovery]") {
        EditorSurfaceRegistry registry;
        auto admission = Admission();
        EditorSurfaceContextProvider provider;
        auto registration = RegisterSurface(registry, provider, admission);
        OpenWithOpaqueState(registry, registration.Id(), 29);
        registration.Reset();
        REQUIRE(registry.RecoverySnapshot().size() == 1);
        REQUIRE(registry.SetProviderStatus(Provider(), EditorSurfaceProviderStatus::Failed).HasValue());
        auto returned = RegisterSurface(registry, provider, admission);
        CHECK_FALSE(registry.Snapshot().front().open);
        REQUIRE(registry.SetExtensionProviderStatus(Provider().extensionId, EditorSurfaceProviderStatus::Active).HasValue());
        CHECK(registry.RecoverySnapshot().empty());
        CHECK(registry.Snapshot().front().open);
        CHECK(registry.Snapshot().front().opaqueState == std::vector<std::uint8_t>{29});
        returned.Reset();
        REQUIRE(registry.Close("com.example.tools.inspector").HasValue());
        CHECK_FALSE(registry.Save().surfaces.front().open);
        CHECK(registry.Save().surfaces.front().opaqueState == std::vector<std::uint8_t>{29});
    }

    TEST_CASE("Malformed and excessive recovery intent cannot replace the previous workspace", "[Extensions][EditorSurface][Recovery]") {
        EditorSurfaceRegistry registry(
            {.maximumSurfaces = 1, .maximumWorkspaceEntries = 1, .maximumOpaqueStateBytes = 2, .maximumTotalStateBytes = 2});
        EditorSurfaceWorkspaceState saved{
            .surfaces = {{.surfaceId = "com.example.tools.inspector", .provider = Provider(), .open = true, .opaqueState = {1, 2}}}};
        REQUIRE(registry.Restore(saved).HasValue());
        auto invalid = saved;
        invalid.surfaces.push_back(saved.surfaces.front());
        CHECK(registry.Restore(invalid).HasError());
        invalid = saved;
        invalid.surfaces.front().opaqueState.push_back(3);
        CHECK(registry.Restore(invalid).HasError());
        invalid = saved;
        invalid.surfaces.front().provider.extensionId = "../untrusted";
        CHECK(registry.Restore(invalid).HasError());
        CHECK(registry.Save().surfaces.front().opaqueState == saved.surfaces.front().opaqueState);
        REQUIRE(registry.SetProviderStatus(Provider(), EditorSurfaceProviderStatus::Disabled).HasValue());
        CHECK(registry.SetProviderStatus({"another.package", "another.module"}, EditorSurfaceProviderStatus::Failed).HasError());
        registry.BeginShutdown();
        CHECK(registry.RecoverySnapshot().empty());
        CHECK(registry.SetExtensionProviderStatus(Provider().extensionId, EditorSurfaceProviderStatus::Active).HasError());
    }
}  // namespace Horo::Extensions::Tests
