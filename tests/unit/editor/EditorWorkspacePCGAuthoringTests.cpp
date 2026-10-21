#include "PCGCanvasTestSupport.h"

#include <algorithm>

namespace Horo::Editor {
    using namespace PCGDocumentTestSupport;
    using PCGCanvasTestSupport::CanvasWorkspace;

    TEST_CASE("Registered PCG inspector submits policy input and node settings through one workspace transaction",
              "[editor][workspace][pcg][authoring]") {
        CanvasWorkspace workspace;
        const auto &palette = workspace.controller->ViewModel().pcgPalette;
        const auto recipe = std::ranges::find(palette, PCG::PCGCpuNodeKind::GridJitter, &PCGPaletteEntry::kind);
        REQUIRE(recipe != palette.end());
        auto request = workspace.Request(PCGEditorAction::CreateNode);
        request.type = recipe->prototype.type;
        workspace.Send(request);
        REQUIRE_FALSE(workspace.View().error);
        const auto before = workspace.View().snapshot;
        const auto &schemas = workspace.controller->ViewModel().pcgSchemas;
        auto draft = PCGGraphAuthoringDraft::Capture(before, schemas).Value();
        draft.values.deterministicSeed = 0;
        draft.values.exposedInputs[0].defaultValue = 0.0;
        const auto property = std::ranges::find_if(draft.properties, [](const auto &value) {
            return value.settings.has_value();
        });
        REQUIRE(property != draft.properties.end());
        std::get<PCG::PCGJitterGenerationSettings>(*property->settings).fraction = 0;
        request = workspace.Request(PCGEditorAction::ApplyProperties);
        request.authoring = draft;
        workspace.Send(request);
        REQUIRE_FALSE(workspace.View().error);
        const auto after = workspace.View().snapshot;
        CHECK(after.revision == before.revision + 1);
        CHECK(after.dirty);
        CHECK(after.source->Data().deterministicSeed == 0);
        CHECK(std::get<double>(after.source->Data().exposedInputs[0].defaultValue) == 0);
        workspace.Send(workspace.Request(PCGEditorAction::Undo));
        REQUIRE_FALSE(workspace.View().error);
        CHECK(workspace.View().snapshot.source->Data() == before.source->Data());
        workspace.Send(workspace.Request(PCGEditorAction::Redo));
        REQUIRE_FALSE(workspace.View().error);
        CHECK(workspace.View().snapshot.source->Data() == after.source->Data());
    }

    TEST_CASE("Workspace PCG property apply rejects unauthorized unsupported and stale intent without mutation",
              "[editor][workspace][pcg][authoring]") {
        CanvasWorkspace workspace;
        const auto before = workspace.View().snapshot;
        const auto &schemas = workspace.controller->ViewModel().pcgSchemas;
        auto draft = PCGGraphAuthoringDraft::Capture(before, schemas).Value();
        draft.values.deterministicSeed = 0;
        auto request = workspace.Request(PCGEditorAction::ApplyProperties);
        request.authoring = draft;
        request.fence.canEdit = false;
        workspace.Send(request);
        REQUIRE(workspace.View().error);
        workspace.Unchanged(before);
        request = workspace.Request(PCGEditorAction::ApplyProperties);
        request.authoring = draft;
        request.authoring->values.mode = PCG::PCGGenerationMode::Runtime;
        workspace.Send(request);
        REQUIRE(workspace.View().error);
        workspace.Unchanged(before);
        request = workspace.Request(PCGEditorAction::ApplyProperties);
        request.authoring = draft;
        ++request.authoring->executableCatalogGeneration;
        workspace.Send(request);
        REQUIRE(workspace.View().error);
        workspace.Unchanged(before);
        request = workspace.Request(PCGEditorAction::ApplyProperties);
        request.authoring = draft;
        REQUIRE(workspace.controller->ExecutePCGDocument(before.identity.instance, Edit(before, 42)).HasValue());
        const auto changed = workspace.View().snapshot;
        workspace.Send(request);
        REQUIRE(workspace.View().error);
        workspace.Unchanged(changed);
        REQUIRE(workspace.controller->ClosePCGDocument(changed.identity.instance, true).HasValue());
        workspace.Open();
        const auto reopened = workspace.View().snapshot;
        workspace.Send(request);
        REQUIRE(workspace.View().error);
        workspace.Unchanged(reopened);
    }
}  // namespace Horo::Editor
