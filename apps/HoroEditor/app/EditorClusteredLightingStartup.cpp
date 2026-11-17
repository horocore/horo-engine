#include "EditorClusteredLightingStartup.h"

#include "EditorClusteredLightingAssets.h"
#include "editor/renderer/metal/EditorViewportRendererMetal.h"

namespace Horo::Editor {
    Result<Render::RenderLightPreparationReadiness> PreparePackagedMetalViewportLighting(EditorViewportRendererMetal &viewport,
                                                                                         Render::RenderFrontend &frontend,
                                                                                         const EditorViewportLightingProfile &profile,
                                                                                         const std::filesystem::path &assetRoot) {
        using Readiness = Result<Render::RenderLightPreparationReadiness>;
        if (profile.requested == Render::LightPreparationMode::BaselineForwardCpu)
            return Readiness::Success({});
        const auto &capabilities = frontend.Capabilities();
        if (!capabilities.support.features.Supports(Render::RenderCapability::LightCulling) || !capabilities.supportsMaterialRasterGraph)
            return Readiness::Success({});
        const auto present = HasPackagedMetalClusteredLightingInputs(assetRoot);
        if (present.HasError())
            return Readiness::Failure(present.ErrorValue());
        if (!present.Value())
            return Readiness::Success({});
        auto assets = LoadPackagedMetalClusteredLighting(assetRoot);
        if (assets.HasError())
            return Readiness::Failure(assets.ErrorValue());
        const auto family = assets.Value().ProgramViews();
        if (auto prepared = viewport.PrepareClusteredLighting(family, assets.Value().kernel, profile); prepared.HasError())
            return Readiness::Failure(prepared.ErrorValue());
        return Readiness::Success({true, true, true});
    }
}  // namespace Horo::Editor
