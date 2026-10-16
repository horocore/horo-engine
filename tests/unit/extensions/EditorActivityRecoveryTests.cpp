#include "EditorActivityPackageSupport.h"
#include "Horo/Extensions/EditorActivityAbi.h"
#include "Horo/Extensions/EditorActivityHost.h"
#include "Horo/Extensions/ExtensionManager.h"
#include "Horo/Foundation/JobSystem.h"
#include "SecurityTestSupport.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Extensions::Tests {
    TEST_CASE("Real native provider failure and reinstallation retain paired workspace state",
              "[Extensions][EditorSurface][Recovery][ABI]") {
        Horo::Tests::EditorActivityPackage package;
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 8}};
        auto host = std::make_shared<EditorActivityHost>(jobs);
        ExtensionManager manager{nullptr,
                                 ExtensionHostProfile::Interactive,
                                 {HORO_EDITOR_ACTIVITY_HOST_CAPABILITY},
                                 Horo::Tests::CreateAcceptingArtifactGate(),
                                 {},
                                 {},
                                 host};
        REQUIRE(manager.LoadExtension(package.root.string()).HasValue());
        host->Update();
        REQUIRE(host->Prepared().size() == 1);
        const auto provider = host->Prepared().front().surface.descriptor.provider;
        REQUIRE(host->Registry().ToggleActivity(provider, "fixture.activity").HasValue());
        const std::array<std::uint8_t, 3> bytes{0, 255, 9};
        REQUIRE(host->Registry().SetOpaqueState("fixture.drawer", bytes).HasValue());
        manager.UnloadExtension("fixture.package");
        host->Update();
        REQUIRE(host->Registry().RecoverySnapshot().size() == 2);
        CHECK(host->Prepared().empty());
        package.Icon("invalid svg");
        REQUIRE(manager.LoadExtension(package.root.string()).HasError());
        for (const auto &entry : host->Registry().RecoverySnapshot())
            CHECK(entry.status == EditorSurfaceProviderStatus::Failed);
        package.Icon(
            R"(<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48" viewBox="0 0 48 48"><path fill="#ffffff" d="M4 4 H44 V44 H4 Z"/></svg>)");
        REQUIRE(manager.LoadExtension(package.root.string()).HasValue());
        host->Update();
        CHECK(host->Registry().RecoverySnapshot().empty());
        REQUIRE(host->Prepared().size() == 1);
        CHECK(host->Prepared().front().surface.open);
        const auto saved = host->Registry().Save();
        const auto drawer = std::ranges::find(saved.surfaces, std::string{"fixture.drawer"}, &EditorSurfaceWorkspaceEntry::surfaceId);
        REQUIRE(drawer != saved.surfaces.end());
        CHECK(drawer->opaqueState == std::vector<std::uint8_t>{0, 255, 9});
        manager.UnloadExtension("fixture.package");
        REQUIRE(host->Registry().CloseProviderWorkspace({"fixture.package", "fixture.module"}).HasValue());
        for (const auto &intent : host->Registry().Save().surfaces)
            CHECK_FALSE(intent.open);
        REQUIRE(manager.LoadExtension(package.root.string()).HasValue());
        host->Update();
        REQUIRE(host->Prepared().size() == 1);
        CHECK_FALSE(host->Prepared().front().surface.open);
    }
}  // namespace Horo::Extensions::Tests
