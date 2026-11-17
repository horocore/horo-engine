#include "EditorColorPipelineStartup.h"

#include "ColorCreativeLookPackage.h"
#include "ColorOutputPackage.h"
#include "editor/renderer/EditorViewportRenderer.h"

namespace Horo::Editor {
    /** @copydoc ResolveEditorColorPipelineRoot */
    Result<std::optional<std::filesystem::path>> ResolveEditorColorPipelineRoot(const std::optional<std::filesystem::path> &explicitRoot,
                                                                                const std::filesystem::path &packagedAssets) {
        using Output = Result<std::optional<std::filesystem::path>>;
        const auto root = explicitRoot.value_or(packagedAssets / "color-pipeline");
        if (!explicitRoot) {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(root, error);
            if (error == std::errc::no_such_file_or_directory || status.type() == std::filesystem::file_type::not_found)
                return Output::Success(std::nullopt);
            if (error)
                return Output::Failure(MakeError(Render::ExposureGpuErrors::InvalidInput));
        }
        const auto resolved = Render::Internal::ResolveColorPackageRoot(root);
        if (resolved.HasError())
            return Output::Failure(resolved.ErrorValue());
        return Output::Success(resolved.Value());
    }

    /** @copydoc PrepareEditorColorPipeline */
    Result<void> PrepareEditorColorPipeline(IEditorViewportRenderer &viewport, const std::filesystem::path &root,
                                            const Render::ExposureSettings &settings,
                                            const std::optional<std::filesystem::path> &lookRoot) {
        auto color = Render::Internal::LoadColorOutputCookPackage(root / "color");
        if (color.HasError())
            return Result<void>::Failure(color.ErrorValue());
        auto exposure = Render::Internal::LoadExposureGpuCookPackage(root / "exposure");
        if (exposure.HasError())
            return Result<void>::Failure(exposure.ErrorValue());
        EditorViewportColorInputs inputs{std::make_shared<const Render::Internal::ColorOutputCookPackage>(std::move(color).Value()),
                                         std::move(exposure).Value()};
        if (lookRoot) {
            const auto look = Render::Internal::LoadColorCreativeLookPackage(*lookRoot);
            if (look.HasError())
                return Result<void>::Failure(look.ErrorValue());
            inputs.lookMode = Render::ColorLookMode::Enabled;
            inputs.look = look.Value().look;
        }
        if (const auto initialized = viewport.PrepareColorPipeline(inputs); initialized.HasError())
            return initialized;
        return viewport.RequestColorSettings(settings);
    }
}  // namespace Horo::Editor
