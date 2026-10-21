#include "PCGCanvasTestSupport.h"
#include "editor/screens/workspace/PCGPreviewView.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <cmath>
#include <thread>

namespace Horo::Editor {
    using namespace PCGCanvasTestSupport;

    namespace {
        /** @brief Refreshes through the production workspace route and waits only in the bounded test harness. */
        std::shared_ptr<const PCGPreviewOverlay> RefreshPreview(CanvasWorkspace &fixture) {
            fixture.Send(fixture.Request(PCGEditorAction::PreviewRefresh));
            REQUIRE_FALSE(fixture.View().error);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (fixture.View().preview.phase == PCGPreviewPhase::Evaluating && std::chrono::steady_clock::now() < deadline) {
                fixture.controller->UpdatePCGPreviews();
                std::this_thread::yield();
            }
            REQUIRE(fixture.View().preview.phase == PCGPreviewPhase::Ready);
            REQUIRE(fixture.View().preview.overlay);
            CHECK(fixture.View().preview.overlay->fence.documentRevision == fixture.View().snapshot.revision);
            return fixture.View().preview.overlay;
        }
    }  // namespace

    /** @brief Exercises actual point/bounds draw primitives under the finite render budget. */
    void CheckPreviewDrawing(const CanvasWorkspace &fixture) {
        Tests::EditorGuiContextFixture gui;
        ImGui::GetIO().IniFilename = nullptr;
        const auto root = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor";
        for (const auto locale : {"en-US", "tr-TR"})
            REQUIRE(gui.localization.LoadCatalogFile(root / (std::string{locale} + ".json")));
        for (const auto preset : {Theme::Preset::HoroDark, Theme::Preset::Light}) {
            Theme::SetThemePreset(preset);
            Theme::ApplyCurrentTheme();
            CHECK(ImGui::GetStyle().Colors[ImGuiCol_Text].x == Theme::GetActiveTokens().colors.textPrimary.x);
            for (const auto locale : {"en-US", "tr-TR"}) {
                REQUIRE(gui.localization.Prepare(LocaleTag{locale}));
                REQUIRE(gui.localization.ActivatePrepared());
                for (const float width : {280.0F, 1000.0F}) {
                    for (int frame = 0; frame < 2; ++frame) {
                        gui.imgui.BeginFrame();
                        ImGui::SetNextWindowPos({0, 0});
                        ImGui::SetNextWindowSize({width, 450});
                        ImGui::Begin("BoundedPCGPreview", nullptr, ImGuiWindowFlags_NoSavedSettings);
                        CHECK(DrawPCGPreview(fixture.View().preview, fixture.View().previewRegion, 4, gui.localization,
                                             gui.imgui.Fonts()) == PCGPreviewAction::None);
                        ImGui::End();
                        gui.imgui.EndFrame();
                    }
                    const auto *draw = ImGui::GetDrawData();
                    REQUIRE(draw);
                    CHECK(draw->TotalVtxCount > 0);
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
        const auto overlay = RefreshPreview(fixture);
        REQUIRE(overlay->candidate.Outputs().size() == 1);
        CHECK(overlay->candidate.Outputs().front().points->PointCount() == 256);
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

    TEST_CASE("PCG authoring Apply and history revoke previews while drafts never enter evaluator capture",
              "[unit][editor][pcg][preview][authoring][workspace]") {
        CanvasWorkspace fixture{true};
        const auto before = fixture.View().snapshot;
        const auto &schemas = fixture.controller->ViewModel().pcgSchemas;
        auto draft = PCGGraphAuthoringDraft::Capture(before, schemas).Value();
        draft.values.deterministicSeed = before.source->Data().deterministicSeed + 1;
        const auto initial = RefreshPreview(fixture);
        CHECK(initial->candidate.Seed() == before.source->Data().deterministicSeed);
        CHECK(initial->candidate.Catalog().Generation() == schemas.Generation());
        fixture.Unchanged(before);  // A detached property draft is never a preview source.
        auto apply = fixture.Request(PCGEditorAction::ApplyProperties);
        apply.authoring = draft;
        fixture.Send(apply);
        REQUIRE_FALSE(fixture.View().error);
        REQUIRE_FALSE(fixture.View().preview.overlay);
        CHECK(fixture.View().preview.phase == PCGPreviewPhase::Cancelled);
        const auto applied = fixture.View().snapshot;
        CHECK(applied.revision == before.revision + 1);
        CHECK(RefreshPreview(fixture)->candidate.Seed() == draft.values.deterministicSeed);
        fixture.Send(fixture.Request(PCGEditorAction::Undo));
        REQUIRE_FALSE(fixture.View().error);
        REQUIRE_FALSE(fixture.View().preview.overlay);
        CHECK(fixture.View().snapshot.source->Data() == before.source->Data());
        CHECK(RefreshPreview(fixture)->candidate.Seed() == before.source->Data().deterministicSeed);
        fixture.Send(fixture.Request(PCGEditorAction::Redo));
        REQUIRE_FALSE(fixture.View().error);
        REQUIRE_FALSE(fixture.View().preview.overlay);
        CHECK(fixture.View().snapshot.source->Data() == applied.source->Data());
        const auto current = RefreshPreview(fixture);
        CHECK(current->candidate.Seed() == draft.values.deterministicSeed);
        auto cancelled = fixture.Request(PCGEditorAction::ApplyProperties);
        cancelled.authoring = PCGGraphAuthoringDraft::Capture(fixture.View().snapshot, schemas).Value();
        ++cancelled.authoring->values.deterministicSeed;
        CancellationSource cancellation;
        cancelled.fence.cancellation = cancellation.Token();
        cancellation.RequestCancellation();
        fixture.Send(cancelled);
        REQUIRE(fixture.View().error);
        CHECK(fixture.View().preview.overlay == current);
        const auto source = fixture.View().snapshot;
        fixture.Send(fixture.Request(PCGEditorAction::PreviewCancel));
        REQUIRE_FALSE(fixture.View().error);
        CHECK_FALSE(fixture.View().preview.overlay);
        CHECK(fixture.View().preview.phase == PCGPreviewPhase::Cancelled);
        fixture.Unchanged(source);
    }

    TEST_CASE("Validation-only PCG composition keeps authoring available but rejects preview evaluation",
              "[unit][editor][pcg][preview][capability]") {
        CanvasWorkspace fixture{true, false};
        const auto before = fixture.View().snapshot;
        REQUIRE(fixture.controller->ViewModel().pcgSchemas.IsValid());
        CHECK(fixture.controller->ViewModel().pcgSchemas.Capabilities().granted.Contains(PCG::PCGCapability::Validation));
        CHECK_FALSE(fixture.controller->ViewModel().pcgSchemas.Capabilities().granted.Contains(PCG::PCGCapability::EditorPreview));
        CHECK_FALSE(fixture.View().previewAvailable);
        auto draft = PCGGraphAuthoringDraft::Capture(before, fixture.controller->ViewModel().pcgSchemas).Value();
        ++draft.values.deterministicSeed;
        auto apply = fixture.Request(PCGEditorAction::ApplyProperties);
        apply.authoring = draft;
        fixture.Send(apply);
        REQUIRE_FALSE(fixture.View().error);
        const auto applied = fixture.View().snapshot;
        CHECK(applied.source->Data().deterministicSeed == draft.values.deterministicSeed);
        fixture.Send(fixture.Request(PCGEditorAction::PreviewRefresh));
        REQUIRE(fixture.View().error);
        CHECK(fixture.View().error->code.Value() == PCGDocumentError(PCGDocumentCommandFailureReason::Unauthorized).code.Value());
        CHECK_FALSE(fixture.View().preview.overlay);
        fixture.Unchanged(applied);
    }

}  // namespace Horo::Editor
