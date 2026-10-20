#include "PCGCanvasTestSupport.h"
#include "editor/screens/workspace/PCGPreviewView.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <cmath>
#include <thread>

namespace Horo::Editor {
    using namespace PCGCanvasTestSupport;

    /** @brief Exercises actual point/bounds draw primitives under the finite render budget. */
    void CheckPreviewDrawing(const CanvasWorkspace &fixture) {
        Tests::EditorGuiContextFixture gui;
        for (const auto preset : {Theme::Preset::HoroDark, Theme::Preset::HoroLight}) {
            Theme::SetThemePreset(preset);
            for (const auto locale : {"en-US", "tr-TR"}) {
                REQUIRE(gui.localization.Prepare(LocaleTag{locale}));
                REQUIRE(gui.localization.ActivatePrepared());
                for (const float width : {280.0F, 1000.0F}) {
                    gui.imgui.BeginFrame();
                    ImGui::SetNextWindowSize({width, 450});
                    ImGui::Begin("BoundedPCGPreview");
                    CHECK(DrawPCGPreview(fixture.View().preview, fixture.View().previewRegion, 4, gui.localization, gui.imgui.Fonts()) ==
                          PCGPreviewAction::None);
                    ImGui::End();
                    gui.imgui.EndFrame();
                    const auto *draw = ImGui::GetDrawData();
                    REQUIRE(draw);
                    // Four point/bounds primitives plus shared controls/text, independent of the 256-point candidate.
                    CHECK(draw->TotalVtxCount < 12000);
                    for (int list = 0; list < draw->CmdListsCount; ++list)
                        for (const auto &vertex : draw->CmdLists[list]->VtxBuffer) {
                            CHECK(std::isfinite(vertex.pos.x));
                            CHECK(std::isfinite(vertex.pos.y));
                        }
                }
            }
        }
        Theme::SetThemePreset(Theme::Preset::HoroDark);
    }

    TEST_CASE("Registered PCG preview evaluates real unsaved source and revokes overlays through canonical commands",
              "[unit][editor][pcg][preview][workspace]") {
        CanvasWorkspace fixture{true};
        REQUIRE(fixture.View().previewAvailable);
        const auto before = fixture.View().snapshot;
        fixture.Send(fixture.Request(PCGEditorAction::PreviewRefresh));
        REQUIRE_FALSE(fixture.View().error);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (fixture.View().preview.phase == PCGPreviewPhase::Evaluating && std::chrono::steady_clock::now() < deadline) {
            fixture.controller->UpdatePCGPreviews();
            std::this_thread::yield();
        }
        REQUIRE(fixture.View().preview.phase == PCGPreviewPhase::Ready);
        REQUIRE(fixture.View().preview.overlay);
        REQUIRE(fixture.View().preview.overlay->candidate.Outputs().size() == 1);
        CHECK(fixture.View().preview.overlay->candidate.Outputs().front().points->PointCount() == 256);
        fixture.Unchanged(before);
        CHECK_FALSE(fixture.controller->ViewModel().isDirty);
        SECTION("source publication revokes output") {
            REQUIRE(fixture.controller->ExecutePCGDocument(before.identity.instance, Edit(before, 82)).HasValue());
            CHECK_FALSE(fixture.View().preview.overlay);
            CHECK(fixture.View().preview.phase == PCGPreviewPhase::Cancelled);
        }
        SECTION("provider replacement revokes output") {
            REQUIRE(fixture.controller->ReplacePCGPreviewInputs(CreatePCGPreviewSandbox().Value()).HasValue());
            CHECK_FALSE(fixture.View().preview.overlay);
            fixture.Unchanged(before);
        }
        SECTION("explicit clear revokes output") {
            fixture.Send(fixture.Request(PCGEditorAction::PreviewCancel));
            CHECK_FALSE(fixture.View().preview.overlay);
            fixture.Unchanged(before);
        }
        SECTION("reopen cannot inherit prior overlay") {
            REQUIRE(fixture.controller->ClosePCGDocument(before.identity.instance).HasValue());
            fixture.Open();
            CHECK(fixture.View().snapshot.session != before.session);
            CHECK_FALSE(fixture.View().preview.overlay);
        }
        SECTION("actual overlay draw stays within the requested budget in both themes and locales") {
            CheckPreviewDrawing(fixture);
        }
        REQUIRE(fixture.controller->ShutdownPCGDocuments(true).HasValue());
        CHECK(fixture.controller->ViewModel().pcgDocuments.empty());
    }

}  // namespace Horo::Editor
