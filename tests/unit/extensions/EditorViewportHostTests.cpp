#include "../../helpers/extensions/ViewportContributionTestSupport.h"
#include "AllocationProbe.h"
#include "Horo/Extensions/EditorActivityHost.h"
#include "Horo/Extensions/EditorViewportAbi.h"
#include "Horo/Extensions/EditorViewportHost.h"
#include "Horo/Extensions/ExtensionManager.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "OwnedTestDirectory.h"
#include "SecurityTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <limits>

namespace Horo::Extensions::Tests {
    namespace {
        struct ViewportPackage final {
            Horo::Tests::OwnedTestDirectory directory{"horo111 native viewport"};
            const std::filesystem::path root = directory.Path();

            ViewportPackage() {
                const std::filesystem::path library{HORO_EDITOR_VIEWPORT_FIXTURE};
                std::filesystem::copy_file(library, root / library.filename());
                std::ofstream manifest{root / "extension.json"};
                manifest
                    << R"({"id":"fixture.package","version":"1.0.0","modules":[{"id":"fixture.module","version":"1.0.0","kind":"native","roles":["editor-presentation"],"entry":")"
                    << library.filename().string()
                    << R"(","abi":{"major":1,"minimumMinor":7},"requiredCapabilities":["editor.viewport"]}],"contributions":[{"type":"editor.gizmo","id":"fixture.gizmo","module":"fixture.module"}]})";
            }
        };

        ExtensionAdmissionPolicy ViewportPolicy() {
            return {.revision = 1,
                    .knownPermissions = {{"editor.gizmo"}, {"editor.viewport-overlay"}},
                    .approvedPermissions = {{"editor.gizmo"}, {"editor.viewport-overlay"}},
                    .availableCapabilities = {EditorGizmoCapability, EditorViewportOverlayCapability}};
        }

        struct ViewportHost final {
            JobSystem jobs;
            std::shared_ptr<EditorActivityHost> activities = std::make_shared<EditorActivityHost>(jobs);
            std::shared_ptr<EditorViewportHost> viewports = std::make_shared<EditorViewportHost>(activities->Registry(), ViewportPolicy());
            ExtensionManager manager{nullptr,
                                     ExtensionHostProfile::Interactive,
                                     {HORO_EDITOR_VIEWPORT_HOST_CAPABILITY},
                                     Horo::Tests::CreateAcceptingArtifactGate(),
                                     {},
                                     {},
                                     {.activities = activities, .viewports = viewports}};
        };
    }  // namespace

    TEST_CASE("Viewport native publication owns copied geometry and revokes stale picking on unload", "[Extensions][Viewport][ABI]") {
        ViewportPackage package;
        ViewportHost host;
        const auto loaded = host.manager.LoadExtension(package.root.string());
        if (loaded.HasError())
            UNSCOPED_INFO(loaded.ErrorValue().message);
        REQUIRE(loaded.HasValue());
        EditorViewportFrame frame;
        host.viewports->Extract(frame, EditorViewportTool::Move, true, true);
        REQUIRE(frame.count == 1);
        CHECK(frame.lines[0].line.x1 == 0.3F);
        const auto pick = host.viewports->Pick(frame, 0.2F, 0.7F, 400, 280);
        REQUIRE(pick);
        CHECK(pick.axis == 0);
        CHECK(host.viewports->IsLive(pick, EditorViewportTool::Move));
        CHECK_FALSE(host.viewports->IsLive(pick, EditorViewportTool::Rotate));
        CHECK_FALSE(host.viewports->Pick(frame, 0.2F, 0.6F, 400, 280));
        CHECK_FALSE(host.viewports->Pick(frame, std::numeric_limits<float>::quiet_NaN(), 0.7F, 400, 280));
        CHECK(host.activities->Registry().Save().surfaces.empty());
        host.viewports->Extract(frame, EditorViewportTool::Move, false, true);
        CHECK(frame.count == 0);
        host.viewports->Extract(frame, EditorViewportTool::Rotate, true, true);
        CHECK(frame.count == 0);
        host.viewports->Extract(frame, EditorViewportTool::Move, true, false);
        CHECK(frame.count == 0);
        const EditorSurfaceProviderIdentity provider{"fixture.package", "fixture.module", 1};
        REQUIRE(host.viewports->SetVisible(provider, "fixture.gizmo", false).HasValue());
        CHECK_FALSE(host.viewports->IsLive(pick, EditorViewportTool::Move));
        host.viewports->Extract(frame, EditorViewportTool::Move, true, true);
        CHECK(frame.count == 0);
        REQUIRE(host.viewports->SetVisible(provider, "fixture.gizmo", true).HasValue());
        host.viewports->Extract(frame, EditorViewportTool::Move, true, true);
        REQUIRE(frame.count == 1);
        host.manager.UnloadExtension("fixture.package");
        CHECK_FALSE(host.viewports->IsLive(pick, EditorViewportTool::Move));
        CHECK_FALSE(host.viewports->Pick(frame, 0.2F, 0.7F, 400, 280));
        REQUIRE(host.manager.LoadExtension(package.root.string()).HasValue());
        CHECK_FALSE(host.viewports->IsLive(pick, EditorViewportTool::Move));
        CHECK(host.viewports->SetVisible(provider, "fixture.gizmo", false).HasError());
        host.viewports->BeginShutdown();
        host.viewports->Extract(frame, EditorViewportTool::Move, true, true);
        CHECK(frame.count == 0);
    }

    TEST_CASE("Viewport staging rejects incompatible ABI malformed geometry and failed activation atomically",
              "[Extensions][Viewport][ABI]") {
        ViewportPackage package;
        ViewportHost host;
        auto library =
            Platform::LoadDynamicLibrary((package.root / std::filesystem::path{HORO_EDITOR_VIEWPORT_FIXTURE}.filename()).string());
        REQUIRE(library.HasValue());
        const auto mode = reinterpret_cast<void (*)(uint32_t)>(library.Value()->GetSymbol("horo_test_viewport_mode"));
        REQUIRE(mode);
        for (const uint32_t value : {1U, 2U, 3U}) {
            mode(value);
            CHECK(host.manager.LoadExtension(package.root.string()).HasError());
            EditorViewportFrame frame;
            host.viewports->Extract(frame, EditorViewportTool::Move, true, true);
            CHECK(frame.count == 0);
        }
        mode(0);
        REQUIRE(host.manager.LoadExtension(package.root.string()).HasValue());
    }

    TEST_CASE("Viewport transport requires a permissioned interactive composition", "[Extensions][Viewport][ABI]") {
        ViewportPackage package;
        ExtensionManager missing{nullptr,
                                 ExtensionHostProfile::Interactive,
                                 {HORO_EDITOR_VIEWPORT_HOST_CAPABILITY},
                                 Horo::Tests::CreateAcceptingArtifactGate()};
        CHECK(missing.LoadExtension(package.root.string()).HasError());
        ViewportHost denied;
        auto policy = ViewportPolicy();
        policy.approvedPermissions.clear();
        denied.viewports = std::make_shared<EditorViewportHost>(denied.activities->Registry(), policy);
        ExtensionManager manager{nullptr,
                                 ExtensionHostProfile::Interactive,
                                 {HORO_EDITOR_VIEWPORT_HOST_CAPABILITY},
                                 Horo::Tests::CreateAcceptingArtifactGate(),
                                 {},
                                 {},
                                 {.viewports = denied.viewports}};
        CHECK(manager.LoadExtension(package.root.string()).HasError());
    }

    TEST_CASE("Viewport frame extraction and picking allocate nothing and permission revocation closes retained picks",
              "[Extensions][Viewport][Allocation]") {
        Horo::Tests::ViewportContribution fixture;
        REQUIRE(
            fixture.Publish({"horo.scene.transform", EditorViewportTool::Move, {{0.1F, 0.7F, 0.3F, 0.7F, 2, EditorViewportTone::X, 0}}}));
        EditorViewportFrame frame;
        Horo::Tests::AllocationProbe::Measurement measured;
        bool live = true;
        {
            const Horo::Tests::AllocationProbe::ScopedMeasurement measurement;
            for (int i = 0; i < 100; ++i) {
                fixture.host.Extract(frame, EditorViewportTool::Move, true, true);
                const auto pick = fixture.host.Pick(frame, 0.2F, 0.7F, 400, 280);
                live = live && fixture.host.IsLive(pick, EditorViewportTool::Move);
            }
            measured = measurement.Snapshot();
        }
        REQUIRE(live);
        CHECK(measured.requests == 0);
        const auto pick = fixture.host.Pick(frame, 0.2F, 0.7F, 400, 280);
        fixture.admission->Revoke();
        CHECK_FALSE(fixture.host.IsLive(pick, EditorViewportTool::Move));
        fixture.host.Extract(frame, EditorViewportTool::Move, true, true);
        CHECK(frame.count == 0);
    }

    TEST_CASE("Viewport geometry budget is enforced before publication and overlays are passive", "[Extensions][Viewport]") {
        Horo::Tests::ViewportContribution fixture;
        EditorViewportDefinition definition{"", EditorViewportTool::None,
                                            std::vector<EditorViewportLine>(32,
                                                                            {0.1F, 0.7F, 0.3F, 0.7F, 1, EditorViewportTone::Neutral, -1})};
        std::vector<EditorViewportRegistration> owners;
        for (int i = 0; i < 8; ++i) {
            REQUIRE(fixture.Publish(definition, "fixture.overlay" + std::to_string(i), true));
            owners.push_back(std::move(*fixture.registration));
        }
        CHECK_FALSE(fixture.Publish(definition, "fixture.overflow", true));
        EditorViewportFrame frame;
        fixture.host.Extract(frame, EditorViewportTool::None, false, true);
        CHECK(frame.count == 256);
        CHECK_FALSE(fixture.host.Pick(frame, 0.2F, 0.7F, 400, 280));
        owners.front().Reset();
        REQUIRE(fixture.Publish(definition, "fixture.replacement", true));
        definition.lines[0].x0 = std::numeric_limits<float>::infinity();
        CHECK_FALSE(fixture.Publish(definition, "fixture.nonfinite", true));
        definition.lines[0].x0 = 0.1F;
        definition.lines[0].axis = 0;
        CHECK_FALSE(fixture.Publish(definition, "fixture.passive", true));
    }

}  // namespace Horo::Extensions::Tests
